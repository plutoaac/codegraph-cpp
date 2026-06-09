/**
 * main.cpp — CLI 入口和索引编排
 *
 * 本文件是 codegraph 的 CLI 入口，负责：
 *   1. 解析命令行参数，分发到对应的命令处理函数
 *   2. 编排索引流程：遍历目录 → 提析 → 存储 → 解析引用
 *   3. 实现增量索引：只处理变更的文件
 *   4. 上下文感知的边消歧：同名函数跨文件时优先选择同文件/目录/命名空间的候选
 *
 * 支持的 CLI 命令：
 *   init              — 初始化 .codegraph/ 目录和数据库
 *   index [<path>]    — 索引源文件（增量）
 *   resolve           — 解析未解析的跨文件引用
 *   search <query>    — 符号搜索
 *   context <symbol>  — 符号上下文
 *   dead-code         — 死代码检测
 *   cycles            — 循环依赖检测（Tarjan SCC）
 *   path <from> <to>  — 调用链路径查找
 *   metrics           — 调用图指标统计
 *   impact-chain      — 影响链分析
 *   export --dot      — DOT 格式导出
 *   change-impact     — git diff 影响分析
 *   serve --mcp       — 启动 MCP 服务器
 *   watch             — 文件监听 + 自动增量索引
 *
 * 索引流程：
 *   1. 遍历目录，收集变更文件（增量：比较 mtime/size）
 *   2. 用 tree-sitter 提取每个文件的节点和未解析引用
 *   3. 批量插入节点到数据库
 *   4. 解析未解析引用：按名字查找候选节点，用上下文评分选择最佳匹配
 *   5. 批量插入边到数据库
 *
 * 上下文感知边消歧：
 *   当多个同名函数存在时（如多个文件都有 WaitServerReady），
 *   优先选择与源节点在同一文件/目录/命名空间的候选。
 *   评分规则：同文件 +10, 同目录 +5, 同命名空间 +3
 */

#include "codegraph/core/types.h"
#include "codegraph/db/database.h"
#include "codegraph/extraction/extractor.h"
#include "codegraph/graph/traverser.h"
#include "codegraph/context/context_builder.h"
#include "codegraph/mcp/mcp_server.h"
#include "codegraph/sync/file_watcher.h"
#include "codegraph/diff/diff_parser.h"

#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <csignal>
#include <atomic>
#include <sys/wait.h>
#include <sys/inotify.h>
#include <unistd.h>

using namespace codegraph;
namespace fs = std::filesystem;

/**
 * 打印使用帮助。
 */
static void print_usage() {
    std::cout << R"(Usage: codegraph <command> [options]

Commands:
  init                    Initialize .codegraph/ in current directory
  init -i <path>          Initialize and index source files at <path>
  index [<path>]          Index source files (default: current directory)
  resolve                 Resolve pending cross-file references
  search <query>          Search for symbols by name
  search-semantic <query> Semantic search (requires: pip install sentence-transformers)
  embed                   Generate embeddings for semantic search
  context <symbol>        Get rich context for a symbol
  change-impact [ref]     Analyze impact of code changes (default: uncommitted)
  dead-code               Find unreferenced symbols
  cycles                  Find circular dependencies (Tarjan's SCC)
  path <from> <to>        Find call chain path between two symbols
  metrics                 Show call graph metrics (top called/calling, depth, cycles)
  impact-chain <symbol>   Impact analysis with call paths
  export --dot            Export call graph in Graphviz DOT format
  status                  Show index statistics
  serve --mcp             Start MCP server (stdio JSON-RPC)
  watch                   Watch for file changes and re-index
)" << std::endl;
}

/**
 * 向上查找 .codegraph/index 数据库路径。
 *
 * 从当前目录开始，逐级向上查找 .codegraph/index 文件。
 * 这样在子目录中也能找到项目根目录的索引。
 */
static std::string get_db_path() {
    fs::path cwd = fs::current_path();
    while (true) {
        fs::path cg = cwd / ".codegraph";
        if (fs::exists(cg / "index")) {
            return (cg / "index").string();
        }
        if (cwd == cwd.parent_path()) break;
        cwd = cwd.parent_path();
    }
    return "";
}

/**
 * 打开数据库，找不到则报错退出。
 */
static Database open_db() {
    std::string path = get_db_path();
    if (path.empty()) {
        std::cerr << "Error: No .codegraph/index found. Run 'codegraph init' first." << std::endl;
        exit(1);
    }
    Database db(path);
    return db;
}

/**
 * 判断文件是否应该跳过（隐藏目录、构建目录、依赖目录等）。
 */
static bool should_skip(const std::string& file_path) {
    return file_path.find("/.") != std::string::npos ||
           file_path.find("/node_modules/") != std::string::npos ||
           file_path.find("/build/") != std::string::npos ||
           file_path.find("/build-") != std::string::npos ||
           file_path.find("/__pycache__/") != std::string::npos ||
           file_path.find("\\.git\\") != std::string::npos;
}

/**
 * 待索引文件的中间结构。
 *
 * 提取阶段的结果先存在这里，后续批量插入数据库。
 * id_map 用于将 tree-sitter 分配的临时负 ID 映射为数据库的真实 ID。
 */
struct PendingIndexedFile {
    std::string file_path;
    std::string language;
    ExtractionResult result;
    std::unordered_map<int64_t, int64_t> id_map;  // 临时ID → 真实ID
    int inserted_edges = 0;
};

/**
 * 创建文件记录（用于 files 表）。
 * 读取文件的修改时间和大小，用于增量索引判断。
 */
static FileRecord make_file_record(const std::string& file_path, const std::string& lang) {
    FileRecord fr;
    fr.path = file_path;
    fr.language = lang;
    try {
        auto ftime = fs::last_write_time(fs::path(file_path));
        fr.mtime = std::chrono::duration_cast<std::chrono::seconds>(
            ftime.time_since_epoch()).count();
        fr.size = fs::file_size(fs::path(file_path));
    } catch (...) {
        fr.mtime = 0;
        fr.size = 0;
    }
    return fr;
}

/**
 * 判断文件是否已变更（增量索引用）。
 *
 * 比较数据库中记录的 mtime/size 与文件系统的当前值。
 * 如果不一致，说明文件被修改过，需要重新索引。
 */
static bool is_changed(Database& db, const fs::directory_entry& entry, const std::string& file_path) {
    auto existing = db.get_file(file_path);
    if (!existing.has_value()) return true;  // 新文件

    try {
        auto ftime = fs::last_write_time(entry);
        auto mtime = std::chrono::duration_cast<std::chrono::seconds>(
            ftime.time_since_epoch()).count();
        return existing->mtime != mtime ||
               existing->size != static_cast<int64_t>(fs::file_size(entry));
    } catch (...) {
        return true;  // 读取失败，保守处理
    }
}

/**
 * 提取单个文件的节点和调用关系。
 *
 * 流程：
 *   1. 根据语言创建对应的提取器（C++ 或 Python）
 *   2. 读取文件内容
 *   3. 调用提取器的 extract() 方法
 */
static bool extract_file(const std::string& file_path, const std::string& lang, PendingIndexedFile& out) {
    auto extractor = create_extractor(lang);
    if (!extractor) return false;

    std::ifstream ifs(file_path);
    if (!ifs.is_open()) return false;
    std::string source((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());

    out.file_path = file_path;
    out.language = lang;
    out.result = extractor->extract(file_path, source);
    return true;
}

/**
 * 上下文感知的候选目标评分。
 *
 * 当多个同名函数存在时（如多个文件都有 WaitServerReady），
 * 优先选择与源节点在同一文件/目录/命名空间的候选。
 *
 * 评分规则：
 *   同文件 +10  — 最强信号（同一个 .cpp 里的函数大概率互相调用）
 *   同目录 +5   — 中等信号（同模块的函数可能有关联）
 *   同命名空间 +3 — 弱信号（同命名空间可能有关联）
 *
 * 为什么需要这个：
 *   tree-sitter 没有类型信息，只能用名字匹配。
 *   如果项目中有多个同名函数（如 test_helper() 在多个测试文件中），
 *   不加消歧会错误地连接到远处的同名函数。
 */
static int score_target(const Node& source, const Node& candidate) {
    int score = 0;

    // 同文件 = 最强信号
    if (source.file_path == candidate.file_path) {
        score += 10;
    } else {
        // 同目录 = 中等信号
        auto src_dir = source.file_path.rfind('/');
        auto cand_dir = candidate.file_path.rfind('/');
        if (src_dir != std::string::npos && cand_dir != std::string::npos) {
            if (source.file_path.substr(0, src_dir) ==
                candidate.file_path.substr(0, cand_dir)) {
                score += 5;
            }
        }
    }

    // 同命名空间 = 弱信号
    if (!source.qualified_name.empty() && !candidate.qualified_name.empty()) {
        auto src_colon = source.qualified_name.rfind("::");
        auto cand_colon = candidate.qualified_name.rfind("::");
        if (src_colon != std::string::npos && cand_colon != std::string::npos) {
            std::string src_ns = source.qualified_name.substr(0, src_colon);
            std::string cand_ns = candidate.qualified_name.substr(0, cand_colon);
            if (src_ns == cand_ns) {
                score += 3;
            }
        }
    }

    return score;
}

/**
 * 清理已删除文件的索引数据。
 *
 * 遍历数据库中的所有文件记录，检查文件是否还存在。
 * 如果文件已被删除，清理其节点、边和未解析引用。
 */
static bool prune_deleted_files(Database& db) {
    bool pruned = false;
    for (const auto& file : db.get_all_files()) {
        if (file.path.empty() || fs::exists(file.path)) {
            continue;
        }
        db.begin_transaction();
        try {
            db.delete_unresolved_refs_by_file(file.path);
            db.delete_edges_for_file_nodes(file.path);
            db.delete_nodes_by_file(file.path);
            db.delete_file(file.path);
            db.commit();
        } catch (...) {
            db.rollback();
            throw;
        }
        pruned = true;
        std::cout << "Pruned deleted file from index: " << file.path << std::endl;
    }
    return pruned;
}

/**
 * 将提取结果批量写入数据库。
 *
 * 流程：
 *   1. 清理旧数据（按文件路径删除旧的 refs/edges/nodes）
 *   2. 创建文件节点（每个文件一个 File 类型节点）
 *   3. 批量插入所有节点，收集返回的真实 ID
 *   4. 建立临时 ID → 真实 ID 的映射
 *   5. 解析未解析引用：按名字查找候选，用上下文评分选择最佳匹配
 *   6. 批量插入边（contains 边 + calls 边）
 *   7. 批量插入剩余的未解析引用
 *   8. 更新文件记录
 *
 * 事务保证：
 *   整个过程在一个事务中，任何失败都会回滚。
 */
static int index_extracted_files(Database& db, std::vector<PendingIndexedFile>& files) {
    if (files.empty()) return 0;

    db.begin_transaction();
    try {

    // 清理旧数据（顺序重要：先删 refs，因为引用了 node id）
    for (const auto& file : files) {
        db.delete_unresolved_refs_by_file(file.file_path);
    }
    for (const auto& file : files) {
        db.delete_edges_for_file_nodes(file.file_path);
    }
    for (const auto& file : files) {
        db.delete_nodes_by_file(file.file_path);
    }

    // 批量插入所有节点
    std::vector<Node> all_nodes;
    std::unordered_map<std::string, int64_t> file_node_indices;
    for (auto& file : files) {
        // 创建文件节点
        Node file_node;
        file_node.kind = NodeKind::File;
        file_node.name = file.file_path;
        file_node.file_path = file.file_path;
        file_node.line = 0;
        file_node_indices[file.file_path] = all_nodes.size();
        all_nodes.push_back(file_node);

        // 添加该文件中的所有其他节点
        for (auto& node : file.result.nodes) {
            all_nodes.push_back(node);
        }
    }
    std::vector<int64_t> all_ids;
    db.insert_nodes_batch(all_nodes, all_ids);

    // 建立临时 ID → 真实 ID 的映射
    int node_count = 0;
    size_t offset = 0;
    std::vector<Edge> contains_edges;
    for (auto& file : files) {
        int64_t file_node_id = (offset < all_ids.size()) ? all_ids[offset] : -1;
        if (file_node_id < 0) { db.rollback(); return 0; }
        file_node_indices[file.file_path] = file_node_id;

        // 映射临时 ID 到真实 ID（跳过 offset 0 的文件节点）
        for (size_t i = 0; i < file.result.nodes.size(); ++i) {
            int64_t temp_id = -static_cast<int64_t>(i + 1);
            int64_t real_id = (offset + 1 + i < all_ids.size()) ? all_ids[offset + 1 + i] : -1;
            if (real_id < 0) { db.rollback(); return 0; }
            file.id_map[temp_id] = real_id;
            node_count++;

            // 创建 "contains" 边：文件 → 节点
            Edge ce;
            ce.source_id = file_node_id;
            ce.target_id = real_id;
            ce.kind = EdgeKind::Contains;
            ce.line = file.result.nodes[i].line;
            ce.col = file.result.nodes[i].col;
            contains_edges.push_back(ce);
        }
        offset += file.result.nodes.size() + 1;  // +1 for file node
    }

    // 解析未解析引用：按名字查找候选，用上下文评分选择最佳匹配
    std::vector<Edge> new_edges;
    std::vector<UnresolvedRef> new_unresolved;
    std::unordered_set<std::string> seen_edges;  // 边去重
    std::unordered_map<std::string, std::vector<Node>> name_cache;  // 候选缓存

    for (auto& file : files) {
        for (auto& ref : file.result.unresolved) {
            int64_t source_real = 0;
            auto it = file.id_map.find(ref.source_node_id);
            if (it != file.id_map.end()) source_real = it->second;
            if (source_real <= 0) continue;

            // 获取源节点信息（用于上下文评分）
            Node source_info;
            int src_idx = -ref.source_node_id - 1;
            if (src_idx >= 0 && src_idx < static_cast<int>(file.result.nodes.size())) {
                source_info = file.result.nodes[src_idx];
            }
            source_info.file_path = file.file_path;

            // 获取候选节点（带缓存）
            auto cache_it = name_cache.find(ref.ref_name);
            if (cache_it == name_cache.end()) {
                auto candidates = db.find_nodes_by_name(ref.ref_name, 10);
                cache_it = name_cache.emplace(ref.ref_name, std::move(candidates)).first;
            }

            const auto& targets = cache_it->second;
            if (!targets.empty()) {
                // 用上下文评分选择最佳候选
                int64_t best_id = targets[0].id;
                int best_score = -1;
                for (const auto& cand : targets) {
                    int s = score_target(source_info, cand);
                    if (s > best_score) {
                        best_score = s;
                        best_id = cand.id;
                    }
                }

                // 边去重：同一对 source+target+kind 只保留一条
                std::string edge_key = std::to_string(source_real) + ":" +
                                       std::to_string(best_id) + ":" +
                                       std::to_string(static_cast<int>(EdgeKind::Calls));
                if (seen_edges.insert(edge_key).second) {
                    Edge e;
                    e.source_id = source_real;
                    e.target_id = best_id;
                    e.kind = EdgeKind::Calls;
                    e.line = ref.line;
                    e.col = ref.col;
                    new_edges.push_back(e);
                }
                file.inserted_edges++;
            } else {
                // 未找到目标，保留为未解析引用
                UnresolvedRef uref;
                uref.source_node_id = source_real;
                uref.ref_name = ref.ref_name;
                uref.ref_kind = ref.ref_kind;
                uref.line = ref.line;
                uref.col = ref.col;
                new_unresolved.push_back(uref);
            }
        }
    }

    // 批量插入边（contains 边 + calls 边）
    contains_edges.insert(contains_edges.end(), new_edges.begin(), new_edges.end());
    db.insert_edges_batch(contains_edges);
    db.insert_unresolved_batch(new_unresolved);

    // 更新文件记录
    for (const auto& file : files) {
        if (db.insert_file(make_file_record(file.file_path, file.language)) < 0) {
            db.rollback();
            return 0;
        }
    }

    db.commit();
    return node_count;
    } catch (...) {
        db.rollback();
        throw;
    }
}

/**
 * 第二遍：解析之前未解析的引用。
 *
 * 现在所有文件都已索引，可以尝试将之前无法解析的引用解析为正式的边。
 * 使用与 index_extracted_files 相同的上下文感知评分策略。
 */
static void resolve_unresolved_refs(Database& db) {
    auto refs = db.get_unresolved_refs();
    if (refs.empty()) {
        std::cout << "No unresolved refs to resolve" << std::endl;
        return;
    }

    std::cout << "Resolving " << refs.size() << " unresolved refs..." << std::endl;

    // 批量获取所有源节点
    std::unordered_set<int64_t> source_ids;
    for (const auto& ref : refs) {
        source_ids.insert(ref.source_node_id);
    }
    std::vector<int64_t> source_id_vec(source_ids.begin(), source_ids.end());
    auto source_nodes = db.get_nodes_by_ids(source_id_vec);
    std::unordered_map<int64_t, Node> source_map;
    for (auto& n : source_nodes) {
        source_map[n.id] = n;
    }

    // 候选缓存
    std::unordered_map<std::string, std::vector<Node>> name_cache;

    std::vector<Edge> new_edges;
    std::vector<int64_t> to_delete;
    std::unordered_set<std::string> seen_edges;

    for (const auto& ref : refs) {
        auto src_it = source_map.find(ref.source_node_id);
        if (src_it == source_map.end()) continue;
        const Node& source = src_it->second;

        // 获取候选（带缓存）
        auto cache_it = name_cache.find(ref.ref_name);
        if (cache_it == name_cache.end()) {
            auto candidates = db.find_nodes_by_name(ref.ref_name, 10);
            cache_it = name_cache.emplace(ref.ref_name, std::move(candidates)).first;
        }

        const auto& candidates = cache_it->second;
        if (candidates.empty()) continue;

        // 用上下文评分选择最佳候选
        int64_t best_id = candidates[0].id;
        int best_score = -1;
        for (const auto& cand : candidates) {
            int s = score_target(source, cand);
            if (s > best_score) {
                best_score = s;
                best_id = cand.id;
            }
        }

        std::string edge_key = std::to_string(ref.source_node_id) + ":" +
                               std::to_string(best_id) + ":" +
                               std::to_string(static_cast<int>(EdgeKind::Calls));
        if (seen_edges.insert(edge_key).second) {
            Edge e;
            e.source_id = ref.source_node_id;
            e.target_id = best_id;
            e.kind = EdgeKind::Calls;
            e.line = ref.line;
            e.col = ref.col;
            new_edges.push_back(e);
        }
        to_delete.push_back(ref.id);
    }

    std::cout << "Resolved " << new_edges.size() << " refs, " << (refs.size() - new_edges.size()) << " still unresolved" << std::endl;

    if (!new_edges.empty()) {
        db.begin_transaction();
        try {
            db.insert_edges_batch(new_edges);
            db.delete_unresolved_refs_batch(to_delete);
            db.commit();
        } catch (...) {
            db.rollback();
            throw;
        }
    }
}

/**
 * 索引目录中的所有源文件。
 *
 * 增量索引流程：
 *   1. 清理已删除文件的索引
 *   2. 遍历目录，收集变更文件（比较 mtime/size）
 *   3. 对变更文件，还要重新索引引用它的其他文件
 *      （因为被引用者的行号可能变了）
 *   4. 提取并批量写入数据库
 *
 * 为什么要重新索引引用者：
 *   如果文件 A 的函数签名变了，文件 B 中对它的调用边
 *   可能需要更新（行号变了）。通过 get_edges_to() 找到 B，
 *   然后把 B 也加入重新索引列表。
 */
static void index_directory(Database& db, const std::string& path, bool incremental = true) {
    std::vector<std::pair<std::string, std::string>> changed_files;
    std::unordered_set<std::string> scheduled_paths;
    bool any_changed = !incremental || prune_deleted_files(db);

    auto schedule_file = [&](const std::string& file_path, const std::string& lang) {
        if (scheduled_paths.insert(file_path).second) {
            changed_files.push_back({file_path, lang});
        }
    };

    // 遍历目录，收集变更文件
    for (auto& entry : fs::recursive_directory_iterator(path)) {
        if (!entry.is_regular_file()) continue;
        std::string file_path = entry.path().string();
        if (should_skip(file_path)) continue;

        std::string lang = detect_language(file_path);
        if (lang.empty()) continue;

        if (!incremental || is_changed(db, entry, file_path)) {
            schedule_file(file_path, lang);
            if (incremental) {
                any_changed = true;
            }
        }
    }

    if (!any_changed) {
        std::cout << "Indexed 0 files, 0 nodes, " << db.count_edges() << " edges" << std::endl;
        return;
    }

    // 增量模式：也要重新索引引用了变更文件的其他文件
    if (incremental) {
        std::vector<std::string> initially_changed;
        initially_changed.reserve(changed_files.size());
        for (const auto& [file_path, _] : changed_files) {
            initially_changed.push_back(file_path);
        }

        for (const auto& file_path : initially_changed) {
            // 找到所有指向该文件节点的 Calls 边
            for (const auto& node : db.find_nodes_by_file(file_path)) {
                for (const auto& edge : db.get_edges_to(node.id, EdgeKind::Calls)) {
                    auto source = db.get_node(edge.source_id);
                    if (!source.has_value() || source->file_path == file_path) {
                        continue;
                    }
                    if (source->file_path.empty() || should_skip(source->file_path)) {
                        continue;
                    }
                    std::string lang = detect_language(source->file_path);
                    if (!lang.empty() && fs::exists(source->file_path)) {
                        schedule_file(source->file_path, lang);
                    }
                }
            }
        }
    }

    // 提取并批量写入
    std::vector<PendingIndexedFile> extracted;
    extracted.reserve(changed_files.size());
    for (const auto& [file_path, lang] : changed_files) {
        PendingIndexedFile file;
        if (extract_file(file_path, lang, file)) {
            extracted.push_back(std::move(file));
        }
    }

    int node_count = index_extracted_files(db, extracted);

    // 更新索引时间戳（用于 MCP 缓存失效）
    std::string timestamp_file = ".codegraph/index_timestamp";
    std::ofstream ts(timestamp_file);
    if (ts) {
        ts << std::chrono::system_clock::now().time_since_epoch().count();
        ts.close();
    }

    std::cout << "Indexed " << extracted.size() << " files, "
              << node_count << " nodes, " << db.count_edges() << " edges" << std::endl;
}

/**
 * 运行 Python 脚本（用于语义搜索的 embed/query）。
 * 使用 fork+execvp，不经过 shell。
 */
static int run_python(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>("python3"));
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "Failed to fork python3" << std::endl;
        return 1;
    }
    if (pid == 0) {
        execvp("python3", argv.data());
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        std::cerr << "Failed to wait for python3" << std::endl;
        return 1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

/**
 * CLI 主入口：解析命令行参数，分发到对应的命令处理。
 *
 * 命令分发逻辑：
 *   - 每个命令用 if-else if 链匹配
 *   - 参数解析在各个分支中独立完成
 *   - 简单的命令直接输出文本，复杂的命令输出 JSON
 */
int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "init") {
        // ── init: 初始化 .codegraph/ 目录和数据库 ──
        std::string index_path;
        bool do_index = false;

        for (int i = 2; i < argc; i++) {
            if (std::string(argv[i]) == "-i") {
                if (i + 1 < argc) {
                    index_path = argv[i + 1];
                    do_index = true;
                    i++;
                } else {
                    std::cerr << "Error: -i requires a path argument" << std::endl;
                    return 1;
                }
            } else {
                index_path = argv[i];
                do_index = true;
            }
        }

        fs::create_directories(".codegraph");
        Database db(".codegraph/index");
        db.init_schema();
        std::cout << "Initialized CodeGraph in " << fs::current_path().string() << std::endl;

        if (do_index) {
            index_directory(db, index_path, false);
            resolve_unresolved_refs(db);
        }

    } else if (cmd == "index") {
        // ── index: 增量索引源文件 ──
        Database db = open_db();
        std::string path = argc > 2 ? argv[2] : ".";
        std::cout << "Indexing " << path << "..." << std::endl;
        index_directory(db, path, true);
        resolve_unresolved_refs(db);

    } else if (cmd == "resolve") {
        // ── resolve: 解析未解析的跨文件引用 ──
        Database db = open_db();
        resolve_unresolved_refs(db);

    } else if (cmd == "search") {
        // ── search: 符号搜索 ──
        if (argc < 3) {
            std::cerr << "Usage: codegraph search <query>" << std::endl;
            return 1;
        }
        Database db = open_db();
        auto results = db.search_fts(argv[2]);
        if (results.empty()) {
            std::cout << "No results found." << std::endl;
        }
        std::string output;
        std::unordered_map<std::string, bool> seen_files;
        for (auto& n : results) {
            std::string line = std::string(node_kind_str(n.kind)) + " " + n.name
                      + " @ " + n.file_path + ":" + std::to_string(n.line);
            if (!n.signature.empty()) line += "  " + n.signature;
            output += line + "\n";
            seen_files[n.file_path] = true;
        }
        std::cout << output;
        // Token 统计：帮助用户了解使用 codegraph 节省了多少 token
        int resp_tokens = static_cast<int>(output.size()) / 4;
        size_t raw_bytes = 0;
        for (auto& [f, _] : seen_files) {
            std::error_code ec;
            auto sz = fs::file_size(f, ec);
            if (!ec) raw_bytes += sz;
        }
        int raw_tokens = static_cast<int>(raw_bytes) / 4;
        if (raw_tokens > 0) {
            std::cerr << "[codegraph] " << resp_tokens << " tokens"
                      << " (vs " << raw_tokens << " tokens to read "
                      << seen_files.size() << " source files)"
                      << std::endl;
        }

    } else if (cmd == "context") {
        // ── context: 符号上下文 ──
        if (argc < 3) {
            std::cerr << "Usage: codegraph context <symbol>" << std::endl;
            return 1;
        }
        Database db = open_db();
        GraphTraverser traverser(db);
        ContextBuilder context(db, traverser);
        auto result = context.build_context(argv[2]);
        std::string output = result.dump(2);
        std::cout << output << std::endl;
        int resp_tokens = static_cast<int>(output.size()) / 4;
        std::cerr << "[codegraph] " << resp_tokens << " tokens returned" << std::endl;

    } else if (cmd == "embed") {
        // ── embed: 生成语义搜索的嵌入向量 ──
        fs::path bin_path = fs::canonical(argv[0]);
        fs::path script_path = bin_path.parent_path() / ".." / "scripts" / "embed.py";
        if (!fs::exists(script_path)) {
            script_path = fs::path(CMAKE_SOURCE_DIR) / "scripts" / "embed.py";
        }
        std::string db_path = get_db_path();
        if (db_path.empty()) {
            std::cerr << "No .codegraph/index found. Run 'codegraph init' first." << std::endl;
            return 1;
        }
        return run_python({script_path.string(), "index", db_path});

    } else if (cmd == "search-semantic") {
        // ── search-semantic: 语义搜索 ──
        if (argc < 3) {
            std::cerr << "Usage: codegraph search-semantic <query>" << std::endl;
            return 1;
        }
        fs::path bin_path = fs::canonical(argv[0]);
        fs::path script_path = bin_path.parent_path() / ".." / "scripts" / "embed.py";
        if (!fs::exists(script_path)) {
            script_path = fs::path(CMAKE_SOURCE_DIR) / "scripts" / "embed.py";
        }
        std::string db_path = get_db_path();
        if (db_path.empty()) {
            std::cerr << "No .codegraph/index found. Run 'codegraph init' first." << std::endl;
            return 1;
        }
        std::string query = argv[2];
        return run_python({script_path.string(), "query", db_path, query});

    } else if (cmd == "status") {
        // ── status: 索引统计 ──
        Database db = open_db();
        std::cout << "Nodes: " << db.count_nodes() << std::endl;
        std::cout << "Edges: " << db.count_edges() << std::endl;
        std::cout << "Files: " << db.count_files() << std::endl;

    } else if (cmd == "dead-code") {
        // ── dead-code: 死代码检测 ──
        Database db = open_db();
        auto dead_nodes = db.find_dead_code();
        if (dead_nodes.empty()) {
            std::cout << "No dead code found." << std::endl;
        } else {
            nlohmann::json output = nlohmann::json::array();
            for (auto& node : dead_nodes) {
                output.push_back({
                    {"kind", node_kind_str(node.kind)},
                    {"name", node.name},
                    {"file", node.file_path},
                    {"line", node.line}
                });
            }
            std::string result = output.dump(2);
            std::cout << result << std::endl;
            int resp_tokens = static_cast<int>(result.size()) / 4;
            std::cerr << "[codegraph] Found " << dead_nodes.size()
                      << " unreferenced symbols (" << resp_tokens << " tokens)" << std::endl;
        }

    } else if (cmd == "cycles") {
        // ── cycles: 循环依赖检测（Tarjan SCC） ──
        Database db = open_db();
        GraphTraverser traverser(db);
        auto cycles = traverser.find_circular_dependencies();
        if (cycles.empty()) {
            std::cout << "No circular dependencies found." << std::endl;
        } else {
            nlohmann::json output = nlohmann::json::array();
            for (const auto& scc : cycles) {
                nlohmann::json members = nlohmann::json::array();
                for (int64_t id : scc) {
                    auto node = db.get_node(id);
                    if (node.has_value()) {
                        members.push_back({
                            {"name", node->name},
                            {"file", node->file_path},
                            {"line", node->line}
                        });
                    }
                }
                output.push_back({{"size", scc.size()}, {"members", std::move(members)}});
            }
            std::string result = output.dump(2);
            std::cout << result << std::endl;
            std::cerr << "[codegraph] Found " << cycles.size()
                      << " circular dependencies" << std::endl;
        }

    } else if (cmd == "path") {
        // ── path: 调用链路径查找 ──
        if (argc < 4) {
            std::cerr << "Usage: codegraph path <from_symbol> <to_symbol>" << std::endl;
            return 1;
        }
        Database db = open_db();
        GraphTraverser traverser(db);

        std::string from_name = argv[2];
        std::string to_name = argv[3];

        auto from_nodes = db.find_nodes_by_name(from_name, 1);
        auto to_nodes = db.find_nodes_by_name(to_name, 1);
        if (from_nodes.empty()) {
            std::cerr << "Symbol not found: " << from_name << std::endl;
            return 1;
        }
        if (to_nodes.empty()) {
            std::cerr << "Symbol not found: " << to_name << std::endl;
            return 1;
        }

        auto path = traverser.find_path(from_nodes[0].id, to_nodes[0].id);
        if (path.empty()) {
            std::cout << nlohmann::json{{"error", "No path found"}}.dump(2) << std::endl;
            return 0;
        }

        nlohmann::json output;
        output["from"] = {{"name", from_nodes[0].name}, {"file", from_nodes[0].file_path}, {"line", from_nodes[0].line}};
        output["to"] = {{"name", to_nodes[0].name}, {"file", to_nodes[0].file_path}, {"line", to_nodes[0].line}};
        output["depth"] = path.size();
        output["path"] = nlohmann::json::array();
        auto path_nodes = db.get_nodes_by_ids(path);
        for (const auto& n : path_nodes) {
            output["path"].push_back({{"name", n.name}, {"file", n.file_path}, {"line", n.line}});
        }
        std::string result = output.dump(2);
        std::cout << result << std::endl;
        std::cerr << "[codegraph] Path depth: " << path.size() << std::endl;

    } else if (cmd == "metrics") {
        // ── metrics: 调用图指标统计 ──
        Database db = open_db();
        GraphTraverser traverser(db);
        auto metrics = traverser.compute_metrics(10);

        nlohmann::json output;
        output["total_function_nodes"] = metrics.total_nodes;
        output["total_call_edges"] = metrics.total_edges;
        output["circular_dependencies"] = metrics.circular_deps;
        output["max_call_depth"] = metrics.max_call_depth;
        output["avg_call_depth"] = metrics.avg_call_depth;

        output["most_called"] = nlohmann::json::array();
        for (const auto& [node, count] : metrics.most_called) {
            output["most_called"].push_back({
                {"name", node.name}, {"file", node.file_path}, {"line", node.line}, {"callers", count}
            });
        }

        output["most_calling"] = nlohmann::json::array();
        for (const auto& [node, count] : metrics.most_calling) {
            output["most_calling"].push_back({
                {"name", node.name}, {"file", node.file_path}, {"line", node.line}, {"callees", count}
            });
        }

        std::string result = output.dump(2);
        std::cout << result << std::endl;
        std::cerr << "[codegraph] " << metrics.total_nodes << " nodes, "
                  << metrics.total_edges << " edges, "
                  << metrics.circular_deps << " cycles" << std::endl;

    } else if (cmd == "impact-chain") {
        // ── impact-chain: 影响链分析 ──
        if (argc < 3) {
            std::cerr << "Usage: codegraph impact-chain <symbol>" << std::endl;
            return 1;
        }
        Database db = open_db();
        GraphTraverser traverser(db);

        std::string symbol = argv[2];
        auto nodes = db.find_nodes_by_name(symbol, 1);
        if (nodes.empty()) {
            std::cerr << "Symbol not found: " << symbol << std::endl;
            return 1;
        }

        auto impact = traverser.get_impact_chain(nodes[0].id, 5);

        nlohmann::json output;
        output["source"] = {{"name", nodes[0].name}, {"file", nodes[0].file_path}, {"line", nodes[0].line}};
        output["impact"] = nlohmann::json::array();
        for (const auto& in : impact) {
            nlohmann::json path_names = nlohmann::json::array();
            auto path_nodes = db.get_nodes_by_ids(in.path);
            for (const auto& pn : path_nodes) {
                path_names.push_back(pn.name);
            }
            output["impact"].push_back({
                {"name", in.node.name},
                {"file", in.node.file_path},
                {"line", in.node.line},
                {"path", std::move(path_names)},
                {"depth", in.path.size()}
            });
        }

        std::string result = output.dump(2);
        std::cout << result << std::endl;
        std::cerr << "[codegraph] Impact: " << impact.size() << " symbols affected" << std::endl;

    } else if (cmd == "export") {
        // ── export: DOT 格式导出 ──
        bool dot_format = false;
        std::string symbol;
        int depth = 2;
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--dot") dot_format = true;
            else if (arg == "--symbol" && i + 1 < argc) symbol = argv[++i];
            else if (arg == "--depth" && i + 1 < argc) depth = std::stoi(argv[++i]);
        }

        if (!dot_format) {
            std::cerr << "Usage: codegraph export --dot [--symbol <name>] [--depth <n>]" << std::endl;
            return 1;
        }

        Database db = open_db();
        GraphTraverser traverser(db);

        std::vector<Node> nodes;
        std::vector<Edge> edges;

        if (!symbol.empty()) {
            // 导出特定符号的子图
            auto found = db.find_nodes_by_name(symbol, 1);
            if (found.empty()) {
                std::cerr << "Symbol not found: " << symbol << std::endl;
                return 1;
            }
            Node& start = found[0];
            nodes.push_back(start);

            auto callers = traverser.get_callers(start.id, depth);
            auto callees = traverser.get_callees(start.id, depth);

            for (auto& n : callers.nodes) nodes.push_back(n);
            for (auto& n : callees.nodes) nodes.push_back(n);

            std::unordered_set<int64_t> seen_edge_ids;
            auto add_edge = [&](const Edge& e) {
                if (seen_edge_ids.insert(e.id).second) {
                    edges.push_back(e);
                }
            };
            for (auto& e : callers.edges) add_edge(e);
            for (auto& e : callees.edges) add_edge(e);

            // 获取文件节点和 contains 边
            std::unordered_set<std::string> file_paths;
            for (auto& n : nodes) {
                if (!n.file_path.empty()) file_paths.insert(n.file_path);
            }
            for (auto& fp : file_paths) {
                auto file_nodes = db.find_nodes_by_file(fp);
                for (auto& fn : file_nodes) {
                    if (fn.kind == NodeKind::File) {
                        nodes.push_back(fn);
                        auto contains = db.get_all_edges_from(fn.id);
                        for (auto& e : contains) {
                            if (e.kind == EdgeKind::Contains) {
                                add_edge(e);
                            }
                        }
                        break;
                    }
                }
            }
        } else {
            // 导出完整图（限函数/类）
            auto all_files = db.get_all_files();
            for (auto& f : all_files) {
                auto file_nodes = db.find_nodes_by_file(f.path);
                for (auto& n : file_nodes) {
                    if (n.kind == NodeKind::Function || n.kind == NodeKind::Method ||
                        n.kind == NodeKind::Class || n.kind == NodeKind::Struct) {
                        nodes.push_back(n);
                    }
                }
            }
            std::unordered_set<int64_t> node_ids;
            for (auto& n : nodes) node_ids.insert(n.id);
            for (auto& n : nodes) {
                auto out_edges = db.get_all_edges_from(n.id);
                for (auto& e : out_edges) {
                    if (node_ids.count(e.target_id)) {
                        edges.push_back(e);
                    }
                }
            }
        }

        // 生成 DOT 输出
        auto escape_dot = [](std::string s) -> std::string {
            std::string result;
            for (char c : s) {
                switch (c) {
                    case '"': result += "\\\""; break;
                    case '<': result += "\\<"; break;
                    case '>': result += "\\>"; break;
                    case '{': result += "\\{"; break;
                    case '}': result += "\\}"; break;
                    case '|': result += "\\|"; break;
                    case '\\': result += "\\\\"; break;
                    case '\n': result += "\\n"; break;
                    default: result += c; break;
                }
            }
            return result;
        };

        std::cout << "digraph codegraph {" << std::endl;
        std::cout << "  rankdir=LR;" << std::endl;
        std::cout << "  node [shape=box, style=filled, fontname=\"Courier\"];" << std::endl;
        std::cout << std::endl;

        std::unordered_map<int64_t, std::string> node_labels;
        for (auto& n : nodes) {
            std::string label = escape_dot(n.name);
            if (!n.signature.empty()) {
                std::string sig = n.signature;
                if (sig.size() > 50) sig = sig.substr(0, 47) + "...";
                label += "\\n" + escape_dot(sig);
            }
            std::string node_id = "n" + std::to_string(n.id);
            node_labels[n.id] = node_id;

            std::string color;
            switch (n.kind) {
                case NodeKind::Function: case NodeKind::Method:
                    color = "#E3F2FD"; break;  // 浅蓝
                case NodeKind::Class: case NodeKind::Struct:
                    color = "#E8F5E9"; break;  // 浅绿
                default:
                    color = "#FFF3E0"; break;  // 浅橙
            }

            std::cout << "  " << node_id << " [label=\"" << label
                      << "\", fillcolor=\"" << color << "\"];" << std::endl;
        }

        std::cout << std::endl;

        for (auto& e : edges) {
            auto src = node_labels.find(e.source_id);
            auto dst = node_labels.find(e.target_id);
            if (src != node_labels.end() && dst != node_labels.end()) {
                std::cout << "  " << src->second << " -> " << dst->second;
                std::cout << " [label=\"" << edge_kind_str(e.kind) << "\"];" << std::endl;
            }
        }

        std::cout << "}" << std::endl;
        std::cerr << "[codegraph] Exported " << nodes.size() << " nodes, "
                  << edges.size() << " edges" << std::endl;

    } else if (cmd == "change-impact") {
        // ── change-impact: git diff 影响分析 ──
        std::string ref = (argc > 2) ? argv[2] : "";
        std::string diff_output = run_git_diff(ref);
        if (diff_output.empty()) {
            std::cout << "No changes found." << std::endl;
            return 0;
        }

        auto hunks = parse_diff(diff_output);
        if (hunks.empty()) {
            std::cout << "No changed lines found." << std::endl;
            return 0;
        }

        Database db = open_db();
        GraphTraverser traverser(db);

        // 收集受影响的节点
        std::unordered_map<int64_t, Node> affected_nodes;
        std::unordered_map<std::string, std::vector<DiffHunk>> file_hunks;
        for (auto& h : hunks) {
            file_hunks[h.file_path].push_back(h);
        }

        for (auto& [file, hunks_in_file] : file_hunks) {
            auto nodes_in_file = db.find_nodes_by_file(file);
            for (auto& node : nodes_in_file) {
                for (auto& hunk : hunks_in_file) {
                    if (node.line <= hunk.line_end &&
                        (node.end_line >= hunk.line_start || node.end_line == 0)) {
                        affected_nodes[node.id] = node;
                        break;
                    }
                }
            }
        }

        if (affected_nodes.empty()) {
            std::cout << "No symbols affected by this diff." << std::endl;
            return 0;
        }

        // 对每个受影响节点运行 impact 分析
        std::unordered_map<int64_t, Node> impact_nodes;
        std::vector<Edge> all_edges;
        for (auto& [id, node] : affected_nodes) {
            auto result = traverser.get_impact(id, 3);
            for (auto& n : result.nodes) {
                impact_nodes[n.id] = n;
            }
            for (auto& e : result.edges) {
                all_edges.push_back(e);
            }
        }

        // 输出 JSON
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

        std::string result = output.dump(2);
        std::cout << result << std::endl;
        int resp_tokens = static_cast<int>(result.size()) / 4;
        std::cerr << "[codegraph] " << resp_tokens << " tokens returned" << std::endl;

    } else if (cmd == "serve") {
        // ── serve: 启动 MCP 服务器 ──
        bool mcp = false;
        for (int i = 2; i < argc; i++) {
            if (std::string(argv[i]) == "--mcp") mcp = true;
        }
        if (!mcp) {
            std::cerr << "Usage: codegraph serve --mcp" << std::endl;
            return 1;
        }
        Database db = open_db();
        GraphTraverser traverser(db);
        ContextBuilder context(db, traverser);
        McpServer server(db, traverser, context);
        server.run();

    } else if (cmd == "watch") {
        // ── watch: 文件监听 + 自动增量索引 ──
        Database db = open_db();
        std::string path = argc > 2 ? argv[2] : ".";
        std::string abs_path = fs::absolute(path).lexically_normal().string();
        std::cout << "Watching " << abs_path << " for changes..." << std::endl;

        FileWatcher watcher(abs_path);

        watcher.set_callback([&](const std::string& changed_path, uint32_t mask) {
            if (should_skip(changed_path)) return;

            // 新目录：自动添加监听
            if (mask & IN_CREATE) {
                try {
                    if (fs::is_directory(changed_path)) {
                        watcher.add_watch_recursive(changed_path);
                        return;
                    }
                } catch (...) {}
            }

            std::cout << "Change detected: " << changed_path << std::endl;
            index_directory(db, path, true);
            resolve_unresolved_refs(db);
        });

        // 优雅退出：SIGINT/SIGTERM 停止监听
        static std::atomic<bool> running{true};
        std::signal(SIGINT, [](int) { running = false; });
        std::signal(SIGTERM, [](int) { running = false; });

        while (running) {
            watcher.poll(1000);
        }
        std::cout << "\nStopped watching." << std::endl;

    } else {
        print_usage();
        return 1;
    }

    return 0;
}
