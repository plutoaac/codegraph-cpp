#pragma once

#include "codegraph/core/types.h"
#include <sqlite3.h>
#include <optional>
#include <string>
#include <vector>

namespace codegraph {

class Database {
public:
    Database(const std::string& db_path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    void init_schema();
    void begin_transaction();
    void commit();
    void rollback();

    // Nodes
    int64_t insert_node(const Node& node);
    std::optional<Node> get_node(int64_t id);
    std::vector<Node> find_nodes_by_name(const std::string& name, int limit = 20);
    std::vector<Node> find_nodes_by_file(const std::string& file_path);
    int64_t count_nodes();

    // Edges
    int64_t insert_edge(const Edge& edge);
    std::vector<Edge> get_edges_from(int64_t source_id, EdgeKind kind);
    std::vector<Edge> get_edges_to(int64_t target_id, EdgeKind kind);
    std::vector<Edge> get_all_edges_from(int64_t source_id);
    int64_t count_edges();

    // Files
    int64_t insert_file(const FileRecord& file);
    std::optional<FileRecord> get_file(const std::string& path);
    std::vector<FileRecord> get_all_files();
    void update_file_mtime(const std::string& path, int64_t mtime);

    // Cleanup for re-indexing
    void delete_nodes_by_file(const std::string& file_path);
    void delete_edges_for_file_nodes(const std::string& file_path);
    void delete_unresolved_refs_by_file(const std::string& file_path);
    void delete_file(const std::string& file_path);

    // Unresolved refs
    int64_t insert_unresolved_ref(const UnresolvedRef& ref);
    std::vector<UnresolvedRef> get_unresolved_refs();
    void delete_unresolved_ref(int64_t ref_id);

    // Batch node lookup
    std::vector<Node> get_nodes_by_ids(const std::vector<int64_t>& ids);

    // Batch insert (reuses prepared statements for performance)
    void insert_nodes_batch(const std::vector<Node>& nodes, std::vector<int64_t>& out_ids);
    void insert_edges_batch(const std::vector<Edge>& edges);
    void insert_unresolved_batch(const std::vector<UnresolvedRef>& refs);
    void delete_unresolved_refs_batch(const std::vector<int64_t>& ref_ids);

    // FTS search
    std::vector<Node> search_fts(const std::string& query, int limit = 20);

    // Dead code detection
    std::vector<Node> find_dead_code(const std::string& exclude_pattern = "");

    // Stats
    int64_t count_files();

private:
    sqlite3* db_ = nullptr;
    void exec(const char* sql);
    Node read_node_row(sqlite3_stmt* stmt);

    // Cached prepared statements for batch operations
    sqlite3_stmt* stmt_insert_node_ = nullptr;
    sqlite3_stmt* stmt_insert_edge_ = nullptr;
    sqlite3_stmt* stmt_insert_unresolved_ = nullptr;
    void finalize_cached_stmts();
};

}  // namespace codegraph
