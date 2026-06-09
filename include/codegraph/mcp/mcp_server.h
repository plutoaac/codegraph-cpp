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
 *
 * 缓存机制：
 *   - 查询结果缓存（LRU，30s TTL）
 *   - 索引更新时自动失效（generation 机制）
 *   - 缓存键格式：tool_name:query_json
 */

#pragma once

#include "codegraph/context/context_builder.h"
#include "codegraph/core/lru_cache.h"
#include "codegraph/db/database.h"
#include "codegraph/graph/traverser.h"
#include <nlohmann/json.hpp>
#include <string>

namespace codegraph {

class McpServer {
public:
    McpServer(Database& db, GraphTraverser& traverser, ContextBuilder& context);

    /**
     * 主循环：从 stdin 读取 JSON-RPC 请求，处理后输出到 stdout。
     * 阻塞运行直到 stdin 关闭或进程被终止。
     */
    void run();

    /**
     * 使缓存失效。
     * 当索引更新时调用，确保下次查询返回最新数据。
     */
    void invalidate_cache();

private:
    Database& db_;
    GraphTraverser& traverser_;
    ContextBuilder& context_;

    // 查询结果缓存（LRU，30s TTL，100 条上限）
    LruCache<std::string, std::string> cache_{100, 30};

    // 索引时间戳（用于检测索引更新）
    int64_t last_index_timestamp_ = 0;

    /**
     * 检查索引是否更新，如果是则失效缓存。
     * 读取 .codegraph/index_timestamp 文件，与上次记录的时间戳比较。
     */
    void check_index_update();

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
