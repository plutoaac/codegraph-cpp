#include "codegraph/core/types.h"

namespace codegraph {

const char* node_kind_str(NodeKind kind) {
    switch (kind) {
        case NodeKind::File:       return "file";
        case NodeKind::Function:   return "function";
        case NodeKind::Method:     return "method";
        case NodeKind::Class:      return "class";
        case NodeKind::Struct:     return "struct";
        case NodeKind::Enum:       return "enum";
        case NodeKind::EnumMember: return "enum_member";
        case NodeKind::Variable:   return "variable";
        case NodeKind::TypeAlias:  return "type_alias";
        case NodeKind::Namespace:  return "namespace";
        case NodeKind::Import:     return "import";
        case NodeKind::Parameter:  return "parameter";
        case NodeKind::Field:      return "field";
    }
    return "unknown";
}

const char* edge_kind_str(EdgeKind kind) {
    switch (kind) {
        case EdgeKind::Contains:   return "contains";
        case EdgeKind::Calls:      return "calls";
        case EdgeKind::Imports:    return "imports";
        case EdgeKind::Exports:    return "exports";
        case EdgeKind::Extends:    return "extends";
        case EdgeKind::Implements: return "implements";
        case EdgeKind::References: return "references";
        case EdgeKind::TypeOf:     return "type_of";
        case EdgeKind::Returns:    return "returns";
        case EdgeKind::Overrides:  return "overrides";
    }
    return "unknown";
}

}  // namespace codegraph
