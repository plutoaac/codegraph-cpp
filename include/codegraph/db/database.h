/**
 * database.h — SQLite 数据库封装
 *
 * 封装了所有与 SQLite 的交互，提供类型安全的 CRUD 接口。
 *
 * 核心功能：
 *   - 节点/边/文件的增删改查
 *   - FTS5 全文搜索
 *   - 批量插入（复用 prepared statement，性能优化）
 *   - 死代码检测
 *   - 事务管理
 *
 * 使用模式：
 *   Database db(".codegraph/index");
 *   db.init_schema();
 *   db.begin_transaction();
 *   // ... 插入操作 ...
 *   db.commit();
 *
 * 线程安全：
 *   每个 Database 实例持有一个 sqlite3* 连接，不是线程安全的。
 *   多线程场景下，每个线程应创建自己的 Database 实例。
 *   WAL 模式下，多个读连接可以并发，但写连接需要串行。
 */

#pragma once

#include "codegraph/core/types.h"
#include <sqlite3.h>
#include <optional>
#include <string>
#include <vector>

namespace codegraph {

class Database {
public:
    /**
     * 构造函数：打开 SQLite 数据库。
     *
     * @param db_path 数据库文件路径（如 ".codegraph/index"）
     * @throws std::runtime_error 如果打开失败
     *
     * 自动配置：
     *   - busy_timeout=5000（写锁冲突时等待 5 秒）
     *   - journal_mode=WAL（写不阻塞读）
     *   - foreign_keys=ON（启用外键约束）
     */
    Database(const std::string& db_path);
    ~Database();

    // 禁止拷贝（SQLite 连接不可拷贝）
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // 允许移动（转移连接所有权）
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    // ── Schema 管理 ──

    /** 初始化数据库 schema（建表、索引、触发器）。 */
    void init_schema();

    // ── 事务管理 ──

    /** 开始事务。 */
    void begin_transaction();
    /** 提交事务。 */
    void commit();
    /** 回滚事务。 */
    void rollback();

    // ── 节点操作 ──

    /**
     * 插入单个节点。
     * @return 自增 ID，失败返回 -1。
     */
    int64_t insert_node(const Node& node);

    /**
     * 按 ID 获取节点。
     * @return 找到返回 Node，找不到返回 nullopt。
     */
    std::optional<Node> get_node(int64_t id);

    /**
     * 按名字查找节点（两阶段：精确 + 模糊）。
     *
     * 排序优先级：
     *   1. 名字匹配度：精确 name > 精确 qualified_name > 后缀匹配
     *   2. 有 signature 的优先（定义 > 前向声明）
     *   3. .cpp/.cc 文件优先（实现 > 头文件声明）
     *
     * @param name 符号名
     * @param limit 最大返回数量
     * @return 匹配的节点列表（已排序）
     */
    std::vector<Node> find_nodes_by_name(const std::string& name, int limit = 20);

    /**
     * 按文件路径查找该文件中的所有节点。
     * 用于增量索引时清理旧节点、change-impact 时定位符号。
     */
    std::vector<Node> find_nodes_by_file(const std::string& file_path);

    /** 统计节点总数。 */
    int64_t count_nodes();

    // ── 边操作 ──

    /**
     * 插入单条边。
     * @return 自增 ID，失败返回 -1。
     */
    int64_t insert_edge(const Edge& edge);

    /**
     * 获取从某节点出发的所有指定类型的边。
     * 例：get_edges_from(id, Calls) → 该函数调用了谁
     */
    std::vector<Edge> get_edges_from(int64_t source_id, EdgeKind kind);

    /**
     * 获取指向某节点的所有指定类型的边。
     * 例：get_edges_to(id, Calls) → 谁调用了该函数
     */
    std::vector<Edge> get_edges_to(int64_t target_id, EdgeKind kind);

    /**
     * 获取从某节点出发的所有边（不限类型）。
     * 用于 DOT 导出。
     */
    std::vector<Edge> get_all_edges_from(int64_t source_id);

    /** 统计边总数。 */
    int64_t count_edges();

    // ── 文件操作 ──

    /**
     * 插入或更新文件记录。
     * INSERT OR REPLACE：如果 path 已存在则更新。
     */
    int64_t insert_file(const FileRecord& file);

    /** 按路径获取文件记录。 */
    std::optional<FileRecord> get_file(const std::string& path);

    /** 获取所有已索引的文件记录。 */
    std::vector<FileRecord> get_all_files();

    /** 更新文件的修改时间。 */
    void update_file_mtime(const std::string& path, int64_t mtime);

    // ── 删除操作（增量索引用） ──

    /** 删除某文件的所有节点。 */
    void delete_nodes_by_file(const std::string& file_path);

    /**
     * 删除某文件所有节点的关联边。
     * 必须在 delete_nodes_by_file 之前调用（外键约束）。
     */
    void delete_edges_for_file_nodes(const std::string& file_path);

    /** 删除某文件节点产生的未解析引用。 */
    void delete_unresolved_refs_by_file(const std::string& file_path);

    /** 删除文件记录。 */
    void delete_file(const std::string& file_path);

    // ── 未解析引用操作 ──

    /** 插入一条未解析引用。 */
    int64_t insert_unresolved_ref(const UnresolvedRef& ref);

    /** 获取所有未解析引用。 */
    std::vector<UnresolvedRef> get_unresolved_refs();

    /** 删除单条未解析引用。 */
    void delete_unresolved_ref(int64_t ref_id);

    // ── 批量查询 ──

    /**
     * 按 ID 列表批量获取节点。
     * 使用 IN (?) 占位符，减少数据库查询次数。
     */
    std::vector<Node> get_nodes_by_ids(const std::vector<int64_t>& ids);

    // ── 批量插入（复用 prepared statement） ──

    /**
     * 批量插入节点。
     *
     * 性能优化：prepared statement 只编译一次，后续复用。
     * 每次 bind → step → 收集 rowid。
     *
     * @param nodes 要插入的节点列表
     * @param out_ids 输出：每个节点的真实 ID（与 nodes 一一对应）
     */
    void insert_nodes_batch(const std::vector<Node>& nodes, std::vector<int64_t>& out_ids);

    /** 批量插入边。 */
    void insert_edges_batch(const std::vector<Edge>& edges);

    /** 批量插入未解析引用。 */
    void insert_unresolved_batch(const std::vector<UnresolvedRef>& refs);

    /** 批量删除已解析的未解析引用。 */
    void delete_unresolved_refs_batch(const std::vector<int64_t>& ref_ids);

    // ── 全文搜索 ──

    /**
     * FTS5 全文搜索。
     *
     * 流程：
     *   1. 将查询转为安全的 FTS5 查询（处理特殊字符）
     *   2. 查询 nodes_fts 虚表，用 bm25() 排序
     *   3. 按 kind 重排序（函数/类 > 变量 > import）
     *   4. 截断到 limit 条
     *
     * @param query 搜索关键词
     * @param limit 最大返回数量
     * @return 匹配的节点列表（已排序）
     */
    std::vector<Node> search_fts(const std::string& query, int limit = 20);

    // ── 死代码检测 ──

    /**
     * 查找没有被任何其他函数调用的函数/方法。
     *
     * 排除条件（避免误报）：
     *   - main() 入口点
     *   - 头文件中的函数（公共 API）
     *   - 测试/基准测试/演示文件
     *   - 有 qualified_name 的方法（类成员）
     *   - 析构函数
     *   - 常见局部变量名（lock、guard、buffer 等）
     */
    std::vector<Node> find_dead_code(const std::string& exclude_pattern = "");

    // ── 统计 ──

    /** 统计已索引的文件数。 */
    int64_t count_files();

private:
    sqlite3* db_ = nullptr;

    /** 执行任意 SQL（不返回结果）。 */
    void exec(const char* sql);

    /** 从 SELECT 结果的当前行读取一个 Node。 */
    Node read_node_row(sqlite3_stmt* stmt);

    // 缓存的 prepared statement（批量插入复用）
    sqlite3_stmt* stmt_insert_node_ = nullptr;
    sqlite3_stmt* stmt_insert_edge_ = nullptr;
    sqlite3_stmt* stmt_insert_unresolved_ = nullptr;

    /** 释放所有缓存的 prepared statement。 */
    void finalize_cached_stmts();
};

}  // namespace codegraph
