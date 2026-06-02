#pragma once

#include "codegraph/db/database.h"
#include <vector>

namespace codegraph {

struct TraversalResult {
    std::vector<Node> nodes;
    std::vector<Edge> edges;
};

// 调用图指标统计
struct GraphMetrics {
    int total_nodes = 0;        // 函数/方法总数
    int total_edges = 0;        // 调用边总数
    int circular_deps = 0;      // 循环依赖 SCC 数
    int max_call_depth = 0;     // 最大调用深度
    double avg_call_depth = 0;  // 平均调用深度
    std::vector<std::pair<Node, int>> most_called;   // 入度 top N (node, caller_count)
    std::vector<std::pair<Node, int>> most_calling;  // 出度 top N (node, callee_count)
};

// 影响链：受影响的节点 + 从源到此节点的调用路径
struct ImpactNode {
    Node node;
    std::vector<int64_t> path;  // 从源节点到此节点的调用链 (node IDs)
};

class GraphTraverser {
public:
    GraphTraverser(Database& db);

    // 正向 BFS：找所有被调用者
    TraversalResult get_callees(int64_t node_id, int max_depth = 3);

    // 反向 BFS：找所有调用者
    TraversalResult get_callers(int64_t node_id, int max_depth = 3);

    // 影响分析：修改此节点会影响哪些节点
    TraversalResult get_impact(int64_t node_id, int max_depth = 5);

    // BFS 找两点间最短调用路径（正向：from 调用 to）
    std::vector<int64_t> find_path(int64_t from_id, int64_t to_id, int max_depth = 10);

    // BFS 找反向路径（to 被 from 调用，即 from → ... → to 的路径）
    std::vector<int64_t> find_reverse_path(int64_t from_id, int64_t to_id, int max_depth = 10);

    // 构建上下文：callers + callees
    TraversalResult build_context(int64_t node_id, int max_depth = 2);

    // Tarjan 强连通分量：找调用图中的循环依赖
    std::vector<std::vector<int64_t>> find_sccs();
    std::vector<std::vector<int64_t>> find_circular_dependencies();

    // 调用图指标：入度/出度排名、深度统计、循环依赖数
    GraphMetrics compute_metrics(int top_n = 10);

    // 影响链：返回每个受影响节点的调用路径（不只是节点集合）
    std::vector<ImpactNode> get_impact_chain(int64_t node_id, int max_depth = 5);

private:
    Database& db_;
};

}  // namespace codegraph
