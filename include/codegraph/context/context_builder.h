/**
 * context_builder.h — 上下文构建器接口
 *
 * 将数据库查询和图遍历组合为 AI Agent 可直接使用的 JSON 上下文。
 * 是 MCP 工具和 CLI 命令的业务逻辑层。
 *
 * 核心功能：
 *   - build_context(): 符号的完整上下文（定义 + callers + callees + methods）
 *   - search_symbols(): 符号搜索
 *   - get_callers/callees/impact(): 图遍历的 JSON 封装
 *   - get_status(): 索引统计
 *
 * 设计要点：
 *   - JSON 输出精简（kind/name/file/line/signature），减少 AI token 消耗
 *   - 多候选选择：同名符号选行范围最大的（定义 vs 前向声明）
 *   - 类/结构体特殊处理：聚合所有方法的 callers/callees
 */

#pragma once

#include "codegraph/db/database.h"
#include "codegraph/graph/traverser.h"
#include <nlohmann/json.hpp>

namespace codegraph {

class ContextBuilder {
public:
    ContextBuilder(Database& db, GraphTraverser& traverser);

    /**
     * 构建符号的完整上下文。
     *
     * 返回 JSON 包含：
     *   - symbol: 符号定义信息
     *   - callers: 调用者列表
     *   - callees: 被调用者列表
     *   - edges: 调用关系边
     *   - methods: （类/结构体）方法列表
     *
     * @param symbol 符号名（支持部分匹配）
     * @param limit 每类结果的最大数量
     * @param max_depth 图遍历深度
     */
    nlohmann::json build_context(const std::string& symbol, int limit = 10, int max_depth = 3);

    /** 符号搜索（FTS5 全文搜索）。 */
    nlohmann::json search_symbols(const std::string& query, int limit = 20);

    /** 查找谁调用了某符号。 */
    nlohmann::json get_callers(const std::string& symbol, int max_depth = 3);

    /** 查找某符号调用了谁。 */
    nlohmann::json get_callees(const std::string& symbol, int max_depth = 3);

    /** 影响分析。 */
    nlohmann::json get_impact(const std::string& symbol, int max_depth = 5);

    /** 索引统计信息。 */
    nlohmann::json get_status();

private:
    Database& db_;
    GraphTraverser& traverser_;

    /** Node → 精简 JSON（kind/name/file/line/signature）。 */
    nlohmann::json node_to_json(const Node& node);

    /** Edge → 精简 JSON（src/dst/kind）。 */
    nlohmann::json edge_to_json(const Edge& edge);
};

}  // namespace codegraph
