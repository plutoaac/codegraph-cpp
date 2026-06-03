/**
 * context_builder.cpp — 上下文构建器实现
 *
 * 本文件是 MCP 工具和 CLI 命令的业务逻辑层，负责将数据库查询和图遍历
 * 组合为 AI Agent 可以直接使用的结构化上下文。
 *
 * 核心功能：
 *   1. search_symbols() — 符号搜索，返回匹配的节点列表
 *   2. build_context()  — 构建符号的完整上下文（定义 + callers + callees + methods）
 *   3. get_callers()    — 查找谁调用了某符号
 *   4. get_callees()    — 查找某符号调用了谁
 *   5. get_impact()     — 影响分析
 *   6. get_status()     — 索引统计信息
 *
 * 设计要点：
 *   - JSON 输出精简（只保留 kind/name/file/line/signature），减少 token 消耗
 *   - 多候选选择：同名符号存在多个定义时，选择行范围最大的（定义 vs 前向声明）
 *   - 类/结构体特殊处理：聚合所有方法的 callers/callees
 *   - pick_best_node() 统一处理多候选选择逻辑
 */

#include "codegraph/context/context_builder.h"
#include <unordered_set>

namespace codegraph {

ContextBuilder::ContextBuilder(Database& db, GraphTraverser& traverser)
    : db_(db), traverser_(traverser) {}

/**
 * 将 Node 转为精简 JSON。
 *
 * 只保留 AI Agent 需要的 5 个字段：
 *   - kind: 节点类型（function/class/variable 等）
 *   - name: 符号名
 *   - file: 所在文件路径
 *   - line: 行号
 *   - signature: 函数签名（可选，只在非空时包含）
 *
 * 为什么不返回全部 16 个字段：
 *   AI Agent 的上下文窗口有限，精简输出可以塞进更多有用信息。
 *   docstring、visibility、is_static 等字段对 Agent 的帮助不大。
 */
nlohmann::json ContextBuilder::node_to_json(const Node& node) {
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

/**
 * 将 Edge 转为精简 JSON。
 * 只保留 source→target+kind，省略 id、line、col 等元数据。
 */
nlohmann::json ContextBuilder::edge_to_json(const Edge& edge) {
    return {
        {"src", edge.source_id},
        {"dst", edge.target_id},
        {"kind", edge_kind_str(edge.kind)}
    };
}

/**
 * 符号搜索：委托给数据库的 FTS5 全文搜索。
 */
nlohmann::json ContextBuilder::search_symbols(const std::string& query, int limit) {
    auto nodes = db_.search_fts(query, limit);
    nlohmann::json result = nlohmann::json::array();
    for (auto& n : nodes) {
        result.push_back(node_to_json(n));
    }
    return result;
}

/**
 * 构建符号的完整上下文。
 *
 * 这是 MCP codegraph_context 工具的核心实现，返回：
 *   - symbol: 符号本身的定义信息
 *   - callers: 谁调用了它
 *   - callees: 它调用了谁
 *   - edges: 调用关系边
 *   - methods: （类/结构体）所有方法列表
 *
 * 多候选选择策略：
 *   当同名符号存在多个定义时（如前向声明 vs 完整定义），
 *   选择行范围最大的那个（定义通常跨越多行，前向声明只有 1-2 行）。
 *
 * 类/结构体特殊处理：
 *   类本身没有 call edges（调用发生在方法上），所以需要：
 *   1. 找到类的所有方法（在同一文件和对应的 .cpp 文件中）
 *   2. 聚合所有方法的 callers 和 callees
 *   3. 去重后返回
 *
 * 对应文件查找：
 *   如果类定义在 .h 文件中，还会查找对应的 .cpp 文件中的方法实现。
 *   支持的头文件扩展名：.h/.hpp/.hxx/.hh
 */
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

    // ── 类/结构体特殊处理 ──
    // 类本身没有 call edges，需要聚合所有方法的 callers/callees
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

        // 如果类在头文件中，还要查找对应的源文件
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

        // 聚合所有方法的 callers 和 callees
        std::unordered_map<int64_t, Node> caller_nodes, callee_nodes;
        std::unordered_set<int64_t> seen_edge_ids;
        nlohmann::json edges_json = nlohmann::json::array();

        for (auto& method : class_methods) {
            // 限制处理的方法数，避免大类输出过多
            if (caller_nodes.size() >= static_cast<size_t>(limit) &&
                callee_nodes.size() >= static_cast<size_t>(limit)) {
                break;
            }

            auto callers = traverser_.get_callers(method.id, max_depth);
            auto callees = traverser_.get_callees(method.id, max_depth);

            for (auto& n : callers.nodes) caller_nodes[n.id] = n;
            for (auto& n : callees.nodes) callee_nodes[n.id] = n;

            // 边去重（callers 和 callees 可能共享边）
            auto add_edge = [&](const Edge& e) {
                if (seen_edge_ids.insert(e.id).second) {
                    edges_json.push_back(edge_to_json(e));
                }
            };
            for (auto& e : callers.edges) add_edge(e);
            for (auto& e : callees.edges) add_edge(e);
        }

        // 应用 limit
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
        // ── 函数/方法：直接查找 callers 和 callees ──
        auto callers = traverser_.get_callers(target.id, max_depth);
        auto callees = traverser_.get_callees(target.id, max_depth);

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

        // 边去重
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

/**
 * 从多个候选节点中选择最佳的一个。
 *
 * 策略：选择行范围最大的（end_line - line）。
 * 原因：定义通常跨越多行（函数实现、类定义），
 *       前向声明只有 1-2 行。
 */
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

/**
 * 查找谁调用了某符号。
 * 流程：按名字查找节点 → 选择最佳候选 → 调用图遍历。
 */
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

/**
 * 查找某符号调用了谁。
 */
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

/**
 * 影响分析：改某符号会影响谁。
 */
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

/**
 * 获取索引统计信息。
 */
nlohmann::json ContextBuilder::get_status() {
    return {
        {"node_count", db_.count_nodes()},
        {"edge_count", db_.count_edges()},
        {"file_count", db_.count_files()}
    };
}

}  // namespace codegraph
