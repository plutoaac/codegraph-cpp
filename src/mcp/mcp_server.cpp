/**
 * mcp_server.cpp — MCP（Model Context Protocol）服务器实现
 *
 * 本文件实现了 JSON-RPC 2.0 协议的 MCP 服务器，让 AI Agent
 * 可以通过标准协议调用 codegraph 的代码分析能力。
 *
 * MCP 协议简介：
 *   - 基于 JSON-RPC 2.0（stdin/stdout 通信）
 *   - AI Agent 发送请求（如 tools/call），服务器返回结果
 *   - 支持工具发现（tools/list）、初始化握手（initialize）
 *
 * 暴露的 MCP 工具（共 10 个）：
 *   1. codegraph_search         — 符号搜索
 *   2. codegraph_context        — 符号上下文（定义 + callers + callees）
 *   3. codegraph_callers        — 查找谁调用了某符号
 *   4. codegraph_callees        — 查找某符号调用了谁
 *   5. codegraph_impact         — 影响分析
 *   6. codegraph_node           — 符号详情
 *   7. codegraph_status         — 索引统计
 *   8. codegraph_files          — 已索引文件列表
 *   9. codegraph_search_semantic — 语义搜索（需要 Python embed）
 *  10. codegraph_change_impact  — git diff 影响分析
 *
 * 通信流程：
 *   AI Agent → stdin → McpServer::run() → handle_request() → stdout → AI Agent
 */

#include "codegraph/mcp/mcp_server.h"
#include "codegraph/diff/diff_parser.h"
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>

namespace codegraph {

McpServer::McpServer(Database& db, GraphTraverser& traverser, ContextBuilder& context)
    : db_(db), traverser_(traverser), context_(context) {}

/**
 * 主循环：从 stdin 逐行读取 JSON-RPC 请求，处理后输出到 stdout。
 *
 * 协议格式：
 *   请求：{"jsonrpc":"2.0","method":"tools/call","params":{...},"id":1}
 *   响应：{"jsonrpc":"2.0","result":{...},"id":1}
 *   错误：{"jsonrpc":"2.0","error":{"code":-32700,"message":"..."},"id":null}
 *
 * JSON-RPC 通知（无 id 字段）：
 *   如 notifications/initialized，不需要返回响应。
 *   通过 __notification__ 标记跳过输出。
 */
void McpServer::run() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        nlohmann::json response;
        try {
            auto request = nlohmann::json::parse(line);
            response = handle_request(request);
        } catch (const std::exception& e) {
            response = {
                {"jsonrpc", "2.0"},
                {"error", {{"code", -32700}, {"message", e.what()}}},
                {"id", nullptr}
            };
        }

        // JSON-RPC 通知（无 id）不返回响应
        if (response.contains("__notification__")) continue;

        std::cout << response.dump() << std::endl;
    }
}

/**
 * 请求路由：根据 method 字段分发到对应的处理函数。
 *
 * 支持的 method：
 *   - initialize: 协议握手，返回服务器信息和能力
 *   - tools/list: 返回可用工具列表
 *   - tools/call: 调用指定工具
 *   - notifications/initialized: 客户端初始化完成通知（无响应）
 */
nlohmann::json McpServer::handle_request(const nlohmann::json& request) {
    std::string method = request.value("method", "");
    auto id = request.contains("id") ? request["id"] : nlohmann::json(nullptr);
    auto params = request.value("params", nlohmann::json::object());

    nlohmann::json result;

    if (method == "initialize") {
        result = handle_initialize(params);
    } else if (method == "tools/list") {
        result = handle_tools_list();
    } else if (method == "tools/call") {
        result = handle_tools_call(params);
    } else if (method == "notifications/initialized") {
        // JSON-RPC 通知：不返回响应
        return {{"__notification__", true}};
    } else {
        return {
            {"jsonrpc", "2.0"},
            {"error", {{"code", -32601}, {"message", "Method not found: " + method}}},
            {"id", id}
        };
    }

    return {
        {"jsonrpc", "2.0"},
        {"result", result},
        {"id", id}
    };
}

/**
 * 处理 initialize 请求：返回协议版本和服务器能力。
 * 这是 MCP 握手的第一步，客户端确认服务器支持的功能。
 */
nlohmann::json McpServer::handle_initialize(const nlohmann::json&) {
    return {
        {"protocolVersion", "2024-11-05"},
        {"capabilities", {{"tools", nlohmann::json::object()}}},
        {"serverInfo", {{"name", "codegraph-cpp"}, {"version", "0.1.0"}}}
    };
}

/**
 * 返回可用工具列表。
 *
 * 每个工具包含：
 *   - name: 工具名（AI Agent 调用时使用）
 *   - description: 工具描述（AI Agent 用于理解工具用途）
 *   - inputSchema: 参数的 JSON Schema（AI Agent 用于构造参数）
 *
 * description 的设计要点：
 *   - 明确说明工具的用途和使用场景
 *   - 提示何时应该使用这个工具
 *   - 说明参数的含义和默认值
 */
nlohmann::json McpServer::handle_tools_list() {
    return {
        {"tools", nlohmann::json::array({
            {
                {"name", "codegraph_search"},
                {"description", "Search for code symbols by name or partial name. Returns matching functions, classes, methods, variables, etc. with their kind, file location, and signature. Results are ranked: functions/classes first, then variables, then imports. Use this to find a symbol before using codegraph_context or codegraph_callers."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"query", {{"type", "string"}, {"description", "Symbol name or search query"}}},
                        {"limit", {{"type", "integer"}, {"description", "Max results (default 20)"}}}
                    }},
                    {"required", nlohmann::json::array({"query"})}
                }}
            },
            {
                {"name", "codegraph_context"},
                {"description", "Get rich context for a symbol: its definition, callers (who calls it), callees (what it calls), and source location. This is the best first tool to call when you need to understand a function or class. Input: exact or partial symbol name."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"symbol", {{"type", "string"}, {"description", "Symbol name to get context for"}}},
                        {"limit", {{"type", "integer"}, {"description", "Max results (default 10)"}}},
                        {"max_depth", {{"type", "integer"}, {"description", "Max traversal depth for callers/callees (default 3)"}}}
                    }},
                    {"required", nlohmann::json::array({"symbol"})}
                }}
            },
            {
                {"name", "codegraph_callers"},
                {"description", "Find all callers of a function or method -- i.e., what code calls this symbol. Traverses the call graph backwards. Use max_depth to control how many levels of indirection to trace (default 3). Returns nodes and edges."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"symbol", {{"type", "string"}, {"description", "Symbol name"}}},
                        {"max_depth", {{"type", "integer"}, {"description", "Max traversal depth (default 3)"}}}
                    }},
                    {"required", nlohmann::json::array({"symbol"})}
                }}
            },
            {
                {"name", "codegraph_callees"},
                {"description", "Find all callees of a function or method -- i.e., what this symbol calls. Traverses the call graph forwards. Use max_depth to control depth (default 3). Useful for understanding dependencies of a function."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"symbol", {{"type", "string"}, {"description", "Symbol name"}}},
                        {"max_depth", {{"type", "integer"}, {"description", "Max traversal depth (default 3)"}}}
                    }},
                    {"required", nlohmann::json::array({"symbol"})}
                }}
            },
            {
                {"name", "codegraph_impact"},
                {"description", "Impact analysis: what would break if this symbol changes. Returns the blast radius by tracing both direct callers and reference edges. Use max_depth to control how far to search (default 5). Useful before refactoring."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"symbol", {{"type", "string"}, {"description", "Symbol name"}}},
                        {"max_depth", {{"type", "integer"}, {"description", "Max traversal depth (default 5)"}}}
                    }},
                    {"required", nlohmann::json::array({"symbol"})}
                }}
            },
            {
                {"name", "codegraph_node"},
                {"description", "Get detailed information for a single symbol: its signature, source location (file:line), docstring, and metadata (static, const, exported). Use this when you already know the symbol name and need its details."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"symbol", {{"type", "string"}, {"description", "Symbol name"}}}
                    }},
                    {"required", nlohmann::json::array({"symbol"})}
                }}
            },
            {
                {"name", "codegraph_status"},
                {"description", "Get index statistics: total number of indexed nodes (symbols), edges (relationships), and files. Use this to understand the scope of the indexed codebase."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", nlohmann::json::object()}
                }}
            },
            {
                {"name", "codegraph_files"},
                {"description", "List all indexed source files with their detected language. Use this to see what parts of the codebase have been indexed."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", nlohmann::json::object()}
                }}
            },
            {
                {"name", "codegraph_search_semantic"},
                {"description", "Semantic code search using natural language. Finds code by meaning, not just symbol name. Examples: 'handle connection error', 'serialize protobuf message', 'thread pool task submission'. Requires embeddings to be generated first with 'codegraph embed'."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"query", {{"type", "string"}, {"description", "Natural language description of what the code does"}}},
                        {"limit", {{"type", "integer"}, {"description", "Max results (default 10)"}}}
                    }},
                    {"required", nlohmann::json::array({"query"})}
                }}
            },
            {
                {"name", "codegraph_change_impact"},
                {"description", "Analyze impact of code changes: what symbols are affected, and what else would break. Use before committing or reviewing PRs. Input: git ref (e.g. HEAD~1, or empty for uncommitted changes)."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"ref", {{"type", "string"}, {"description", "Git ref to diff against (default: uncommitted changes)"}}}
                    }}
                }}
            }
        })}
    };
}

/**
 * 工具调用路由：根据工具名分发到对应的实现函数。
 */
nlohmann::json McpServer::handle_tools_call(const nlohmann::json& params) {
    std::string tool_name = params.value("name", "");
    auto args = params.value("arguments", nlohmann::json::object());

    if (tool_name == "codegraph_search") return tool_search(args);
    if (tool_name == "codegraph_context") return tool_context(args);
    if (tool_name == "codegraph_callers") return tool_callers(args);
    if (tool_name == "codegraph_callees") return tool_callees(args);
    if (tool_name == "codegraph_impact") return tool_impact(args);
    if (tool_name == "codegraph_node") return tool_node(args);
    if (tool_name == "codegraph_status") return tool_status(args);
    if (tool_name == "codegraph_files") return tool_files(args);
    if (tool_name == "codegraph_search_semantic") return tool_semantic_search(args);
    if (tool_name == "codegraph_change_impact") return tool_diff(args);

    return make_error("Unknown tool: " + tool_name);
}

// ── 工具实现 ──

/** 符号搜索：委托给 ContextBuilder::search_symbols()。 */
nlohmann::json McpServer::tool_search(const nlohmann::json& args) {
    std::string query = args.value("query", "");
    int limit = args.value("limit", 20);
    auto results = context_.search_symbols(query, limit);
    return make_result(results.dump());
}

/** 符号上下文：委托给 ContextBuilder::build_context()。 */
nlohmann::json McpServer::tool_context(const nlohmann::json& args) {
    std::string symbol = args.value("symbol", "");
    int limit = args.value("limit", 10);
    int max_depth = args.value("max_depth", 3);
    auto results = context_.build_context(symbol, limit, max_depth);
    return make_result(results.dump());
}

/** callers 查询。 */
nlohmann::json McpServer::tool_callers(const nlohmann::json& args) {
    std::string symbol = args.value("symbol", "");
    int max_depth = args.value("max_depth", 3);
    auto results = context_.get_callers(symbol, max_depth);
    return make_result(results.dump());
}

/** callees 查询。 */
nlohmann::json McpServer::tool_callees(const nlohmann::json& args) {
    std::string symbol = args.value("symbol", "");
    int max_depth = args.value("max_depth", 3);
    auto results = context_.get_callees(symbol, max_depth);
    return make_result(results.dump());
}

/** 影响分析。 */
nlohmann::json McpServer::tool_impact(const nlohmann::json& args) {
    std::string symbol = args.value("symbol", "");
    int max_depth = args.value("max_depth", 5);
    auto results = context_.get_impact(symbol, max_depth);
    return make_result(results.dump());
}

/**
 * 符号详情：返回单个符号的完整信息。
 * 与 context 不同，这里只返回符号本身，不遍历图。
 */
nlohmann::json McpServer::tool_node(const nlohmann::json& args) {
    std::string symbol = args.value("symbol", "");
    auto nodes = db_.find_nodes_by_name(symbol, 1);
    if (nodes.empty()) return make_error("Symbol not found: " + symbol);
    nlohmann::json node_json = {
        {"id", nodes[0].id},
        {"kind", node_kind_str(nodes[0].kind)},
        {"name", nodes[0].name},
        {"file", nodes[0].file_path},
        {"line", nodes[0].line},
        {"signature", nodes[0].signature}
    };
    return make_result(node_json.dump());
}

/** 索引统计。 */
nlohmann::json McpServer::tool_status(const nlohmann::json&) {
    return make_result(context_.get_status().dump());
}

/** 已索引文件列表。 */
nlohmann::json McpServer::tool_files(const nlohmann::json&) {
    auto files = db_.get_all_files();
    nlohmann::json result = nlohmann::json::array();
    for (auto& f : files) {
        result.push_back({{"path", f.path}, {"language", f.language}});
    }
    return make_result(result.dump());
}

/**
 * Git diff 影响分析。
 *
 * 流程：
 *   1. 执行 git diff 获取变更
 *   2. 解析 diff 输出，提取变更的文件和行范围
 *   3. 找到行范围内的受影响符号
 *   4. 对每个受影响符号运行影响分析
 *   5. 返回变更文件、直接受影响符号、间接受影响符号
 */
nlohmann::json McpServer::tool_diff(const nlohmann::json& args) {
    std::string ref = args.value("ref", "");
    std::string diff_output = run_git_diff(ref);
    if (diff_output.empty()) {
        return make_result("{\"message\": \"No changes found\"}");
    }

    auto hunks = parse_diff(diff_output);
    if (hunks.empty()) {
        return make_result("{\"message\": \"No changed lines found\"}");
    }

    // 收集受影响的节点
    std::unordered_map<int64_t, Node> affected_nodes;
    std::unordered_map<std::string, std::vector<DiffHunk>> file_hunks;
    for (auto& h : hunks) {
        file_hunks[h.file_path].push_back(h);
    }

    // 找到行范围内的符号
    for (auto& [file, hunks_in_file] : file_hunks) {
        auto nodes_in_file = db_.find_nodes_by_file(file);
        for (auto& node : nodes_in_file) {
            for (auto& hunk : hunks_in_file) {
                // 检查行范围重叠
                if (node.line <= hunk.line_end &&
                    (node.end_line >= hunk.line_start || node.end_line == 0)) {
                    affected_nodes[node.id] = node;
                    break;
                }
            }
        }
    }

    if (affected_nodes.empty()) {
        return make_result("{\"message\": \"No symbols affected by this diff\"}");
    }

    // 运行 impact 分析
    GraphTraverser traverser(db_);
    std::unordered_map<int64_t, Node> impact_nodes;
    for (auto& [id, node] : affected_nodes) {
        auto result = traverser.get_impact(id, 3);
        for (auto& n : result.nodes) {
            impact_nodes[n.id] = n;
        }
    }

    // 构建输出
    nlohmann::json output;
    output["changed_files"] = nlohmann::json::array();
    for (auto& [file, _] : file_hunks) {
        output["changed_files"].push_back(file);
    }

    output["affected_symbols"] = nlohmann::json::array();
    for (auto& [id, node] : affected_nodes) {
        output["affected_symbols"].push_back({
            {"kind", node_kind_str(node.kind)},
            {"name", node.name},
            {"file", node.file_path},
            {"line", node.line}
        });
    }

    output["impact"] = nlohmann::json::array();
    for (auto& [id, node] : impact_nodes) {
        if (affected_nodes.count(id)) continue;
        output["impact"].push_back({
            {"kind", node_kind_str(node.kind)},
            {"name", node.name},
            {"file", node.file_path},
            {"line", node.line}
        });
    }

    return make_result(output.dump());
}

/**
 * 语义搜索：调用 Python embed.py 脚本。
 *
 * 为什么用 fork+exec 而不是嵌入 Python：
 *   - 避免 C++ 项目依赖 Python 头文件
 *   - sentence-transformers 库只在需要时安装
 *   - 进程隔离，Python 崩溃不影响 C++ 服务器
 */
nlohmann::json McpServer::tool_semantic_search(const nlohmann::json& args) {
    std::string query = args.value("query", "");
    int limit = args.value("limit", 10);

    if (query.empty()) {
        return make_error("query is required");
    }

    std::string script_path = "scripts/embed.py";
    std::string db_path = ".codegraph/index";

    if (!std::filesystem::exists(db_path)) {
        return make_error("No .codegraph/index found. Run 'codegraph init' first.");
    }

    std::vector<std::string> argv_strs = {
        "python3", script_path, "query", db_path, query, std::to_string(limit)
    };

    std::vector<char*> argv;
    for (auto& s : argv_strs) {
        argv.push_back(const_cast<char*>(s.c_str()));
    }
    argv.push_back(nullptr);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return make_error("Failed to create pipe");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return make_error("Failed to fork");
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipefd[1]);
    std::string result;
    char buf[4096];
    while (true) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n > 0) { result.append(buf, n); continue; }
        if (n == 0) break;
        if (errno == EINTR) continue;
        break;
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if (result.empty()) {
        return make_error("Semantic search returned no results. Run 'codegraph embed' first to generate embeddings.");
    }

    return make_result(result);
}

/** 构造成功的 MCP 响应。 */
nlohmann::json McpServer::make_result(const std::string& text) {
    return {
        {"content", nlohmann::json::array({
            {{"type", "text"}, {"text", text}}
        })}
    };
}

/** 构造错误的 MCP 响应。 */
nlohmann::json McpServer::make_error(const std::string& message) {
    return {
        {"content", nlohmann::json::array({
            {{"type", "text"}, {"text", "Error: " + message}}
        })},
        {"isError", true}
    };
}

}  // namespace codegraph
