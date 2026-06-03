/**
 * traverser.cpp — 图遍历算法实现
 *
 * 本文件实现了调用图上的所有遍历和分析算法，是 codegraph 的算法核心。
 *
 * 实现的算法：
 *   1. get_callees()    — 正向 BFS：找某函数调用了谁
 *   2. get_callers()    — 反向 BFS：找谁调用了某函数
 *   3. get_impact()     — 影响分析：改某函数会影响谁（Calls + References）
 *   4. find_path()      — 正向 BFS 路径查找：A 到 B 的调用链
 *   5. find_reverse_path() — 反向 BFS 路径查找：影响链的回溯路径
 *   6. find_sccs()      — Tarjan 强连通分量：检测循环依赖
 *   7. compute_metrics() — 图指标统计：入度/出度排名、调用深度
 *   8. get_impact_chain() — 影响链：附带调用路径的影响分析
 *
 * 设计要点：
 *   - 所有遍历都用 visited 集合防止环路死循环
 *   - BFS 保证找到的路径是最短的（层序遍历）
 *   - 批量获取节点（get_nodes_by_ids）减少数据库查询次数
 *   - max_depth 限制遍历深度，防止大规模图上的性能问题
 */

#include "codegraph/graph/traverser.h"
#include <functional>
#include <queue>
#include <stack>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>

namespace codegraph {

GraphTraverser::GraphTraverser(Database& db) : db_(db) {}

/**
 * 正向 BFS：找某函数调用了谁（callees）。
 *
 * 算法：
 *   1. 从起始节点出发，沿 Calls 边正向遍历
 *   2. 每层扩展：获取当前节点的所有 Calls 边，批量获取目标节点
 *   3. 用 visited 集合防止重复访问（图中可能有环）
 *   4. 达到 max_depth 时停止扩展
 *
 * 优化：
 *   - 先收集所有 target_id，用 get_nodes_by_ids() 批量查询
 *   - 再用 unordered_map 建立 id → Node 映射，O(1) 查找
 *
 * 返回值：TraversalResult，包含所有可达节点和对应的边
 */
TraversalResult GraphTraverser::get_callees(int64_t node_id, int max_depth) {
    TraversalResult result;
    std::queue<std::pair<int64_t, int>> frontier;  // (节点ID, 深度)
    std::unordered_set<int64_t> visited;

    frontier.push({node_id, 0});
    visited.insert(node_id);

    while (!frontier.empty()) {
        auto [current_id, depth] = frontier.front();
        frontier.pop();

        if (depth >= max_depth) continue;

        // 获取当前节点的所有 Calls 边（正向）
        auto edges = db_.get_edges_from(current_id, EdgeKind::Calls);

        // 批量收集目标节点 ID
        std::vector<int64_t> target_ids;
        for (auto& edge : edges) {
            if (visited.count(edge.target_id)) continue;
            visited.insert(edge.target_id);
            target_ids.push_back(edge.target_id);
        }

        // 批量获取节点信息
        auto nodes_vec = db_.get_nodes_by_ids(target_ids);
        std::unordered_map<int64_t, Node> nodes_by_id;
        for (auto& n : nodes_vec) nodes_by_id[n.id] = n;

        // 将新节点加入结果和队列
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

/**
 * 反向 BFS：找谁调用了某函数（callers）。
 *
 * 与 get_callees 的区别：
 *   - get_callees 用 get_edges_from()（正向边）
 *   - get_callers 用 get_edges_to()（反向边）
 *   - get_callees 扩展 target_id，get_callers 扩展 source_id
 *
 * 用途：
 *   - "谁调用了 foo()？" → get_callers(foo_id)
 *   - 影响分析的基础：改了 foo()，需要知道谁受影响
 */
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

        // 反向：获取指向当前节点的 Calls 边
        auto edges = db_.get_edges_to(current_id, EdgeKind::Calls);

        // 批量收集源节点 ID
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

/**
 * 影响分析：改某函数会影响谁。
 *
 * 与 get_callers 的区别：
 *   - get_callers 只看 Calls 边（函数调用）
 *   - get_impact 同时看 Calls 和 References 边（调用 + 引用）
 *
 * References 边的含义：
 *   - 函数指针、回调、模板实例化等非直接调用的依赖
 *   - 这些依赖在改函数签名时也会受影响
 *
 * 用途：
 *   - 重构前评估影响范围："改了这个接口，有多少代码会受影响？"
 *   - PR review 时理解变更的传播范围
 */
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

        // 同时检查 Calls 和 References 两种边
        for (auto kind : {EdgeKind::Calls, EdgeKind::References}) {
            auto edges = db_.get_edges_to(current_id, kind);

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

/**
 * 正向 BFS 路径查找：从 from_id 到 to_id 的调用链。
 *
 * 算法：BFS 保证找到的是最短路径。
 * 实现：队列中存储完整路径（而不仅仅是节点 ID），
 *       每次扩展时复制路径并追加新节点。
 *
 * 用途：
 *   - "A 怎么调用到 B 的？" → find_path(A, B)
 *   - 调试调用链、理解代码流程
 *
 * 局限：
 *   - 只沿 Calls 边正向查找
 *   - 路径长度受 max_depth 限制
 */
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

    return {};  // 无路径
}

/**
 * 反向路径查找：从 from_id 出发，沿着反向边（被调用方向）找 to_id。
 *
 * 与 find_path 的区别：
 *   - find_path 沿 Calls 边正向（调用方向）
 *   - find_reverse_path 沿 Calls 边反向（被调用方向）
 *
 * 用途：影响分析中，找"从受影响节点到源节点"的调用链。
 *   例：A → B → C，find_reverse_path(C, A) 返回 [C, B, A]
 *   含义：C 被 B 调用，B 被 A 调用，所以改 A 会影响 C
 */
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

/**
 * 构建符号的完整上下文：节点本身 + callers + callees。
 * 用于 MCP 的 codegraph_context 工具。
 */
TraversalResult GraphTraverser::build_context(int64_t node_id, int max_depth) {
    TraversalResult result;

    // 添加节点本身
    auto node = db_.get_node(node_id);
    if (node.has_value()) {
        result.nodes.push_back(*node);
    }

    // 添加 callers（谁调用了它）
    auto callers = get_callers(node_id, max_depth);
    result.nodes.insert(result.nodes.end(), callers.nodes.begin(), callers.nodes.end());
    result.edges.insert(result.edges.end(), callers.edges.begin(), callers.edges.end());

    // 添加 callees（它调用了谁）
    auto callees = get_callees(node_id, max_depth);
    result.nodes.insert(result.nodes.end(), callees.nodes.begin(), callees.nodes.end());
    result.edges.insert(result.edges.end(), callees.edges.begin(), callees.edges.end());

    return result;
}

// ══════════════════════════════════════════════════════════════
//  Tarjan 强连通分量算法
// ══════════════════════════════════════════════════════════════

/**
 * Tarjan 强连通分量（SCC）算法。
 *
 * 用途：在调用图中查找所有强连通分量。
 * SCC 是一个极大节点集合，其中每个节点都能到达其他所有节点。
 * 大小 > 1 的 SCC 表示循环依赖（A 调用 B，B 调用 A）。
 *
 * 算法核心思路：
 *   1. DFS 遍历调用图，给每个节点分配一个发现时间戳（index）
 *   2. 维护 lowlink 值：从当前节点出发能到达的最小时间戳
 *   3. 当一个节点的 lowlink == 自己的 index 时，它是 SCC 的根
 *   4. 从栈中弹出直到根节点 → 这些节点构成一个 SCC
 *
 * 关键洞察：如果节点 v 能到达 DFS 栈中的祖先节点，
 * 那么 v 和该祖先属于同一个 SCC。
 *
 * 时间复杂度：O(V + E)，空间复杂度：O(V)
 *
 * 数据结构：
 *   - index_map: 节点 → 发现时间戳（DFS 顺序）
 *   - lowlink_map: 节点 → 能到达的最小时间戳
 *   - tarjan_stack: 当前 DFS 路径上的节点（用于弹出 SCC）
 *   - on_stack: 快速判断节点是否在栈中（O(1) 查找）
 */
std::vector<std::vector<int64_t>> GraphTraverser::find_sccs() {
    // 第一步：收集所有函数/方法节点，构建邻接表。
    // 只关注 Function 和 Method（不包括 Class、Variable 等），
    // 因为调用图中的循环才是有意义的模式。
    auto all_files = db_.get_all_files();
    std::unordered_set<int64_t> node_ids;
    std::unordered_map<int64_t, std::vector<int64_t>> adj;  // 邻接表

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

/**
 * 查找循环依赖：过滤出大小 > 1 的 SCC。
 * 大小为 1 的 SCC 是正常节点（自己调用自己不算循环）。
 */
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

// ══════════════════════════════════════════════════════════════
//  调用图指标统计
// ══════════════════════════════════════════════════════════════

/**
 * 计算调用图的关键指标。
 *
 * 统计内容：
 *   - 入度排名（被调用最多的函数）→ 衡量代码复用度
 *   - 出度排名（调用最多的函数）→ 衡量代码复杂度
 *   - 调用深度：从入口点到叶子的最长路径
 *   - 循环依赖数：Tarjan SCC 统计
 *
 * 调用深度计算：
 *   从入口点（入度为 0 或名为 main 的节点）正向 BFS，
 *   记录每个 BFS 的最大深度，取所有入口点的最大值和平均值。
 *
 * @param top_n 每个排名返回前 N 个
 */
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

    // 入度排名（被调用最多）→ 代码复用度指标
    std::vector<std::pair<int64_t, int>> in_vec(in_degree.begin(), in_degree.end());
    std::sort(in_vec.begin(), in_vec.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (int i = 0; i < std::min(top_n, static_cast<int>(in_vec.size())); ++i) {
        if (in_vec[i].second > 0) {
            metrics.most_called.push_back({nodes_map[in_vec[i].first], in_vec[i].second});
        }
    }

    // 出度排名（调用最多）→ 代码复杂度指标
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

    // 调用深度：从入口点（入度为 0 或 main）正向 BFS
    int total_depth = 0;
    int depth_samples = 0;
    for (const auto& [id, _] : nodes_map) {
        // 只从入口点开始（入度为 0 或名为 main）
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

// ══════════════════════════════════════════════════════════════
//  影响链
// ══════════════════════════════════════════════════════════════

/**
 * 影响链：在影响分析的基础上，为每个受影响节点附上调用路径。
 *
 * 回答"为什么改 X 会影响 Y"：X → A → B → Y
 *
 * 流程：
 *   1. 先做标准影响分析（get_impact），拿到受影响的节点集合
 *   2. 对每个受影响节点，用 find_reverse_path() 找从源节点到该节点的反向调用链
 *
 * 为什么用反向路径：
 *   get_impact 是反向 BFS（找 callers），所以路径也是反向的。
 *   例：A → B → C，impact(C) = {A, B}
 *   路径是 C → B → A（C 被 B 调用，B 被 A 调用）
 */
std::vector<ImpactNode> GraphTraverser::get_impact_chain(int64_t node_id, int max_depth) {
    // 先做标准影响分析
    auto impact_result = get_impact(node_id, max_depth);

    // 对每个受影响节点，找反向调用链
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
