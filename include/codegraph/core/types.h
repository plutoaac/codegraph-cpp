#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace codegraph {

// ── Node kinds ──
enum class NodeKind : int {
    File        = 0,
    Function    = 1,
    Method      = 2,
    Class       = 3,
    Struct      = 4,
    Enum        = 5,
    EnumMember  = 6,
    Variable    = 7,
    TypeAlias   = 8,
    Namespace   = 9,
    Import      = 10,
    Parameter   = 11,
    Field       = 12,
};

const char* node_kind_str(NodeKind kind);

// ── Edge kinds ──
enum class EdgeKind : int {
    Contains    = 0,
    Calls       = 1,
    Imports     = 2,
    Exports     = 3,
    Extends     = 4,
    Implements  = 5,
    References  = 6,
    TypeOf      = 7,
    Returns     = 8,
    Overrides   = 9,
};

const char* edge_kind_str(EdgeKind kind);

// ── Node ──
struct Node {
    int64_t id = 0;
    NodeKind kind = NodeKind::Function;
    std::string name;
    std::string qualified_name;
    std::string file_path;
    std::string language;
    int line = 0;
    int col = 0;
    int end_line = 0;
    int end_col = 0;
    std::string signature;
    std::string docstring;
    std::string visibility;   // public/private/protected
    bool is_static = false;
    bool is_const = false;
    bool is_exported = false;
};

// ── Edge ──
struct Edge {
    int64_t id = 0;
    int64_t source_id = 0;
    int64_t target_id = 0;
    EdgeKind kind = EdgeKind::Calls;
    int line = 0;
    int col = 0;
    std::string metadata;
};

// ── File record ──
struct FileRecord {
    int64_t id = 0;
    std::string path;
    std::string language;
    int64_t mtime = 0;
    int64_t size = 0;
};

// ── Unresolved reference ──
struct UnresolvedRef {
    int64_t id = 0;
    int64_t source_node_id = 0;
    std::string ref_name;
    std::string ref_kind;
    int line = 0;
    int col = 0;
};

}  // namespace codegraph
