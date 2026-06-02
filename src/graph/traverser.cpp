#include "codegraph/graph/traverser.h"
#include <functional>
#include <queue>
#include <stack>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>

namespace codegraph {

GraphTraverser::GraphTraverser(Database& db) : db_(db) {}

TraversalResult GraphTraverser::get_callees(int64_t node_id, int max_depth) {
    TraversalResult result;
    std::queue<std::pair<int64_t, int>> frontier;
    std::unordered_set<int64_t> visited;

    frontier.push({node_id, 0});
    visited.insert(node_id);

    while (!frontier.empty()) {
        auto [current_id, depth] = frontier.front();
        frontier.pop();

        if (depth >= max_depth) continue;

        auto edges = db_.get_edges_from(current_id, EdgeKind::Calls);

        // Batch collect target IDs
        std::vector<int64_t> target_ids;
        for (auto& edge : edges) {
            if (visited.count(edge.target_id)) continue;
            visited.insert(edge.target_id);
            target_ids.push_back(edge.target_id);
        }

        auto nodes_vec = db_.get_nodes_by_ids(target_ids);
        std::unordered_map<int64_t, Node> nodes_by_id;
        for (auto& n : nodes_vec) nodes_by_id[n.id] = n;

        for (auto& edge : edges) {
            auto it = nodes_by_id.find(edge.target_id);
            if (it != nodes_by_id.end()) {
                result.nodes.push_back(it->second);
                result.edges.push_back(edge);
                frontier.push({edge.target_id, depth + 1});
            }
        }
    }

    return result;
}

TraversalResult GraphTraverser::get_callers(int64_t node_id, int max_depth) {
    TraversalResult result;
    std::queue<std::pair<int64_t, int>> frontier;
    std::unordered_set<int64_t> visited;

    frontier.push({node_id, 0});
    visited.insert(node_id);

    while (!frontier.empty()) {
        auto [current_id, depth] = frontier.front();
        frontier.pop();

        if (depth >= max_depth) continue;

        auto edges = db_.get_edges_to(current_id, EdgeKind::Calls);

        // Batch collect source IDs
        std::vector<int64_t> source_ids;
        for (auto& edge : edges) {
            if (visited.count(edge.source_id)) continue;
            visited.insert(edge.source_id);
            source_ids.push_back(edge.source_id);
        }

        auto nodes_vec = db_.get_nodes_by_ids(source_ids);
        std::unordered_map<int64_t, Node> nodes_by_id;
        for (auto& n : nodes_vec) nodes_by_id[n.id] = n;

        for (auto& edge : edges) {
            auto it = nodes_by_id.find(edge.source_id);
            if (it != nodes_by_id.end()) {
                result.nodes.push_back(it->second);
                result.edges.push_back(edge);
                frontier.push({edge.source_id, depth + 1});
            }
        }
    }

    return result;
}

TraversalResult GraphTraverser::get_impact(int64_t node_id, int max_depth) {
    TraversalResult result;
    std::queue<std::pair<int64_t, int>> frontier;
    std::unordered_set<int64_t> visited;

    frontier.push({node_id, 0});
    visited.insert(node_id);

    while (!frontier.empty()) {
        auto [current_id, depth] = frontier.front();
        frontier.pop();

        if (depth >= max_depth) continue;

        // Check both Calls and References edges
        for (auto kind : {EdgeKind::Calls, EdgeKind::References}) {
            auto edges = db_.get_edges_to(current_id, kind);

            // Batch collect source IDs
            std::vector<int64_t> source_ids;
            for (auto& edge : edges) {
                if (visited.count(edge.source_id)) continue;
                visited.insert(edge.source_id);
                source_ids.push_back(edge.source_id);
            }

            auto nodes_vec = db_.get_nodes_by_ids(source_ids);
            std::unordered_map<int64_t, Node> nodes_by_id;
            for (auto& n : nodes_vec) nodes_by_id[n.id] = n;

            for (auto& edge : edges) {
                auto it = nodes_by_id.find(edge.source_id);
                if (it != nodes_by_id.end()) {
                    result.nodes.push_back(it->second);
                    result.edges.push_back(edge);
                    frontier.push({edge.source_id, depth + 1});
                }
            }
        }
    }

    return result;
}

std::vector<int64_t> GraphTraverser::find_path(int64_t from_id, int64_t to_id, int max_depth) {
    std::queue<std::vector<int64_t>> frontier;
    std::unordered_set<int64_t> visited;

    frontier.push({from_id});
    visited.insert(from_id);

    while (!frontier.empty()) {
        auto path = frontier.front();
        frontier.pop();

        int64_t current = path.back();
        if (current == to_id) return path;
        if ((int)path.size() > max_depth) continue;

        auto edges = db_.get_edges_from(current, EdgeKind::Calls);
        for (auto& edge : edges) {
            if (visited.count(edge.target_id)) continue;
            visited.insert(edge.target_id);
            auto new_path = path;
            new_path.push_back(edge.target_id);
            frontier.push(new_path);
        }
    }

    return {};  // No path found
}

// 反向路径查找：从 from_id 出发，沿着反向边（被调用方向）找 to_id。
// 用途：影响分析中，找"从受影响节点到源节点"的调用链。
// 例：A → B → C，find_reverse_path(C, A) 返回 [C, B, A]（C 被 B 调用，B 被 A 调用）
std::vector<int64_t> GraphTraverser::find_reverse_path(int64_t from_id, int64_t to_id, int max_depth) {
    std::queue<std::vector<int64_t>> frontier;
    std::unordered_set<int64_t> visited;

    frontier.push({from_id});
    visited.insert(from_id);

    while (!frontier.empty()) {
        auto path = frontier.front();
        frontier.pop();

        int64_t current = path.back();
        if (current == to_id) return path;
        if ((int)path.size() > max_depth) continue;

        // 反向：找谁调用了 current（get_edges_to）
        auto edges = db_.get_edges_to(current, EdgeKind::Calls);
        for (auto& edge : edges) {
            if (visited.count(edge.source_id)) continue;
            visited.insert(edge.source_id);
            auto new_path = path;
            new_path.push_back(edge.source_id);
            frontier.push(new_path);
        }
    }

    return {};
}

TraversalResult GraphTraverser::build_context(int64_t node_id, int max_depth) {
    TraversalResult result;

    // Add the node itself
    auto node = db_.get_node(node_id);
    if (node.has_value()) {
        result.nodes.push_back(*node);
    }

    // Add callers
    auto callers = get_callers(node_id, max_depth);
    result.nodes.insert(result.nodes.end(), callers.nodes.begin(), callers.nodes.end());
    result.edges.insert(result.edges.end(), callers.edges.begin(), callers.edges.end());

    // Add callees
    auto callees = get_callees(node_id, max_depth);
    result.nodes.insert(result.nodes.end(), callees.nodes.begin(), callees.nodes.end());
    result.edges.insert(result.edges.end(), callees.edges.begin(), callees.edges.end());

    return result;
}

// ── Tarjan 强连通分量算法 ──
//
// 在调用图中查找所有强连通分量（SCC）。
// SCC 是一个极大节点集合，其中每个节点都能到达其他所有节点。
// 大小 > 1 的 SCC 表示循环依赖（A 调用 B，B 调用 A）。
//
// 算法核心思路：
//   1. DFS 遍历调用图，给每个节点分配一个发现时间戳（index）
//   2. 维护 lowlink 值：从当前节点出发能到达的最小时间戳
//   3. 当一个节点的 lowlink == 自己的 index 时，它是 SCC 的根
//   4. 从栈中弹出直到根节点 → 这些节点构成一个 SCC
//
// 关键洞察：如果节点 v 能到达 DFS 栈中的祖先节点，
// 那么 v 和该祖先属于同一个 SCC。
//
// 时间复杂度：O(V + E)，空间复杂度：O(V)
//
std::vector<std::vector<int64_t>> GraphTraverser::find_sccs() {
    // 第一步：收集所有函数/方法节点，构建邻接表。
    // 只关注 Function 和 Method（不包括 Class、Variable 等），
    // 因为调用图中的循环才是有意义的模式。
    auto all_files = db_.get_all_files();
    std::unordered_set<int64_t> node_ids;
    std::unordered_map<int64_t, std::vector<int64_t>> adj;  // adjacency list

    for (const auto& file : all_files) {
        auto nodes = db_.find_nodes_by_file(file.path);
        for (const auto& n : nodes) {
            if (n.kind == NodeKind::Function || n.kind == NodeKind::Method) {
                node_ids.insert(n.id);
            }
        }
    }

    // 从调用边构建邻接表
    for (int64_t id : node_ids) {
        auto edges = db_.get_edges_from(id, EdgeKind::Calls);
        for (const auto& e : edges) {
            if (node_ids.count(e.target_id)) {
                adj[id].push_back(e.target_id);
            }
        }
    }

    // 第二步：Tarjan 算法主体
    std::vector<std::vector<int64_t>> sccs;          // 结果：所有 SCC
    std::stack<int64_t> tarjan_stack;                 // 当前 DFS 路径上的节点
    std::unordered_set<int64_t> on_stack;             // 快速判断节点是否在栈中
    std::unordered_map<int64_t, int> index_map;       // 节点 → 发现时间戳
    std::unordered_map<int64_t, int> lowlink_map;     // 节点 → lowlink 值
    int current_index = 0;

    // 递归 lambda：从节点 v 开始 DFS
    using Fn = std::function<void(int64_t)>;
    Fn strongconnect = [&](int64_t v) {
        // 给 v 分配时间戳，初始化 lowlink = 自己的 index
        index_map[v] = current_index;
        lowlink_map[v] = current_index;
        current_index++;
        tarjan_stack.push(v);
        on_stack.insert(v);

        // 遍历 v 的所有后继节点
        auto it = adj.find(v);
        if (it != adj.end()) {
            for (int64_t w : it->second) {
                if (index_map.find(w) == index_map.end()) {
                    // 后继 w 未访问：递归，然后用 w 的 lowlink 更新 v
                    strongconnect(w);
                    lowlink_map[v] = std::min(lowlink_map[v], lowlink_map[w]);
                } else if (on_stack.count(w)) {
                    // 后继 w 在栈中：说明 w 是 v 的祖先，v 和 w 在同一个 SCC
                    lowlink_map[v] = std::min(lowlink_map[v], index_map[w]);
                }
                // 后继 w 已访问但不在栈中：属于另一个已完成的 SCC，跳过
            }
        }

        // 如果 v 是 SCC 的根（lowlink == 自己的 index），弹出整个 SCC
        if (lowlink_map[v] == index_map[v]) {
            std::vector<int64_t> scc;
            int64_t w;
            do {
                w = tarjan_stack.top();
                tarjan_stack.pop();
                on_stack.erase(w);
                scc.push_back(w);
            } while (w != v);
            sccs.push_back(std::move(scc));
        }
    };

    // 从所有未访问节点开始（处理不连通的图）
    for (int64_t id : node_ids) {
        if (index_map.find(id) == index_map.end()) {
            strongconnect(id);
        }
    }

    return sccs;
}

std::vector<std::vector<int64_t>> GraphTraverser::find_circular_dependencies() {
    auto all_sccs = find_sccs();
    std::vector<std::vector<int64_t>> cycles;
    for (auto& scc : all_sccs) {
        if (scc.size() > 1) {
            cycles.push_back(std::move(scc));
        }
    }
    return cycles;
}

// ── 调用图指标统计 ──
//
// 统计调用图的关键指标：
//   - 入度排名（被调用最多的函数）：衡量代码复用度
//   - 出度排名（调用最多的函数）：衡量代码复杂度
//   - 调用深度：从入口点到叶子的最长路径
//   - 循环依赖数：Tarjan SCC 统计
GraphMetrics GraphTraverser::compute_metrics(int top_n) {
    GraphMetrics metrics;

    // 收集所有函数/方法节点
    auto all_files = db_.get_all_files();
    std::unordered_map<int64_t, Node> nodes_map;
    std::unordered_map<int64_t, int> in_degree;   // 被调用次数
    std::unordered_map<int64_t, int> out_degree;  // 调用次数

    for (const auto& file : all_files) {
        auto nodes = db_.find_nodes_by_file(file.path);
        for (const auto& n : nodes) {
            if (n.kind == NodeKind::Function || n.kind == NodeKind::Method) {
                nodes_map[n.id] = n;
                in_degree[n.id] = 0;
                out_degree[n.id] = 0;
            }
        }
    }

    metrics.total_nodes = static_cast<int>(nodes_map.size());

    // 统计入度/出度
    for (const auto& [id, _] : nodes_map) {
        auto edges = db_.get_edges_from(id, EdgeKind::Calls);
        for (const auto& e : edges) {
            if (nodes_map.count(e.target_id)) {
                out_degree[id]++;
                in_degree[e.target_id]++;
                metrics.total_edges++;
            }
        }
    }

    // 入度排名（被调用最多）
    std::vector<std::pair<int64_t, int>> in_vec(in_degree.begin(), in_degree.end());
    std::sort(in_vec.begin(), in_vec.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (int i = 0; i < std::min(top_n, static_cast<int>(in_vec.size())); ++i) {
        if (in_vec[i].second > 0) {
            metrics.most_called.push_back({nodes_map[in_vec[i].first], in_vec[i].second});
        }
    }

    // 出度排名（调用最多）
    std::vector<std::pair<int64_t, int>> out_vec(out_degree.begin(), out_degree.end());
    std::sort(out_vec.begin(), out_vec.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (int i = 0; i < std::min(top_n, static_cast<int>(out_vec.size())); ++i) {
        if (out_vec[i].second > 0) {
            metrics.most_calling.push_back({nodes_map[out_vec[i].first], out_vec[i].second});
        }
    }

    // 循环依赖数
    metrics.circular_deps = static_cast<int>(find_circular_dependencies().size());

    // 调用深度：从出度为 0 的节点（叶子）反向 BFS，找最大深度
    // 简化做法：从每个入口点（入度为 0 或 main）正向 BFS
    int total_depth = 0;
    int depth_samples = 0;
    for (const auto& [id, _] : nodes_map) {
        // 只从入口点（入度为 0 或名为 main）开始
        if (in_degree[id] > 0 && nodes_map[id].name != "main") continue;

        // BFS 计算最大深度
        std::queue<std::pair<int64_t, int>> frontier;
        std::unordered_set<int64_t> visited;
        frontier.push({id, 0});
        visited.insert(id);
        int max_d = 0;

        while (!frontier.empty()) {
            auto [current, depth] = frontier.front();
            frontier.pop();
            max_d = std::max(max_d, depth);

            auto edges = db_.get_edges_from(current, EdgeKind::Calls);
            for (const auto& e : edges) {
                if (!visited.count(e.target_id) && nodes_map.count(e.target_id)) {
                    visited.insert(e.target_id);
                    frontier.push({e.target_id, depth + 1});
                }
            }
        }

        if (max_d > 0) {
            total_depth += max_d;
            depth_samples++;
            metrics.max_call_depth = std::max(metrics.max_call_depth, max_d);
        }
    }

    metrics.avg_call_depth = depth_samples > 0
        ? static_cast<double>(total_depth) / depth_samples : 0.0;

    return metrics;
}

// ── 影响链 ──
//
// 在影响分析的基础上，为每个受影响节点附上调用路径。
// 回答"为什么改 X 会影响 Y"：X → A → B → Y。
std::vector<ImpactNode> GraphTraverser::get_impact_chain(int64_t node_id, int max_depth) {
    // 先做标准影响分析，拿到受影响的节点集合
    auto impact_result = get_impact(node_id, max_depth);

    // 对每个受影响节点，找从该节点到源节点的反向调用链。
    // 为什么用反向：impact 是反向 BFS（找 callers），所以路径也是反向的。
    // 例：A → B → C，impact(C) = {A, B}，路径是 C → B → A（谁调用了谁）
    std::vector<ImpactNode> result;
    for (const auto& node : impact_result.nodes) {
        ImpactNode in;
        in.node = node;
        in.path = find_reverse_path(node_id, node.id, max_depth);
        result.push_back(std::move(in));
    }

    return result;
}

}  // namespace codegraph
