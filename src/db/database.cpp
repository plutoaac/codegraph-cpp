/**
 * database.cpp — SQLite 数据库层实现
 *
 * 本文件封装了所有与 SQLite 的交互，是 codegraph 的持久化层。
 *
 * 数据库架构：
 *   - nodes 表：存储所有代码符号（函数、类、变量等）
 *   - edges 表：存储符号间的关系（调用、包含、继承等）
 *   - files 表：记录已索引的文件（用于增量更新判断）
 *   - unresolved_refs 表：存储未解析的引用（跨文件调用，后续统一解析）
 *   - nodes_fts：FTS5 虚表，支持全文搜索
 *
 * 设计要点：
 *   - 使用 WAL 模式（PRAGMA journal_mode=WAL）支持并发读
 *   - 批量插入复用 prepared statement，避免重复编译 SQL
 *   - StmtGuard（unique_ptr + 自定义 deleter）确保 stmt 不泄漏
 *   - read_node_row 中对 nullable 列做空指针检查
 *   - FTS5 搜索结果按 kind 重排序（函数/类优先，变量其次，import 最后）
 */

#include "codegraph/db/database.h"
#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>

namespace codegraph {

// ── 辅助工具 ──

/**
 * StmtDeleter：sqlite3_stmt 的自定义删除器。
 * 配合 unique_ptr 使用，确保 stmt 在作用域结束时自动 finalize。
 * 这是 RAII 模式在 C API 上的应用。
 */
struct StmtDeleter {
    void operator()(sqlite3_stmt* s) const { if (s) sqlite3_finalize(s); }
};
using StmtGuard = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

/**
 * 构造 SQLite 错误信息的异常。
 * 从 sqlite3_errmsg(db) 获取可读的错误描述。
 */
static std::runtime_error sqlite_error(sqlite3* db, const std::string& op) {
    return std::runtime_error(op + ": " +
                              (db ? sqlite3_errmsg(db) : "unknown sqlite error"));
}

/**
 * 编译 SQL 语句，失败则抛异常。
 * sqlite3_prepare_v2 将 SQL 文本编译为字节码，返回 stmt 指针。
 */
static sqlite3_stmt* prepare_or_throw(sqlite3* db, const char* sql, const std::string& op) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw sqlite_error(db, op);
    }
    return stmt;
}

/**
 * 执行 SQL 直到完成（INSERT/UPDATE/DELETE），失败则抛异常。
 */
static void expect_done(sqlite3* db, sqlite3_stmt* stmt, const std::string& op) {
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        throw sqlite_error(db, op);
    }
}

/**
 * 执行 SQL 并返回状态码（SELECT 返回 SQLITE_ROW，结束返回 SQLITE_DONE）。
 * 如果返回非预期状态码则抛异常。
 */
static int step_or_throw(sqlite3* db, sqlite3_stmt* stmt, const std::string& op) {
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        throw sqlite_error(db, op);
    }
    return rc;
}

/**
 * 将用户查询转换为安全的 FTS5 查询字符串。
 *
 * 问题：C++ 限定名如 "ns::test_func" 包含 ":"，FTS5 会将其解析为操作符。
 * 方案：检测到非字母数字字符时，用双引号包装为短语查询（"ns::test_func"），
 * 双引号内的字符不会被 FTS5 解析为操作符。
 *
 * 安全性：
 *   - 用户输入中的双引号 " 转义为 ""（FTS5 的转义规则）
 *   - 空查询返回空字符串（不执行搜索）
 *   - 纯字母数字查询直接返回（无需包装）
 */
static std::string make_fts_query(const std::string& query) {
    bool has_token_char = false;
    bool needs_phrase = false;
    for (unsigned char ch : query) {
        if (std::isalnum(ch) || ch == '_') {
            has_token_char = true;
            continue;
        }
        if (!std::isspace(ch)) {
            needs_phrase = true;
        }
    }

    if (!has_token_char) return "";
    if (!needs_phrase) return query;

    // 包装为短语查询，双引号内的 " 需要转义为 ""
    std::string quoted;
    quoted.reserve(query.size() + 2);
    quoted.push_back('"');
    for (char ch : query) {
        if (ch == '"') quoted.push_back('"');
        quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
}

// ── 数据库 Schema ──

/**
 * 数据库表结构定义。
 *
 * nodes 表：
 *   - id: 自增主键
 *   - kind: 节点类型（NodeKind 枚举的整数值）
 *   - name: 符号名（如 "foo"）
 *   - qualified_name: 限定名（如 "MyClass::foo"）
 *   - file_path: 所在文件路径
 *   - line/col/end_line/end_col: 位置信息
 *   - signature: 函数签名
 *   - docstring: 文档注释
 *   - visibility: 访问控制（public/private/protected）
 *   - is_static/is_const/is_exported: 修饰符
 *
 * edges 表：
 *   - id: 自增主键
 *   - source_id/target_id: 源和目标节点的 ID
 *   - kind: 边类型（EdgeKind 枚举的整数值）
 *   - line/col: 边的位置（调用发生的行号）
 *   - metadata: 额外元数据（JSON 字符串）
 *
 * files 表：
 *   - path: 文件路径（唯一约束）
 *   - language: 语言标识
 *   - mtime: 最后修改时间（用于增量更新判断）
 *   - size: 文件大小（辅助增量判断）
 *
 * unresolved_refs 表：
 *   - 存储第一遍索引时无法解析的跨文件引用
 *   - resolve 命令会尝试将这些引用解析为正式的 edge
 *
 * nodes_fts 虚表：
 *   - FTS5 全文搜索，索引 name/qualified_name/signature/docstring/file_path
 *   - 通过触发器与 nodes 表自动同步
 *   - bm25() 函数用于相关性排序
 *
 * 索引：
 *   - idx_nodes_name: 按名字查找节点
 *   - idx_nodes_file: 按文件查找节点
 *   - idx_edges_source/target: 按源/目标查找边
 *   - idx_edges_kind: 按类型过滤边
 */
static const char* SCHEMA_SQL = R"(
CREATE TABLE IF NOT EXISTS nodes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind INTEGER NOT NULL,
    name TEXT NOT NULL,
    qualified_name TEXT,
    file_path TEXT NOT NULL,
    language TEXT,
    line INTEGER,
    col INTEGER,
    end_line INTEGER,
    end_col INTEGER,
    signature TEXT,
    docstring TEXT,
    visibility TEXT,
    is_static INTEGER DEFAULT 0,
    is_const INTEGER DEFAULT 0,
    is_exported INTEGER DEFAULT 0
);

CREATE TABLE IF NOT EXISTS edges (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_id INTEGER NOT NULL,
    target_id INTEGER NOT NULL,
    kind INTEGER NOT NULL,
    line INTEGER,
    col INTEGER,
    metadata TEXT,
    FOREIGN KEY (source_id) REFERENCES nodes(id),
    FOREIGN KEY (target_id) REFERENCES nodes(id)
);

CREATE TABLE IF NOT EXISTS files (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    path TEXT UNIQUE NOT NULL,
    language TEXT,
    mtime INTEGER,
    size INTEGER
);

CREATE TABLE IF NOT EXISTS unresolved_refs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_node_id INTEGER NOT NULL,
    ref_name TEXT NOT NULL,
    ref_kind TEXT,
    line INTEGER,
    col INTEGER
);

CREATE VIRTUAL TABLE IF NOT EXISTS nodes_fts USING fts5(
    name, qualified_name, signature, docstring, file_path,
    content='nodes', content_rowid='id'
);

CREATE TRIGGER IF NOT EXISTS nodes_ai AFTER INSERT ON nodes BEGIN
    INSERT INTO nodes_fts(rowid, name, qualified_name, signature, docstring, file_path)
    VALUES (new.id, new.name, new.qualified_name, new.signature, new.docstring, new.file_path);
END;

CREATE TRIGGER IF NOT EXISTS nodes_ad AFTER DELETE ON nodes BEGIN
    INSERT INTO nodes_fts(nodes_fts, rowid, name, qualified_name, signature, docstring, file_path)
    VALUES ('delete', old.id, old.name, old.qualified_name, old.signature, old.docstring, old.file_path);
END;

CREATE TRIGGER IF NOT EXISTS nodes_au AFTER UPDATE ON nodes BEGIN
    INSERT INTO nodes_fts(nodes_fts, rowid, name, qualified_name, signature, docstring, file_path)
    VALUES ('delete', old.id, old.name, old.qualified_name, old.signature, old.docstring, old.file_path);
    INSERT INTO nodes_fts(rowid, name, qualified_name, signature, docstring, file_path)
    VALUES (new.id, new.name, new.qualified_name, new.signature, new.docstring, new.file_path);
END;

CREATE INDEX IF NOT EXISTS idx_nodes_name ON nodes(name);
CREATE INDEX IF NOT EXISTS idx_nodes_file ON nodes(file_path);
CREATE INDEX IF NOT EXISTS idx_edges_source ON edges(source_id);
CREATE INDEX IF NOT EXISTS idx_edges_target ON edges(target_id);
CREATE INDEX IF NOT EXISTS idx_edges_kind ON edges(kind);
)";

// ── Database 生命周期 ──

/**
 * 构造函数：打开 SQLite 数据库并配置基本参数。
 *
 * 配置说明：
 *   - busy_timeout=5000: 写锁冲突时等待 5 秒再报错（WAL 模式下很少触发）
 *   - journal_mode=WAL: 写操作不阻塞读操作（关键性能优化）
 *   - foreign_keys=ON: 启用外键约束（edges 表引用 nodes 表）
 */
Database::Database(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        throw std::runtime_error("Failed to open database: " + std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_busy_timeout(db_, 5000);
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA foreign_keys=ON");
}

/**
 * 析构函数：释放缓存的 prepared statement 和数据库连接。
 */
Database::~Database() {
    finalize_cached_stmts();
    if (db_) sqlite3_close(db_);
}

/**
 * 释放所有缓存的 prepared statement。
 * 批量插入时会缓存 stmt 以避免重复编译，析构时必须释放。
 */
void Database::finalize_cached_stmts() {
    if (stmt_insert_node_) { sqlite3_finalize(stmt_insert_node_); stmt_insert_node_ = nullptr; }
    if (stmt_insert_edge_) { sqlite3_finalize(stmt_insert_edge_); stmt_insert_edge_ = nullptr; }
    if (stmt_insert_unresolved_) { sqlite3_finalize(stmt_insert_unresolved_); stmt_insert_unresolved_ = nullptr; }
}

/**
 * 移动构造函数：转移数据库所有权。
 * 将 other 的指针置空，防止 other 析构时关闭数据库。
 */
Database::Database(Database&& other) noexcept
    : db_(other.db_),
      stmt_insert_node_(other.stmt_insert_node_),
      stmt_insert_edge_(other.stmt_insert_edge_),
      stmt_insert_unresolved_(other.stmt_insert_unresolved_) {
    other.db_ = nullptr;
    other.stmt_insert_node_ = nullptr;
    other.stmt_insert_edge_ = nullptr;
    other.stmt_insert_unresolved_ = nullptr;
}

/**
 * 移动赋值运算符：释放当前资源，转移 other 的所有权。
 */
Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        finalize_cached_stmts();
        if (db_) sqlite3_close(db_);
        db_ = other.db_;
        stmt_insert_node_ = other.stmt_insert_node_;
        stmt_insert_edge_ = other.stmt_insert_edge_;
        stmt_insert_unresolved_ = other.stmt_insert_unresolved_;
        other.db_ = nullptr;
        other.stmt_insert_node_ = nullptr;
        other.stmt_insert_edge_ = nullptr;
        other.stmt_insert_unresolved_ = nullptr;
    }
    return *this;
}

// ── 行读取 ──

/**
 * 从 SELECT 结果的当前行读取一个 Node 对象。
 *
 * 列顺序：id, kind, name, qualified_name, file_path, language,
 *          line, col, end_line, end_col, signature, docstring,
 *          visibility, is_static, is_const, is_exported
 *
 * 注意：SQLite 的 TEXT 列可能返回 NULL（如 qualified_name 未设置），
 * sqlite3_column_text() 对 NULL 返回 nullptr，需要检查后给默认值。
 */
Node Database::read_node_row(sqlite3_stmt* stmt) {
    Node n;
    n.id = sqlite3_column_int64(stmt, 0);
    n.kind = static_cast<NodeKind>(sqlite3_column_int(stmt, 1));
    n.name = reinterpret_cast<const char*>(
        sqlite3_column_text(stmt, 2) ? sqlite3_column_text(stmt, 2) : (const unsigned char*)"");
    n.qualified_name = reinterpret_cast<const char*>(
        sqlite3_column_text(stmt, 3) ? sqlite3_column_text(stmt, 3) : (const unsigned char*)"");
    n.file_path = reinterpret_cast<const char*>(
        sqlite3_column_text(stmt, 4) ? sqlite3_column_text(stmt, 4) : (const unsigned char*)"");
    n.language = reinterpret_cast<const char*>(
        sqlite3_column_text(stmt, 5) ? sqlite3_column_text(stmt, 5) : (const unsigned char*)"");
    n.line = sqlite3_column_int(stmt, 6);
    n.col = sqlite3_column_int(stmt, 7);
    n.end_line = sqlite3_column_int(stmt, 8);
    n.end_col = sqlite3_column_int(stmt, 9);
    n.signature = reinterpret_cast<const char*>(
        sqlite3_column_text(stmt, 10) ? sqlite3_column_text(stmt, 10) : (const unsigned char*)"");
    n.docstring = reinterpret_cast<const char*>(
        sqlite3_column_text(stmt, 11) ? sqlite3_column_text(stmt, 11) : (const unsigned char*)"");
    n.visibility = reinterpret_cast<const char*>(
        sqlite3_column_text(stmt, 12) ? sqlite3_column_text(stmt, 12) : (const unsigned char*)"");
    n.is_static = sqlite3_column_int(stmt, 13) != 0;
    n.is_const = sqlite3_column_int(stmt, 14) != 0;
    n.is_exported = sqlite3_column_int(stmt, 15) != 0;
    return n;
}

// ── 基础操作 ──

/**
 * 执行任意 SQL（不返回结果）。
 * 用于 DDL（CREATE TABLE）、PRAGMA 设置等。
 */
void Database::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("SQL error: " + msg);
    }
}

/** 初始化数据库 schema（建表、建索引、建触发器）。 */
void Database::init_schema() { exec(SCHEMA_SQL); }

/** 开始事务。 */
void Database::begin_transaction() { exec("BEGIN"); }
/** 提交事务。 */
void Database::commit() { exec("COMMIT"); }
/** 回滚事务。 */
void Database::rollback() { exec("ROLLBACK"); }

// ── 节点操作 ──

/**
 * 插入单个节点，返回自增 ID。
 * 使用 StmtGuard 确保 stmt 在函数返回时自动释放。
 */
int64_t Database::insert_node(const Node& node) {
    const char* sql = R"(INSERT INTO nodes
        (kind, name, qualified_name, file_path, language, line, col, end_line, end_col, signature, docstring, visibility, is_static, is_const, is_exported)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?))";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare insert_node");
    StmtGuard guard(stmt);
    sqlite3_bind_int(stmt, 1, static_cast<int>(node.kind));
    sqlite3_bind_text(stmt, 2, node.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, node.qualified_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, node.file_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, node.language.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, node.line);
    sqlite3_bind_int(stmt, 7, node.col);
    sqlite3_bind_int(stmt, 8, node.end_line);
    sqlite3_bind_int(stmt, 9, node.end_col);
    sqlite3_bind_text(stmt, 10, node.signature.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, node.docstring.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, node.visibility.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 13, node.is_static ? 1 : 0);
    sqlite3_bind_int(stmt, 14, node.is_const ? 1 : 0);
    sqlite3_bind_int(stmt, 15, node.is_exported ? 1 : 0);
    if (sqlite3_step(stmt) != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(db_);
}

/**
 * 按 ID 获取单个节点。
 * 返回 optional：找到返回 Node，找不到返回 nullopt。
 */
std::optional<Node> Database::get_node(int64_t id) {
    const char* sql = "SELECT id, kind, name, qualified_name, file_path, language, line, col, end_line, end_col, signature, docstring, visibility, is_static, is_const, is_exported FROM nodes WHERE id=?";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare get_node");
    StmtGuard guard(stmt);
    sqlite3_bind_int64(stmt, 1, id);
    int rc = step_or_throw(db_, stmt, "get_node");
    if (rc != SQLITE_ROW) return std::nullopt;
    return read_node_row(stmt);
}

/**
 * 按名字查找节点。两阶段查询：
 *
 * 第一阶段（精确匹配）：
 *   WHERE name=? OR qualified_name=? OR qualified_name LIKE '%::name'
 *   排序优先级：
 *     1. 名字匹配度：精确 name > 精确 qualified_name > 后缀匹配
 *     2. 有 signature 的优先（定义 > 前向声明）
 *     3. .cpp/.cc 文件优先（实现 > 头文件声明）
 *
 * 第二阶段（模糊匹配）：
 *   如果精确匹配结果不足，用 LIKE '%name%' 补充
 *   排除第一阶段已匹配的结果
 *
 * 为什么用两阶段：
 *   精确匹配结果更准确，应该优先返回。
 *   模糊匹配用于处理部分匹配的情况（如搜索 "foo" 找到 "bar_foo_baz"）。
 */
std::vector<Node> Database::find_nodes_by_name(const std::string& name, int limit) {
    std::vector<Node> results;

    auto append_rows = [&](const char* sql, auto bind_fn, int max_rows) {
        sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare find_nodes_by_name");
        StmtGuard guard(stmt);
        bind_fn(stmt, max_rows);
        int rc = SQLITE_OK;
        while ((rc = step_or_throw(db_, stmt, "find_nodes_by_name")) == SQLITE_ROW) {
            results.push_back(read_node_row(stmt));
        }
    };

    // 第一阶段：精确匹配 + 后缀匹配
    const char* exact_sql = R"(SELECT id, kind, name, qualified_name, file_path, language, line, col, end_line, end_col, signature, docstring, visibility, is_static, is_const, is_exported
        FROM nodes WHERE kind NOT IN (0, 10, 11) AND (name=? OR qualified_name=? OR qualified_name LIKE ?)
        ORDER BY CASE WHEN name=? THEN 0 WHEN qualified_name=? THEN 1 WHEN qualified_name LIKE ? THEN 2 ELSE 3 END,
                 CASE WHEN signature IS NOT NULL AND signature != '' THEN 0 ELSE 1 END,
                 CASE WHEN file_path LIKE '%.cpp' THEN 0 WHEN file_path LIKE '%.cc' THEN 0 ELSE 1 END
        LIMIT ?)";
    std::string suffix_pattern = "%::" + name;
    append_rows(exact_sql, [&](sqlite3_stmt* stmt, int max_rows) {
        sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, suffix_pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, suffix_pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 7, max_rows);
    }, limit);

    int remaining = limit - static_cast<int>(results.size());
    if (remaining <= 0) return results;

    // 第二阶段：模糊匹配（排除已匹配的精确结果）
    const char* like_sql = R"(SELECT id, kind, name, qualified_name, file_path, language, line, col, end_line, end_col, signature, docstring, visibility, is_static, is_const, is_exported
        FROM nodes
        WHERE kind NOT IN (0, 10, 11) AND (name LIKE ? OR qualified_name LIKE ?)
          AND name<>?
          AND (qualified_name IS NULL OR qualified_name<>?)
        LIMIT ?)";
    std::string like_pattern = "%" + name + "%";
    append_rows(like_sql, [&](sqlite3_stmt* stmt, int max_rows) {
        sqlite3_bind_text(stmt, 1, like_pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, like_pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, max_rows);
    }, remaining);

    return results;
}

/**
 * 按文件路径查找该文件中的所有节点。
 * 用于：
 *   - 增量更新时清理旧节点
 *   - change-impact 时定位受影响的符号
 *   - export 时收集文件中的所有符号
 */
std::vector<Node> Database::find_nodes_by_file(const std::string& file_path) {
    std::vector<Node> results;
    const char* sql = "SELECT id, kind, name, qualified_name, file_path, language, line, col, end_line, end_col, signature, docstring, visibility, is_static, is_const, is_exported FROM nodes WHERE file_path=?";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare find_nodes_by_file");
    StmtGuard guard(stmt);
    sqlite3_bind_text(stmt, 1, file_path.c_str(), -1, SQLITE_TRANSIENT);
    int rc = SQLITE_OK;
    while ((rc = step_or_throw(db_, stmt, "find_nodes_by_file")) == SQLITE_ROW) {
        results.push_back(read_node_row(stmt));
    }
    return results;
}

/** 统计节点总数。 */
int64_t Database::count_nodes() {
    sqlite3_stmt* stmt = prepare_or_throw(db_, "SELECT COUNT(*) FROM nodes", "prepare count_nodes");
    StmtGuard guard(stmt);
    if (step_or_throw(db_, stmt, "count_nodes") != SQLITE_ROW) {
        throw std::runtime_error("count_nodes: no result row");
    }
    return sqlite3_column_int64(stmt, 0);
}

// ── 边操作 ──

/**
 * 插入单条边，返回自增 ID。
 */
int64_t Database::insert_edge(const Edge& edge) {
    const char* sql = "INSERT INTO edges (source_id, target_id, kind, line, col, metadata) VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare insert_edge");
    StmtGuard guard(stmt);
    sqlite3_bind_int64(stmt, 1, edge.source_id);
    sqlite3_bind_int64(stmt, 2, edge.target_id);
    sqlite3_bind_int(stmt, 3, static_cast<int>(edge.kind));
    sqlite3_bind_int(stmt, 4, edge.line);
    sqlite3_bind_int(stmt, 5, edge.col);
    sqlite3_bind_text(stmt, 6, edge.metadata.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(db_);
}

/**
 * 获取从某节点出发的所有指定类型的边。
 * 用于：
 *   - get_edges_from(id, Calls) → 找该函数调用了谁（callees）
 *   - get_edges_from(id, Contains) → 找文件包含哪些符号
 */
std::vector<Edge> Database::get_edges_from(int64_t source_id, EdgeKind kind) {
    std::vector<Edge> results;
    const char* sql = "SELECT id, source_id, target_id, kind, line, col, metadata FROM edges WHERE source_id=? AND kind=?";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare get_edges_from");
    StmtGuard guard(stmt);
    sqlite3_bind_int64(stmt, 1, source_id);
    sqlite3_bind_int(stmt, 2, static_cast<int>(kind));
    int rc = SQLITE_OK;
    while ((rc = step_or_throw(db_, stmt, "get_edges_from")) == SQLITE_ROW) {
        Edge e;
        e.id = sqlite3_column_int64(stmt, 0);
        e.source_id = sqlite3_column_int64(stmt, 1);
        e.target_id = sqlite3_column_int64(stmt, 2);
        e.kind = static_cast<EdgeKind>(sqlite3_column_int(stmt, 3));
        e.line = sqlite3_column_int(stmt, 4);
        e.col = sqlite3_column_int(stmt, 5);
        e.metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6) ? sqlite3_column_text(stmt, 6) : (const unsigned char*)"");
        results.push_back(e);
    }
    return results;
}

/**
 * 获取指向某节点的所有指定类型的边。
 * 用于：
 *   - get_edges_to(id, Calls) → 找谁调用了该函数（callers）
 *   - get_edges_to(id, References) → 找谁引用了该符号
 */
std::vector<Edge> Database::get_edges_to(int64_t target_id, EdgeKind kind) {
    std::vector<Edge> results;
    const char* sql = "SELECT id, source_id, target_id, kind, line, col, metadata FROM edges WHERE target_id=? AND kind=?";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare get_edges_to");
    StmtGuard guard(stmt);
    sqlite3_bind_int64(stmt, 1, target_id);
    sqlite3_bind_int(stmt, 2, static_cast<int>(kind));
    int rc = SQLITE_OK;
    while ((rc = step_or_throw(db_, stmt, "get_edges_to")) == SQLITE_ROW) {
        Edge e;
        e.id = sqlite3_column_int64(stmt, 0);
        e.source_id = sqlite3_column_int64(stmt, 1);
        e.target_id = sqlite3_column_int64(stmt, 2);
        e.kind = static_cast<EdgeKind>(sqlite3_column_int(stmt, 3));
        e.line = sqlite3_column_int(stmt, 4);
        e.col = sqlite3_column_int(stmt, 5);
        e.metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6) ? sqlite3_column_text(stmt, 6) : (const unsigned char*)"");
        results.push_back(e);
    }
    return results;
}

/**
 * 获取从某节点出发的所有边（不限类型）。
 * 用于 DOT 导出时收集所有关系。
 */
std::vector<Edge> Database::get_all_edges_from(int64_t source_id) {
    std::vector<Edge> results;
    const char* sql = "SELECT id, source_id, target_id, kind, line, col, metadata FROM edges WHERE source_id=?";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare get_all_edges_from");
    StmtGuard guard(stmt);
    sqlite3_bind_int64(stmt, 1, source_id);
    int rc = SQLITE_OK;
    while ((rc = step_or_throw(db_, stmt, "get_all_edges_from")) == SQLITE_ROW) {
        Edge e;
        e.id = sqlite3_column_int64(stmt, 0);
        e.source_id = sqlite3_column_int64(stmt, 1);
        e.target_id = sqlite3_column_int64(stmt, 2);
        e.kind = static_cast<EdgeKind>(sqlite3_column_int(stmt, 3));
        e.line = sqlite3_column_int(stmt, 4);
        e.col = sqlite3_column_int(stmt, 5);
        e.metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6) ? sqlite3_column_text(stmt, 6) : (const unsigned char*)"");
        results.push_back(e);
    }
    return results;
}

/** 统计边总数。 */
int64_t Database::count_edges() {
    sqlite3_stmt* stmt = prepare_or_throw(db_, "SELECT COUNT(*) FROM edges", "prepare count_edges");
    StmtGuard guard(stmt);
    if (step_or_throw(db_, stmt, "count_edges") != SQLITE_ROW) {
        throw std::runtime_error("count_edges: no result row");
    }
    return sqlite3_column_int64(stmt, 0);
}

// ── 文件操作 ──

/**
 * 插入或更新文件记录。
 * INSERT OR REPLACE：如果 path 已存在则更新 mtime/size。
 */
int64_t Database::insert_file(const FileRecord& file) {
    const char* sql = "INSERT OR REPLACE INTO files (path, language, mtime, size) VALUES (?, ?, ?, ?)";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare insert_file");
    StmtGuard guard(stmt);
    sqlite3_bind_text(stmt, 1, file.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, file.language.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, file.mtime);
    sqlite3_bind_int64(stmt, 4, file.size);
    if (sqlite3_step(stmt) != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(db_);
}

/** 按路径获取文件记录。 */
std::optional<FileRecord> Database::get_file(const std::string& path) {
    const char* sql = "SELECT id, path, language, mtime, size FROM files WHERE path=?";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare get_file");
    StmtGuard guard(stmt);
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    int rc = step_or_throw(db_, stmt, "get_file");
    if (rc != SQLITE_ROW) return std::nullopt;
    FileRecord f;
    f.id = sqlite3_column_int64(stmt, 0);
    f.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    f.language = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2) ? sqlite3_column_text(stmt, 2) : (const unsigned char*)"");
    f.mtime = sqlite3_column_int64(stmt, 3);
    f.size = sqlite3_column_int64(stmt, 4);
    return f;
}

/** 获取所有已索引的文件记录。 */
std::vector<FileRecord> Database::get_all_files() {
    std::vector<FileRecord> results;
    const char* sql = "SELECT id, path, language, mtime, size FROM files";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare get_all_files");
    StmtGuard guard(stmt);
    int rc = SQLITE_OK;
    while ((rc = step_or_throw(db_, stmt, "get_all_files")) == SQLITE_ROW) {
        FileRecord f;
        f.id = sqlite3_column_int64(stmt, 0);
        f.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        f.language = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2) ? sqlite3_column_text(stmt, 2) : (const unsigned char*)"");
        f.mtime = sqlite3_column_int64(stmt, 3);
        f.size = sqlite3_column_int64(stmt, 4);
        results.push_back(f);
    }
    return results;
}

/** 更新文件的修改时间（增量索引时用）。 */
void Database::update_file_mtime(const std::string& path, int64_t mtime) {
    const char* sql = "UPDATE files SET mtime=? WHERE path=?";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare update_file_mtime");
    StmtGuard guard(stmt);
    sqlite3_bind_int64(stmt, 1, mtime);
    sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    expect_done(db_, stmt, "update_file_mtime");
}

// ── 删除操作（增量索引用） ──

/**
 * 删除某文件所有节点的关联边。
 * 增量索引时，先删边再删节点（外键约束）。
 * 包括该文件中的节点作为 source 或 target 的所有边。
 */
void Database::delete_edges_for_file_nodes(const std::string& file_path) {
    const char* sql = R"(DELETE FROM edges WHERE source_id IN (SELECT id FROM nodes WHERE file_path=?)
        OR target_id IN (SELECT id FROM nodes WHERE file_path=?))";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare delete_edges_for_file_nodes");
    StmtGuard guard(stmt);
    sqlite3_bind_text(stmt, 1, file_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, file_path.c_str(), -1, SQLITE_TRANSIENT);
    expect_done(db_, stmt, "delete_edges_for_file_nodes");
}

/** 删除某文件的所有节点。 */
void Database::delete_nodes_by_file(const std::string& file_path) {
    const char* sql = "DELETE FROM nodes WHERE file_path=?";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare delete_nodes_by_file");
    StmtGuard guard(stmt);
    sqlite3_bind_text(stmt, 1, file_path.c_str(), -1, SQLITE_TRANSIENT);
    expect_done(db_, stmt, "delete_nodes_by_file");
}

/** 删除某文件节点产生的未解析引用。 */
void Database::delete_unresolved_refs_by_file(const std::string& file_path) {
    const char* sql = R"(DELETE FROM unresolved_refs WHERE source_node_id IN (SELECT id FROM nodes WHERE file_path=?))";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare delete_unresolved_refs_by_file");
    StmtGuard guard(stmt);
    sqlite3_bind_text(stmt, 1, file_path.c_str(), -1, SQLITE_TRANSIENT);
    expect_done(db_, stmt, "delete_unresolved_refs_by_file");
}

/** 删除文件记录。 */
void Database::delete_file(const std::string& file_path) {
    const char* sql = "DELETE FROM files WHERE path=?";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare delete_file");
    StmtGuard guard(stmt);
    sqlite3_bind_text(stmt, 1, file_path.c_str(), -1, SQLITE_TRANSIENT);
    expect_done(db_, stmt, "delete_file");
}

// ── 未解析引用操作 ──

/** 插入一条未解析引用。 */
int64_t Database::insert_unresolved_ref(const UnresolvedRef& ref) {
    const char* sql = "INSERT INTO unresolved_refs (source_node_id, ref_name, ref_kind, line, col) VALUES (?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare insert_unresolved_ref");
    StmtGuard guard(stmt);
    sqlite3_bind_int64(stmt, 1, ref.source_node_id);
    sqlite3_bind_text(stmt, 2, ref.ref_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, ref.ref_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, ref.line);
    sqlite3_bind_int(stmt, 5, ref.col);
    if (sqlite3_step(stmt) != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(db_);
}

/** 获取所有未解析引用。 */
std::vector<UnresolvedRef> Database::get_unresolved_refs() {
    std::vector<UnresolvedRef> results;
    const char* sql = "SELECT id, source_node_id, ref_name, ref_kind, line, col FROM unresolved_refs";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare get_unresolved_refs");
    StmtGuard guard(stmt);
    int rc = SQLITE_OK;
    while ((rc = step_or_throw(db_, stmt, "get_unresolved_refs")) == SQLITE_ROW) {
        UnresolvedRef r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.source_node_id = sqlite3_column_int64(stmt, 1);
        r.ref_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        r.ref_kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3) ? sqlite3_column_text(stmt, 3) : (const unsigned char*)"");
        r.line = sqlite3_column_int(stmt, 4);
        r.col = sqlite3_column_int(stmt, 5);
        results.push_back(r);
    }
    return results;
}

/** 删除单条未解析引用（解析成功后调用）。 */
void Database::delete_unresolved_ref(int64_t ref_id) {
    const char* sql = "DELETE FROM unresolved_refs WHERE id=?";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare delete_unresolved_ref");
    StmtGuard guard(stmt);
    sqlite3_bind_int64(stmt, 1, ref_id);
    expect_done(db_, stmt, "delete_unresolved_ref");
}

// ── 批量查询 ──

/**
 * 按 ID 列表批量获取节点。
 * 使用 IN (?) 占位符动态构建 SQL。
 * 用于图遍历时批量获取邻居节点。
 */
std::vector<Node> Database::get_nodes_by_ids(const std::vector<int64_t>& ids) {
    std::vector<Node> results;
    if (ids.empty()) return results;

    std::string placeholders;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) placeholders += ",";
        placeholders += "?";
    }

    std::string sql = "SELECT id, kind, name, qualified_name, file_path, language, "
                      "line, col, end_line, end_col, signature, docstring, "
                      "visibility, is_static, is_const, is_exported "
                      "FROM nodes WHERE id IN (" + placeholders + ")";

    sqlite3_stmt* stmt = prepare_or_throw(db_, sql.c_str(), "prepare get_nodes_by_ids");
    StmtGuard guard(stmt);
    for (size_t i = 0; i < ids.size(); ++i) {
        sqlite3_bind_int64(stmt, static_cast<int>(i + 1), ids[i]);
    }
    int rc = SQLITE_OK;
    while ((rc = step_or_throw(db_, stmt, "get_nodes_by_ids")) == SQLITE_ROW) {
        results.push_back(read_node_row(stmt));
    }
    return results;
}

// ── 全文搜索 ──

/**
 * FTS5 全文搜索。
 *
 * 流程：
 *   1. 将用户查询转为安全的 FTS5 查询（处理特殊字符）
 *   2. 查询 nodes_fts 虚表，用 bm25() 排序
 *   3. 结果按 kind 重排序（函数/类 > 变量 > import）
 *   4. 截断到 limit 条
 *
 * 为什么 over-fetch 3x：
 *   bm25() 按文本相关性排序，但我们需要按 kind 优先级排序。
 *   多取一些结果，排序后再截断，避免遗漏重要的 kind。
 *
 * kind 重排序策略：
 *   rank 0: Function, Method, Class, Struct, Enum, Namespace（最重要的符号）
 *   rank 1: Variable, TypeAlias, EnumMember, Field（次要符号）
 *   rank 2: 其他（Import 等）
 */
std::vector<Node> Database::search_fts(const std::string& query, int limit) {
    std::vector<Node> results;
    const std::string fts_query = make_fts_query(query);
    if (fts_query.empty()) return results;

    // Over-fetch by 3x so kind-based re-ranking has room to reorder
    int fetch_limit = limit * 3;
    const char* sql = R"(SELECT n.id, n.kind, n.name, n.qualified_name, n.file_path, n.language, n.line, n.col, n.end_line, n.end_col, n.signature, n.docstring, n.visibility, n.is_static, n.is_const, n.is_exported
        FROM nodes_fts f JOIN nodes n ON f.rowid = n.id
        WHERE nodes_fts MATCH ? ORDER BY bm25(nodes_fts) LIMIT ?)";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare search_fts");
    StmtGuard guard(stmt);
    sqlite3_bind_text(stmt, 1, fts_query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, fetch_limit);
    int rc = SQLITE_OK;
    while ((rc = step_or_throw(db_, stmt, "search_fts")) == SQLITE_ROW) {
        results.push_back(read_node_row(stmt));
    }

    // Kind-based re-ranking: Functions/Classes first, then Variables, last Imports
    std::sort(results.begin(), results.end(), [](const Node& a, const Node& b) {
        auto kind_rank = [](NodeKind k) -> int {
            switch (k) {
                case NodeKind::Function:
                case NodeKind::Method:
                case NodeKind::Class:
                case NodeKind::Struct:
                case NodeKind::Enum:
                case NodeKind::Namespace:
                    return 0;
                case NodeKind::Variable:
                case NodeKind::TypeAlias:
                case NodeKind::EnumMember:
                case NodeKind::Field:
                    return 1;
                default:
                    return 2;
            }
        };
        int ra = kind_rank(a.kind), rb = kind_rank(b.kind);
        if (ra != rb) return ra < rb;
        return a.id < b.id;
    });

    if (static_cast<int>(results.size()) > limit) {
        results.resize(limit);
    }
    return results;
}

// ── 死代码检测 ──

/**
 * 查找可能的死代码（没有被任何其他函数调用的函数/方法）。
 *
 * 排除条件（避免误报）：
 *   1. 只检测函数/方法（kind 1, 2），不检测类型/类
 *   2. 排除 main() 入口点
 *   3. 排除头文件中的函数（公共 API）
 *   4. 排除测试/基准测试/演示文件
 *   5. 排除构建目录和 protobuf 文件
 *   6. 排除有 qualified_name 的方法（类成员，通常被外部调用）
 *   7. 排除常见名字（如 lock、guard、buffer 等局部变量名）
 *   8. 排除析构函数（~ClassName）
 *   9. 必须没有任何入边（除了 "contains" 边）
 */
std::vector<Node> Database::find_dead_code(const std::string& exclude_pattern) {
    std::string sql = R"(
        SELECT id, kind, name, qualified_name, file_path, language,
               line, col, end_line, end_col, signature, docstring,
               visibility, is_static, is_const, is_exported
        FROM nodes
        WHERE kind IN (1, 2)
        AND name != ''
        AND name != 'main'
        AND name NOT LIKE '%::main'
        -- 排除有 qualified_name 的方法（类成员，肯定被调用）
        AND (qualified_name IS NULL OR qualified_name = '' OR qualified_name = name)
        AND name NOT IN (
            'lock', 'guard', 'write_lock', 'lk', 'buffer', 'db', 'svc',
            'arg', 'lock_guard', 'unique_lock', 'scoped_lock',
            'sf', 'tf', 'cf', 'epsf', 'csf', 'base', 'out',
            'futures', 'send_times', 'locals', 'guard',
            'await_ready', 'await_suspend', 'await_resume',
            'operator()', 'operator co_await',
            'get_return_object', 'initial_suspend', 'final_suspend',
            'return_value', 'return_void', 'unhandled_exception',
            'promise_type', 'FinalAwaiter', 'Awaiter'
        )
        AND name NOT LIKE '~%'  -- 析构函数
        AND name NOT LIKE '%::%::await_%'  -- await 方法
        AND file_path NOT LIKE '%build%'
        AND file_path NOT LIKE '%.pb.%'
        AND file_path NOT LIKE '%.h'
        AND file_path NOT LIKE '%.hpp'
        AND file_path NOT LIKE '%tests/%'
        AND file_path NOT LIKE '%test/%'
        AND file_path NOT LIKE '%benchmarks/%'
        AND file_path NOT LIKE '%demo/%'
        AND file_path NOT LIKE '%example%'
        AND NOT EXISTS (
            SELECT 1 FROM edges e
            WHERE e.target_id = nodes.id
            AND e.kind != 0
        )
        ORDER BY file_path, line
    )";

    sqlite3_stmt* stmt = prepare_or_throw(db_, sql.c_str(), "prepare find_dead_code");
    StmtGuard guard(stmt);

    std::vector<Node> results;
    int rc = SQLITE_OK;
    while ((rc = step_or_throw(db_, stmt, "find_dead_code")) == SQLITE_ROW) {
        results.push_back(read_node_row(stmt));
    }
    return results;
}

/** 统计已索引的文件数。 */
int64_t Database::count_files() {
    sqlite3_stmt* stmt = prepare_or_throw(db_, "SELECT COUNT(*) FROM files", "prepare count_files");
    StmtGuard guard(stmt);
    if (step_or_throw(db_, stmt, "count_files") != SQLITE_ROW) {
        throw std::runtime_error("count_files: no result row");
    }
    return sqlite3_column_int64(stmt, 0);
}

// ── 批量操作（复用 prepared statement，避免重复编译） ──

/**
 * bind_node：将 Node 对象绑定到 prepared statement 的参数。
 * 复用 stmt 时，先 reset() 清除上一次的状态，再 clear_bindings() 清除旧绑定。
 */
static void bind_node(sqlite3_stmt* stmt, const Node& node) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int(stmt, 1, static_cast<int>(node.kind));
    sqlite3_bind_text(stmt, 2, node.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, node.qualified_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, node.file_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, node.language.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, node.line);
    sqlite3_bind_int(stmt, 7, node.col);
    sqlite3_bind_int(stmt, 8, node.end_line);
    sqlite3_bind_int(stmt, 9, node.end_col);
    sqlite3_bind_text(stmt, 10, node.signature.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, node.docstring.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, node.visibility.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 13, node.is_static ? 1 : 0);
    sqlite3_bind_int(stmt, 14, node.is_const ? 1 : 0);
    sqlite3_bind_int(stmt, 15, node.is_exported ? 1 : 0);
}

/**
 * 批量插入节点。
 *
 * 性能优化：
 *   - prepared statement 只编译一次，后续复用
 *   - 每次调用 bind_node() → step() → 收集返回的 rowid
 *   - out_ids 用于后续构建 edge 时映射临时 ID 到真实 ID
 *
 * 为什么不用事务包裹：
 *   调用方（index_extracted_files）已经在事务中，这里不需要嵌套。
 */
void Database::insert_nodes_batch(const std::vector<Node>& nodes, std::vector<int64_t>& out_ids) {
    if (nodes.empty()) return;
    if (!stmt_insert_node_) {
        const char* sql = R"(INSERT INTO nodes
            (kind, name, qualified_name, file_path, language, line, col, end_line, end_col, signature, docstring, visibility, is_static, is_const, is_exported)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?))";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt_insert_node_, nullptr) != SQLITE_OK) {
            throw sqlite_error(db_, "prepare insert_nodes_batch");
        }
    }
    out_ids.reserve(nodes.size());
    for (const auto& node : nodes) {
        bind_node(stmt_insert_node_, node);
        if (sqlite3_step(stmt_insert_node_) != SQLITE_DONE) {
            out_ids.push_back(-1);
        } else {
            out_ids.push_back(sqlite3_last_insert_rowid(db_));
        }
    }
}

/**
 * 批量插入边。
 * 同样复用 prepared statement，每个 edge 绑定 → step → 继续。
 */
void Database::insert_edges_batch(const std::vector<Edge>& edges) {
    if (edges.empty()) return;
    if (!stmt_insert_edge_) {
        const char* sql = "INSERT INTO edges (source_id, target_id, kind, line, col, metadata) VALUES (?, ?, ?, ?, ?, ?)";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt_insert_edge_, nullptr) != SQLITE_OK) {
            throw sqlite_error(db_, "prepare insert_edges_batch");
        }
    }
    for (const auto& edge : edges) {
        sqlite3_reset(stmt_insert_edge_);
        sqlite3_clear_bindings(stmt_insert_edge_);
        sqlite3_bind_int64(stmt_insert_edge_, 1, edge.source_id);
        sqlite3_bind_int64(stmt_insert_edge_, 2, edge.target_id);
        sqlite3_bind_int(stmt_insert_edge_, 3, static_cast<int>(edge.kind));
        sqlite3_bind_int(stmt_insert_edge_, 4, edge.line);
        sqlite3_bind_int(stmt_insert_edge_, 5, edge.col);
        sqlite3_bind_text(stmt_insert_edge_, 6, edge.metadata.c_str(), -1, SQLITE_TRANSIENT);
        expect_done(db_, stmt_insert_edge_, "insert_edges_batch");
    }
}

/**
 * 批量插入未解析引用。
 */
void Database::insert_unresolved_batch(const std::vector<UnresolvedRef>& refs) {
    if (refs.empty()) return;
    if (!stmt_insert_unresolved_) {
        const char* sql = "INSERT INTO unresolved_refs (source_node_id, ref_name, ref_kind, line, col) VALUES (?, ?, ?, ?, ?)";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt_insert_unresolved_, nullptr) != SQLITE_OK) {
            throw sqlite_error(db_, "prepare insert_unresolved_batch");
        }
    }
    for (const auto& ref : refs) {
        sqlite3_reset(stmt_insert_unresolved_);
        sqlite3_clear_bindings(stmt_insert_unresolved_);
        sqlite3_bind_int64(stmt_insert_unresolved_, 1, ref.source_node_id);
        sqlite3_bind_text(stmt_insert_unresolved_, 2, ref.ref_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt_insert_unresolved_, 3, ref.ref_kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt_insert_unresolved_, 4, ref.line);
        sqlite3_bind_int(stmt_insert_unresolved_, 5, ref.col);
        expect_done(db_, stmt_insert_unresolved_, "insert_unresolved_batch");
    }
}

/**
 * 批量删除已解析的未解析引用。
 * resolve_unresolved_refs() 解析成功后，删除对应的 unresolved_refs 记录。
 */
void Database::delete_unresolved_refs_batch(const std::vector<int64_t>& ref_ids) {
    if (ref_ids.empty()) return;
    const char* sql = "DELETE FROM unresolved_refs WHERE id=?";
    sqlite3_stmt* stmt = prepare_or_throw(db_, sql, "prepare delete_unresolved_refs_batch");
    StmtGuard guard(stmt);
    for (int64_t id : ref_ids) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int64(stmt, 1, id);
        expect_done(db_, stmt, "delete_unresolved_refs_batch");
    }
}

}  // namespace codegraph
