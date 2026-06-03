/**
 * types.cpp — 类型枚举的字符串转换实现
 *
 * 本文件提供 NodeKind 和 EdgeKind 到字符串的映射，
 * 用于 JSON 序列化、日志输出和 DOT 图导出。
 *
 * 设计要点：
 *   - 使用 switch-case 而非 map，避免运行时哈希开销
 *   - 枚举值和字符串的对应关系必须与 types.h 中的定义严格一致
 *   - 返回 "unknown" 作为兜底，确保不会因未知枚举值崩溃
 */

#include "codegraph/core/types.h"

namespace codegraph {

/**
 * NodeKind → 字符串
 *
 * 用途：
 *   - JSON 输出中的 "kind" 字段（如 {"kind": "function"}）
 *   - DOT 图中的节点标签
 *   - CLI 输出中的类型标识
 *
 * 枚举值定义见 types.h：
 *   File=0, Function=1, Method=2, Class=3, Struct=4,
 *   Enum=5, EnumMember=6, Variable=7, TypeAlias=8,
 *   Namespace=9, Import=10, Parameter=11, Field=12
 */
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

/**
 * EdgeKind → 字符串
 *
 * 用途：
 *   - JSON 输出中的 "kind" 字段（如 {"kind": "calls"}）
 *   - DOT 图中的边标签
 *
 * 枚举值定义见 types.h：
 *   Contains=0, Calls=1, Imports=2, Exports=3,
 *   Extends=4, Implements=5, References=6,
 *   TypeOf=7, Returns=8, Overrides=9
 */
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
