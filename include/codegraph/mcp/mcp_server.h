/**
 * mcp_server.h — MCP（Model Context Protocol）服务器接口
 *
 * 实现 JSON-RPC 2.0 协议的 MCP 服务器，让 AI Agent
 * 可以通过标准协议调用 codegraph 的代码分析能力。
 *
 * 通信方式：stdin/stdout（每行一个 JSON 对象）
 *
 * 暴露的 MCP 工具（共 10 个）：
 *   codegraph_search          — 符号搜索
 *   codegraph_context         — 符号上下文
 *   codegraph_callers         — 调用者
 *   codegraph_callees         — 被调用者
 *   codegraph_impact          — 影响分析
 *   codegraph_node            — 符号详情
 *   codegraph_status          — 索引统计
 *   codegraph_files           — 文件列表
 *   codegraph_search_semantic — 语义搜索
 *   codegraph_change_impact   — git diff 影响分析
 */

#pragma once

#include "codegraph/context/context_builder.h"
#include "codegraph/db/database.h"
#include "codegraph/graph/traverser.h"
#include <nlohmann/json.hpp>

namespace codegraph {

class McpServer {
public:
    McpServer(Database& db, GraphTraverser& traverser, ContextBuilder& context);

    /**
     * 主循环：从 stdin 读取 JSON-RPC 请求，处理后输出到 stdout。
     * 阻塞运行直到 stdin 关闭或进程被终止。
     */
    void run();

private:
    Database& db_;
    GraphTraverser& traverser_;
    ContextBuilder& context_;

    // ── JSON-RPC 路由 ──

    /** 请求路由：根据 method 分发到处理函数。 */
    nlohmann::json handle_request(const nlohmann::json& request);

    /** 处理 initialize 握手。 */
    nlohmann::json handle_initialize(const nlohmann::json& params);

    /** 返回可用工具列表。 */
    nlohmann::json handle_tools_list();

    /** 工具调用路由。 */
    nlohmann::json handle_tools_call(const nlohmann::json& params);

    // ── 工具实现 ──

    nlohmann::json tool_search(const nlohmann::json& args);
    nlohmann::json tool_context(const nlohmann::json& args);
    nlohmann::json tool_callers(const nlohmann::json& args);
    nlohmann::json tool_callees(const nlohmann::json& args);
    nlohmann::json tool_impact(const nlohmann::json& args);
    nlohmann::json tool_node(const nlohmann::json& args);
    nlohmann::json tool_status(const nlohmann::json& args);
    nlohmann::json tool_files(const nlohmann::json& args);
    nlohmann::json tool_diff(const nlohmann::json& args);
    nlohmann::json tool_semantic_search(const nlohmann::json& args);

    // ── 响应构造 ──

    /** 构造成功的 MCP 响应。 */
    nlohmann::json make_result(const std::string& text);

    /** 构造错误的 MCP 响应。 */
    nlohmann::json make_error(const std::string& message);
};

}  // namespace codegraph
