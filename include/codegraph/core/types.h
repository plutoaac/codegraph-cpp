/**
 * types.h — codegraph 的核心数据类型定义
 *
 * 本文件定义了 codegraph 中所有模块共享的基础数据结构：
 *   - NodeKind: 节点类型枚举（函数、类、变量等）
 *   - EdgeKind: 边类型枚举（调用、包含、继承等）
 *   - Node: 代码符号节点（对应数据库 nodes 表）
 *   - Edge: 符号间关系（对应数据库 edges 表）
 *   - FileRecord: 文件记录（对应数据库 files 表）
 *   - UnresolvedRef: 未解析引用（对应数据库 unresolved_refs 表）
 *
 * 设计要点：
 *   - 所有结构体都是 POD（Plain Old Data），可以直接序列化到数据库
 *   - 枚举使用 enum class（强类型），避免隐式转换
 *   - id 字段默认为 0，插入数据库后由 autoincrement 分配真实 ID
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace codegraph {

/**
 * 节点类型枚举。
 *
 * 对应代码中的各种语法结构：
 *   File(0)       — 源文件（每个文件一个 File 节点，作为 contains 边的起点）
 *   Function(1)   — 自由函数（不在类中的函数）
 *   Method(2)     — 类方法（在类中的函数）
 *   Class(3)      — class 定义
 *   Struct(4)     — struct 定义
 *   Enum(5)       — enum 定义
 *   EnumMember(6) — enum 值
 *   Variable(7)   — 变量声明
 *   TypeAlias(8)  — typedef / using
 *   Namespace(9)  — namespace 定义
 *   Import(10)    — #include / import 语句
 *   Parameter(11) — 函数参数
 *   Field(12)     — 结构体/类的成员变量
 *
 * 注意：枚举值的顺序是稳定的，数据库中存储的是整数值。
 *       不要在中间插入新值，只能在末尾追加。
 */
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

/** 将 NodeKind 转为可读字符串（如 "function"、"class"）。 */
const char* node_kind_str(NodeKind kind);

/**
 * 边类型枚举。
 *
 * 描述两个节点之间的关系：
 *   Contains(0)   — 文件包含符号（File → Function/Class/...）
 *   Calls(1)      — 函数调用（caller → callee）
 *   Imports(2)    — 导入关系
 *   Exports(3)    — 导出关系
 *   Extends(4)    — 继承（子类 → 父类）
 *   Implements(5) — 实现接口
 *   References(6) — 引用（非直接调用，如函数指针、模板实例化）
 *   TypeOf(7)     — 类型关系
 *   Returns(8)    — 返回值类型
 *   Overrides(9)  — 重写父类方法
 *
 * 最常用的是 Contains（文件→符号）和 Calls（调用关系）。
 */
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

/** 将 EdgeKind 转为可读字符串（如 "calls"、"contains"）。 */
const char* edge_kind_str(EdgeKind kind);

/**
 * 代码符号节点。
 *
 * 对应数据库 nodes 表，存储从源代码中提取的每个符号的信息。
 *
 * 字段说明：
 *   id:            数据库自增主键（提取阶段为 0，插入后分配真实 ID）
 *   kind:          节点类型（NodeKind 枚举）
 *   name:          符号名（如 "foo"、"MyClass"）
 *   qualified_name: 限定名（如 "ns::MyClass::foo"）
 *   file_path:     所在文件路径
 *   language:      语言标识（"cpp" 或 "python"）
 *   line/col:      起始位置（行号/列号，从 1 开始）
 *   end_line/end_col: 结束位置
 *   signature:     函数签名（如 "void foo(int x, std::string s)"）
 *   docstring:     文档注释
 *   visibility:    访问控制（"public"/"private"/"protected"）
 *   is_static:     是否为 static
 *   is_const:      是否为 const
 *   is_exported:   是否导出（Python 装饰器、export 等）
 */
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

/**
 * 符号间关系（边）。
 *
 * 对应数据库 edges 表，存储两个节点之间的关系。
 *
 * 字段说明：
 *   id:         数据库自增主键
 *   source_id:  源节点 ID（如调用者）
 *   target_id:  目标节点 ID（如被调用者）
 *   kind:       边类型（EdgeKind 枚举）
 *   line/col:   关系发生的位置（如调用所在的行号）
 *   metadata:   额外元数据（JSON 字符串，当前未使用）
 */
struct Edge {
    int64_t id = 0;
    int64_t source_id = 0;
    int64_t target_id = 0;
    EdgeKind kind = EdgeKind::Calls;
    int line = 0;
    int col = 0;
    std::string metadata;
};

/**
 * 文件记录。
 *
 * 对应数据库 files 表，记录已索引的文件信息。
 * 用于增量索引：比较 mtime/size 判断文件是否变更。
 */
struct FileRecord {
    int64_t id = 0;
    std::string path;
    std::string language;
    int64_t mtime = 0;  // 最后修改时间（Unix 时间戳，秒）
    int64_t size = 0;   // 文件大小（字节）
};

/**
 * 未解析引用。
 *
 * 对应数据库 unresolved_refs 表。
 * 第一遍索引时，跨文件的调用目标可能还未被索引，
 * 先存为 UnresolvedRef，后续由 resolve 命令统一解析为 Edge。
 */
struct UnresolvedRef {
    int64_t id = 0;
    int64_t source_node_id = 0;  // 引用者（调用者）的节点 ID
    std::string ref_name;        // 被引用的符号名
    std::string ref_kind;        // 引用类型（"call"）
    int line = 0;
    int col = 0;
};

}  // namespace codegraph
