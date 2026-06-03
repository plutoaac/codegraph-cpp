/**
 * traverser.h — 图遍历算法接口
 *
 * 封装了调用图上的所有遍历和分析算法：
 *   - 正向/反向 BFS（callers/callees）
 *   - 影响分析（impact）
 *   - 路径查找（find_path/find_reverse_path）
 *   - Tarjan 强连通分量（循环依赖检测）
 *   - 图指标统计（入度/出度排名、调用深度）
 *   - 影响链（附带调用路径的影响分析）
 *
 * 使用模式：
 *   Database db(".codegraph/index");
 *   GraphTraverser traverser(db);
 *   auto callees = traverser.get_callees(node_id, 3);
 *   auto cycles = traverser.find_circular_dependencies();
 */

#pragma once

#include "codegraph/db/database.h"
#include <vector>

namespace codegraph {

/**
 * 图遍历结果。
 *
 * 包含遍历过程中访问到的所有节点和边。
 * nodes 和 edges 的顺序是 BFS 的访问顺序。
 */
struct TraversalResult {
    std::vector<Node> nodes;  // 访问到的节点
    std::vector<Edge> edges;  // 遍历经过的边
};

/**
 * 调用图指标统计结果。
 *
 * 用于 CLI 的 metrics 命令和 MCP 的 codegraph_status 工具。
 */
struct GraphMetrics {
    int total_nodes = 0;        // 函数/方法总数
    int total_edges = 0;        // 调用边总数
    int circular_deps = 0;      // 循环依赖 SCC 数
    int max_call_depth = 0;     // 最大调用深度（从入口点到叶子）
    double avg_call_depth = 0;  // 平均调用深度
    std::vector<std::pair<Node, int>> most_called;   // 入度 top N (node, caller_count)
    std::vector<std::pair<Node, int>> most_calling;  // 出度 top N (node, callee_count)
};

/**
 * 影响链节点。
 *
 * 在影响分析的基础上，为每个受影响节点附上调用路径。
 * 回答"为什么改 X 会影响 Y"：X → A → B → Y。
 */
struct ImpactNode {
    Node node;                     // 受影响的节点
    std::vector<int64_t> path;     // 从源节点到此节点的调用链 (node IDs)
};

/**
 * 图遍历器。
 *
 * 所有方法都接受 Database 引用，直接查询数据库。
 * 不缓存图结构（每次调用都重新查询），适合低频调用场景。
 */
class GraphTraverser {
public:
    GraphTraverser(Database& db);

    /**
     * 正向 BFS：找某函数调用了谁（callees）。
     *
     * 沿 Calls 边正向遍历，返回所有可达的被调用函数。
     *
     * @param node_id 起始节点 ID
     * @param max_depth 最大遍历深度（默认 3）
     * @return 遍历到的所有节点和边
     */
    TraversalResult get_callees(int64_t node_id, int max_depth = 3);

    /**
     * 反向 BFS：找谁调用了某函数（callers）。
     *
     * 沿 Calls 边反向遍历，返回所有调用了该函数的函数。
     *
     * @param node_id 起始节点 ID
     * @param max_depth 最大遍历深度（默认 3）
     * @return 遍历到的所有节点和边
     */
    TraversalResult get_callers(int64_t node_id, int max_depth = 3);

    /**
     * 影响分析：修改此节点会影响哪些节点。
     *
     * 与 get_callers 的区别：
     *   - get_callers 只看 Calls 边
     *   - get_impact 同时看 Calls 和 References 边
     *
     * References 边包含函数指针、回调、模板实例化等非直接调用的依赖。
     *
     * @param node_id 起始节点 ID
     * @param max_depth 最大遍历深度（默认 5）
     * @return 遍历到的所有节点和边
     */
    TraversalResult get_impact(int64_t node_id, int max_depth = 5);

    /**
     * BFS 找两点间最短调用路径（正向）。
     *
     * 沿 Calls 边正向查找从 from_id 到 to_id 的路径。
     * BFS 保证找到的是最短路径。
     *
     * @param from_id 起始节点
     * @param to_id 目标节点
     * @param max_depth 最大搜索深度
     * @return 路径上的节点 ID 列表（包含起始和目标），无路径返回空
     */
    std::vector<int64_t> find_path(int64_t from_id, int64_t to_id, int max_depth = 10);

    /**
     * BFS 找反向路径。
     *
     * 沿 Calls 边反向查找从 from_id 到 to_id 的路径。
     * 用于影响分析中的调用链回溯。
     *
     * 例：A → B → C，find_reverse_path(C, A) 返回 [C, B, A]
     *
     * @param from_id 起始节点
     * @param to_id 目标节点
     * @param max_depth 最大搜索深度
     * @return 路径上的节点 ID 列表，无路径返回空
     */
    std::vector<int64_t> find_reverse_path(int64_t from_id, int64_t to_id, int max_depth = 10);

    /**
     * 构建上下文：节点本身 + callers + callees。
     * 用于 MCP 的 codegraph_context 工具。
     */
    TraversalResult build_context(int64_t node_id, int max_depth = 2);

    /**
     * Tarjan 强连通分量算法。
     *
     * 在调用图中查找所有 SCC。大小 > 1 的 SCC 表示循环依赖。
     * 时间复杂度：O(V + E)
     *
     * @return 所有 SCC，每个 SCC 是一个节点 ID 列表
     */
    std::vector<std::vector<int64_t>> find_sccs();

    /**
     * 查找循环依赖：过滤出大小 > 1 的 SCC。
     */
    std::vector<std::vector<int64_t>> find_circular_dependencies();

    /**
     * 计算调用图指标。
     *
     * 统计内容：
     *   - 入度排名（被调用最多的函数）
     *   - 出度排名（调用最多的函数）
     *   - 调用深度（最大/平均）
     *   - 循环依赖数
     *
     * @param top_n 每个排名返回前 N 个
     * @return 指标统计结果
     */
    GraphMetrics compute_metrics(int top_n = 10);

    /**
     * 影响链分析。
     *
     * 在影响分析的基础上，为每个受影响节点附上调用路径。
     * 回答"为什么改 X 会影响 Y"。
     *
     * @param node_id 起始节点 ID
     * @param max_depth 最大遍历深度
     * @return 受影响节点列表，每个附带调用路径
     */
    std::vector<ImpactNode> get_impact_chain(int64_t node_id, int max_depth = 5);

private:
    Database& db_;
};

}  // namespace codegraph
