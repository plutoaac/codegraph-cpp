#include "codegraph/context/context_builder.h"
#include <unordered_set>

namespace codegraph {

ContextBuilder::ContextBuilder(Database& db, GraphTraverser& traverser)
    : db_(db), traverser_(traverser) {}

nlohmann::json ContextBuilder::node_to_json(const Node& node) {
    // Compact: only 5 fields (AI needs kind+name+location+signature, not 16 fields)
    nlohmann::json j = {
        {"kind", node_kind_str(node.kind)},
        {"name", node.name},
        {"file", node.file_path},
        {"line", node.line}
    };
    if (!node.signature.empty()) {
        j["signature"] = node.signature;
    }
    return j;
}

nlohmann::json ContextBuilder::edge_to_json(const Edge& edge) {
    // Compact: skip id, only source→target+kind+line
    return {
        {"src", edge.source_id},
        {"dst", edge.target_id},
        {"kind", edge_kind_str(edge.kind)}
    };
}

nlohmann::json ContextBuilder::search_symbols(const std::string& query, int limit) {
    auto nodes = db_.search_fts(query, limit);
    nlohmann::json result = nlohmann::json::array();
    for (auto& n : nodes) {
        result.push_back(node_to_json(n));
    }
    return result;
}

// 构建符号的完整上下文：定义 + callers + callees + methods。
//
// 对于类/结构体：聚合所有方法的 callers/callees（不只是类本身的）。
// 对于函数：直接查找 callers 和 callees。
//
// 多候选选择策略：当同名符号存在多个定义时（如前向声明 vs 完整定义），
// 选择行范围最大的那个（定义通常跨越多行，前向声明只有 1-2 行）。
nlohmann::json ContextBuilder::build_context(const std::string& symbol, int limit, int max_depth) {
    // 先精确匹配，再 FTS 模糊搜索
    auto nodes = db_.find_nodes_by_name(symbol, 5);
    if (nodes.empty()) {
        nodes = db_.search_fts(symbol, 5);
    }
    if (nodes.empty()) {
        return {{"error", "Symbol not found: " + symbol}};
    }

    // 在多个候选中选择"定义"而非"前向声明"：
    // 定义通常跨越多行（类定义、函数实现），前向声明只有 1-2 行。
    // 选行范围最大的候选。
    int best_idx = 0;
    int best_span = 0;
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        int span = nodes[i].end_line - nodes[i].line;
        if (span > best_span) {
            best_span = span;
            best_idx = i;
        }
    }
    const auto& target = nodes[best_idx];
    nlohmann::json result;
    result["symbol"] = node_to_json(target);

    // 对于类/结构体：聚合所有方法的 callers/callees
    // 而不是只返回类本身的（类本身没有 call edges）
    if (target.kind == NodeKind::Class || target.kind == NodeKind::Struct) {
        // 在类所在文件和对应的 .cpp 文件中查找所有属于该类的方法
        std::vector<Node> class_methods;
        auto add_methods_from_file = [&](const std::string& path) {
            auto file_nodes = db_.find_nodes_by_file(path);
            for (auto& n : file_nodes) {
                if ((n.kind == NodeKind::Method || n.kind == NodeKind::Function) &&
                    n.qualified_name.find(target.name + "::") != std::string::npos) {
                    class_methods.push_back(n);
                }
            }
        };

        add_methods_from_file(target.file_path);

        // Also search corresponding source file if class is in a header
        auto try_add_source = [&](const std::string& header_ext, const std::string& source_ext) {
            if (target.file_path.size() >= header_ext.size() &&
                target.file_path.substr(target.file_path.size() - header_ext.size()) == header_ext) {
                std::string source_file = target.file_path.substr(0, target.file_path.size() - header_ext.size()) + source_ext;
                add_methods_from_file(source_file);
            }
        };
        try_add_source(".h", ".cpp");
        try_add_source(".hpp", ".cpp");
        try_add_source(".hxx", ".cxx");
        try_add_source(".hh", ".cc");

        // Aggregate callers and callees from all methods
        std::unordered_map<int64_t, Node> caller_nodes, callee_nodes;
        std::unordered_set<int64_t> seen_edge_ids;
        nlohmann::json edges_json = nlohmann::json::array();

        for (auto& method : class_methods) {
            // Limit methods to process (avoid huge output for large classes)
            if (caller_nodes.size() >= static_cast<size_t>(limit) &&
                callee_nodes.size() >= static_cast<size_t>(limit)) {
                break;
            }

            auto callers = traverser_.get_callers(method.id, max_depth);
            auto callees = traverser_.get_callees(method.id, max_depth);

            for (auto& n : callers.nodes) caller_nodes[n.id] = n;
            for (auto& n : callees.nodes) callee_nodes[n.id] = n;

            auto add_edge = [&](const Edge& e) {
                if (seen_edge_ids.insert(e.id).second) {
                    edges_json.push_back(edge_to_json(e));
                }
            };
            for (auto& e : callers.edges) add_edge(e);
            for (auto& e : callees.edges) add_edge(e);
        }

        // Apply limit to callers and callees
        result["callers"] = nlohmann::json::array();
        int caller_count = 0;
        for (auto& [id, n] : caller_nodes) {
            if (caller_count++ >= limit) break;
            result["callers"].push_back(node_to_json(n));
        }

        result["callees"] = nlohmann::json::array();
        int callee_count = 0;
        for (auto& [id, n] : callee_nodes) {
            if (callee_count++ >= limit) break;
            result["callees"].push_back(node_to_json(n));
        }

        result["edges"] = std::move(edges_json);
        result["methods"] = nlohmann::json::array();
        for (auto& m : class_methods) {
            result["methods"].push_back(node_to_json(m));
        }
    } else {
        // For functions/methods, get callers and callees directly
        auto callers = traverser_.get_callers(target.id, max_depth);
        auto callees = traverser_.get_callees(target.id, max_depth);

        // Apply limit to callers and callees
        result["callers"] = nlohmann::json::array();
        int caller_count = 0;
        for (auto& n : callers.nodes) {
            if (caller_count++ >= limit) break;
            result["callers"].push_back(node_to_json(n));
        }

        result["callees"] = nlohmann::json::array();
        int callee_count = 0;
        for (auto& n : callees.nodes) {
            if (callee_count++ >= limit) break;
            result["callees"].push_back(node_to_json(n));
        }

        // Deduplicate edges
        std::unordered_set<int64_t> seen_edge_ids;
        result["edges"] = nlohmann::json::array();
        auto add_edge = [&](const Edge& e) {
            if (seen_edge_ids.insert(e.id).second) {
                result["edges"].push_back(edge_to_json(e));
            }
        };
        for (auto& e : callers.edges) add_edge(e);
        for (auto& e : callees.edges) add_edge(e);
    }

    return result;
}

// Pick the best node from candidates: prefer definitions over forward declarations
// by selecting the one with the largest line span.
static const Node& pick_best_node(const std::vector<Node>& nodes) {
    int best_idx = 0;
    int best_span = 0;
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        int span = nodes[i].end_line - nodes[i].line;
        if (span > best_span) {
            best_span = span;
            best_idx = i;
        }
    }
    return nodes[best_idx];
}

nlohmann::json ContextBuilder::get_callers(const std::string& symbol, int max_depth) {
    auto nodes = db_.find_nodes_by_name(symbol, 5);
    if (nodes.empty()) {
        return {{"error", "Symbol not found: " + symbol}};
    }

    auto result = traverser_.get_callers(pick_best_node(nodes).id, max_depth);
    nlohmann::json json_result;
    json_result["nodes"] = nlohmann::json::array();
    for (auto& n : result.nodes) {
        json_result["nodes"].push_back(node_to_json(n));
    }
    json_result["edges"] = nlohmann::json::array();
    for (auto& e : result.edges) {
        json_result["edges"].push_back(edge_to_json(e));
    }
    return json_result;
}

nlohmann::json ContextBuilder::get_callees(const std::string& symbol, int max_depth) {
    auto nodes = db_.find_nodes_by_name(symbol, 5);
    if (nodes.empty()) {
        return {{"error", "Symbol not found: " + symbol}};
    }

    auto result = traverser_.get_callees(pick_best_node(nodes).id, max_depth);
    nlohmann::json json_result;
    json_result["nodes"] = nlohmann::json::array();
    for (auto& n : result.nodes) {
        json_result["nodes"].push_back(node_to_json(n));
    }
    json_result["edges"] = nlohmann::json::array();
    for (auto& e : result.edges) {
        json_result["edges"].push_back(edge_to_json(e));
    }
    return json_result;
}

nlohmann::json ContextBuilder::get_impact(const std::string& symbol, int max_depth) {
    auto nodes = db_.find_nodes_by_name(symbol, 5);
    if (nodes.empty()) {
        return {{"error", "Symbol not found: " + symbol}};
    }

    auto result = traverser_.get_impact(pick_best_node(nodes).id, max_depth);
    nlohmann::json json_result;
    json_result["nodes"] = nlohmann::json::array();
    for (auto& n : result.nodes) {
        json_result["nodes"].push_back(node_to_json(n));
    }
    json_result["edges"] = nlohmann::json::array();
    for (auto& e : result.edges) {
        json_result["edges"].push_back(edge_to_json(e));
    }
    return json_result;
}

nlohmann::json ContextBuilder::get_status() {
    return {
        {"node_count", db_.count_nodes()},
        {"edge_count", db_.count_edges()},
        {"file_count", db_.count_files()}
    };
}

}  // namespace codegraph
