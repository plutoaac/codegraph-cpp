/**
 * cpp_extractor.cpp — C++ 和 Python 语言提取器实现
 *
 * 本文件是 codegraph 的核心模块，负责将源代码解析为结构化的节点（Node）和边（Edge）。
 *
 * 整体架构：
 *   源代码 → tree-sitter 解析 → AST 遍历 → 提取节点（函数/类/变量）+ 边（调用关系）
 *
 * 提取流程：
 *   1. tree-sitter 将源代码解析为 AST（抽象语法树）
 *   2. 递归遍历 AST，识别感兴趣的节点类型（函数、类、结构体等）
 *   3. 对每个函数/方法，递归搜索其函数体中的 call_expression，提取调用关系
 *   4. 调用关系先存为 UnresolvedRef（未解析引用），后续由 main.cpp 统一解析为 Edge
 *
 * 设计要点：
 *   - tree-sitter 只负责解析，不负责语义分析（不知道类型信息）
 *   - 调用目标用名字匹配，而不是类型推导（这是和 clang 的根本区别）
 *   - 为了过滤标准库/系统调用噪声，维护了一个 builtin 符号黑名单
 *   - field_expression（obj.method()）不过滤，因为可能是用户自定义方法
 *
 * tree-sitter API 速览：
 *   - ts_parser_parse_string()  → 解析源代码为语法树
 *   - ts_tree_root_node()       → 获取 AST 根节点
 *   - ts_node_type()            → 获取节点类型名（如 "function_definition"）
 *   - ts_node_named_child()     → 获取命名子节点（排除标点等匿名节点）
 *   - ts_node_child_by_field_name() → 按字段名获取子节点（如 "name"、"body"）
 *   - ts_node_start_byte()      → 节点在源代码中的起始字节偏移
 *   - ts_node_end_byte()        → 节点在源代码中的结束字节偏移
 *   - ts_node_start_point()     → 节点的起始行列号（TSPoint）
 */

#include "codegraph/extraction/extractor.h"
#include <tree_sitter/api.h>
#include <cstring>
#include <functional>
#include <unordered_set>

// ── tree-sitter 语言函数的前向声明 ──
// tree-sitter 每种语言是一个独立的 C 库，编译时链接。
// tree_sitter_cpp() 和 tree_sitter_python() 是各自语言的入口函数，
// 返回 TSLanguage* 指针，用于配置 parser。
extern "C" TSLanguage* tree_sitter_cpp();
extern "C" TSLanguage* tree_sitter_python();

namespace codegraph {

/**
 * 判断符号是否为内置/标准库符号，应被过滤。
 *
 * 为什么需要过滤：
 *   tree-sitter 没有类型信息，无法区分 `sort()` 是 std::sort 还是用户定义的 sort。
 *   如果不过滤，调用图中会充满 std::sort、printf、memcpy 等噪声节点。
 *
 * 过滤策略：
 *   1. 模板实例化名以 '>' 结尾（如 vector<int>），直接过滤
 *   2. 维护一个静态集合，包含常见的 C++ 标准库、POSIX 系统调用、protobuf 内部符号
 *
 * 为什么不完美：
 *   - 用户如果定义了同名函数（如自己的 sort()），也会被过滤
 *   - 这是 trade-off：宁可漏掉一些边界情况，也不能让调用图充满噪声
 */
static bool is_builtin_symbol(const std::string& name) {
    // 模板实例化名以 '>' 结尾，如 "vector<int>"、"shared_ptr<Foo>"
    if (!name.empty() && name.back() == '>') return true;

    // 内置符号黑名单：C++ 标准库 + POSIX + protobuf 内部
    static const std::unordered_set<std::string> builtins = {
        // ── C++ 类型转换和运算符 ──
        "static_cast", "dynamic_cast", "reinterpret_cast", "const_cast",
        "sizeof", "alignof", "typeid", "noexcept",
        // ── C++ 基础类型 ──
        "size_t", "int8_t", "uint8_t", "int16_t", "uint16_t",
        "int32_t", "uint32_t", "int64_t", "uint64_t",
        "intptr_t", "uintptr_t", "ptrdiff_t", "nullptr_t",
        // ── C++ 标准库函数 ──
        "to_string", "sort", "min", "max", "swap", "move", "forward",
        "make_shared", "make_unique", "memcpy", "memset", "memmove",
        "fprintf", "printf", "snprintf", "abort", "exit",
        "setw", "setprecision", "setfill", "left", "right",
        // ── STL 容器迭代器 ──
        "begin", "end", "rbegin", "rend", "data", "size", "empty",
        // ── std::variant/optional/any ──
        "get", "holds_alternative", "visit", "any_cast",
        // ── 字符串转换 ──
        "stoi", "stol", "stoul", "stod", "stof",
        "atoi", "atol", "strtol", "strtoul", "strtod",
        // ── POSIX / Linux 系统调用 ──
        "sleep_for", "sleep_until",
        "strerror", "perror",
        "waitpid", "fork", "exec", "execv", "execvp",
        "kill", "getpid", "getppid",
        // ── Socket API ──
        "socket", "setsockopt", "getsockopt", "bind", "listen",
        "accept", "accept4", "connect", "close", "shutdown",
        "send", "recv", "sendto", "recvfrom",
        "htons", "htonl", "ntohs", "ntohl",
        "inet_pton", "inet_ntop", "inet_addr",
        // ── epoll / fcntl ──
        "epoll_create", "epoll_ctl", "epoll_wait",
        "fcntl", "ioctl", "pipe", "pipe2", "socketpair",
        // ── 文件 IO ──
        "open", "read", "write", "lseek", "dup", "dup2",
        "mmap", "munmap", "mprotect",
        // ── 进程状态宏 ──
        "WEXITSTATUS", "WIFEXITED", "WIFSIGNALED", "WTERMSIG",
        "eventfd", "timerfd_create", "signalfd",
        // ── protobuf 内部符号 ──
        "GetEmptyStringAlreadyInited", "InitSCC", "GetArena",
        "GetArenaNoVirtual", "GetCachedSize", "ByteSizeLong", "Clear",
        "_Internal", "default_instance_", "InitDefaultsImpl",
        "GetProto3PreserveUnknownsDefault", "VerifyUtf8String",
        "ComputeUnknownFieldsSize", "SerializeUnknownFields",
        "SerializeUnknownFieldsToArray", "DynamicCastToGenerated",
        "CreateMaybeMessage", "OnShutdownDestroyMessage",
        "InternalAddGeneratedFile", "InternalRegisterGeneratedFile",
        "NameOfEnum",
    };
    return builtins.count(name) > 0;
}

// ══════════════════════════════════════════════════════════════
//  C++ 提取器实现
// ══════════════════════════════════════════════════════════════

/**
 * 构造函数：初始化 tree-sitter 的 C++ 语言解析器。
 * tree_sitter_cpp() 返回 tree-sitter-cpp 库预定义的语言描述符。
 */
CppExtractor::CppExtractor() : lang_(tree_sitter_cpp()) {}
CppExtractor::~CppExtractor() = default;

/**
 * 从 AST 节点提取对应的源代码文本。
 *
 * tree-sitter 的节点通过字节偏移定位源代码：
 *   - ts_node_start_byte(node) → 节点在源代码中的起始位置
 *   - ts_node_end_byte(node)   → 节点在源代码中的结束位置
 *   - 源代码文本 = source[start..end]
 *
 * 注意：end 是开区间，substr 的第二个参数是长度，所以用 end - start。
 */
std::string CppExtractor::get_node_text(TSNode node, const std::string& source) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start >= source.size()) return "";
    return source.substr(start, std::min(end, (uint32_t)source.size()) - start);
}

/**
 * 提取函数签名（去掉函数体）。
 *
 * 签名 = 函数声明到 '{' 之前的部分，例如：
 *   "void foo(int x, std::string s)"
 *
 * 实现：取节点完整文本，找到第一个 '{'，截断并去掉尾部空白。
 */
std::string CppExtractor::extract_signature(TSNode node, const std::string& source) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    std::string full = source.substr(start, std::min(end, (uint32_t)source.size()) - start);
    auto pos = full.find('{');
    if (pos != std::string::npos) {
        // 去掉 '{' 前的空白字符
        while (pos > 0 && (full[pos-1] == ' ' || full[pos-1] == '\t' || full[pos-1] == '\n')) pos--;
        return full.substr(0, pos);
    }
    return full;
}

/**
 * 提取函数/类的文档注释。
 *
 * 支持两种注释格式：
 *   1. 块注释：/* ... *\/  （紧接在节点之前）
 *   2. 行注释：// ...       （同一行或上一行）
 *
 * 实现：从节点起始位置向前扫描，跳过空白，检查是否有注释。
 */
std::string CppExtractor::extract_docstring(TSNode node, const std::string& source) {
    TSPoint start_point = ts_node_start_point(node);
    if (start_point.row == 0) return "";  // 第一行没有前置注释

    uint32_t node_start = ts_node_start_byte(node);
    uint32_t pos = node_start;
    // 向前跳过空白字符
    while (pos > 0 && (source[pos-1] == ' ' || source[pos-1] == '\t' || source[pos-1] == '\n')) pos--;

    // 检查块注释 /* ... */
    if (pos >= 2 && source[pos-2] == '/' && source[pos-1] == '*') {
        uint32_t comment_end = pos;
        uint32_t comment_start = source.rfind("/*", comment_end - 2);
        if (comment_start != std::string::npos) {
            return source.substr(comment_start, comment_end - comment_start);
        }
    }

    // 检查行注释 //
    uint32_t line_start = pos;
    while (line_start > 0 && source[line_start-1] != '\n') line_start--;
    if (line_start < pos && source[line_start] == '/' && line_start+1 < pos && source[line_start+1] == '/') {
        return source.substr(line_start, pos - line_start);
    }

    return "";
}

/**
 * 检查声明节点是否包含 function_declarator 子节点。
 *
 * 在 tree-sitter 的 C++ AST 中，函数声明的结构是：
 *   declaration
 *     ├── type_primitive  → "void"
 *     └── function_declarator
 *         ├── identifier  → "foo"
 *         └── parameter_list → "(int x)"
 *
 * 如果没有 function_declarator，说明是变量声明而非函数声明。
 */
static bool has_function_declarator(TSNode node) {
    uint32_t cnt = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode child = ts_node_named_child(node, i);
        if (strcmp(ts_node_type(child), "function_declarator") == 0) return true;
    }
    return false;
}

/**
 * 检查 function_declarator 是否包含类型参数（parameter_declaration）。
 *
 * 这是区分"真正的函数声明"和"带构造参数的变量声明"的关键：
 *
 *   真正的函数：void foo(int x, std::string s)
 *     → parameter_list 中的子节点是 parameter_declaration（有类型）
 *
 *   变量声明：RpcServer server(port, registry)
 *     → parameter_list 中的子节点是 identifier/expression（没有类型）
 *
 * 为什么需要这个区分：
 *   tree-sitter 把 "RpcServer server(port, registry)" 也解析为 declaration + function_declarator，
 *   但实际上这是变量声明（用构造函数初始化），不是函数声明。
 */
static bool has_type_parameters(TSNode func_decl_node) {
    uint32_t cnt = ts_node_named_child_count(func_decl_node);
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode child = ts_node_named_child(func_decl_node, i);
        if (strcmp(ts_node_type(child), "parameter_list") == 0) {
            // 检查 parameter_list 中是否有 parameter_declaration
            uint32_t param_cnt = ts_node_named_child_count(child);
            for (uint32_t j = 0; j < param_cnt; j++) {
                TSNode param = ts_node_named_child(child, j);
                if (strcmp(ts_node_type(param), "parameter_declaration") == 0) return true;
            }
        }
    }
    return false;
}

/**
 * 检查声明是否有函数体（compound_statement）。
 *
 *   void foo(int x) { ... }  → 有函数体 → 函数定义
 *   void foo(int x);         → 无函数体 → 函数声明（前向声明）
 *
 * 注：当前代码中，有函数体的识别为 Function，无函数体的识别为 Variable。
 * 这是一个简化处理，因为前向声明通常不需要在调用图中体现。
 */
static bool has_function_body(TSNode node) {
    uint32_t cnt = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode child = ts_node_named_child(node, i);
        if (strcmp(ts_node_type(child), "compound_statement") == 0) return true;
    }
    return false;
}

/**
 * 规范化被调用函数名。
 *
 * 处理各种 C++ 调用表达式的格式：
 *   - "template get<int>"  → 去掉 "template " 前缀
 *   - "obj->method"        → 提取 "method"
 *   - "ns::func<T>"        → 提取 "func"（去掉模板参数）
 *   - 去掉首尾空白
 *
 * 为什么需要规范化：
 *   tree-sitter 原样输出源代码文本，但我们需要统一的名字来匹配数据库中的节点。
 */
static std::string normalize_callee_name(std::string name) {
    // 去掉 "template " 前缀（如 ref.template get<int>()）
    const std::string template_prefix = "template ";
    if (name.rfind(template_prefix, 0) == 0) {
        name = name.substr(template_prefix.size());
    }

    // 去掉成员访问前缀（obj.method → method, ptr->method → method, ns::func → func）
    for (const std::string sep : {"->", ".", "::"}) {
        auto pos = name.rfind(sep);
        if (pos != std::string::npos) {
            name = name.substr(pos + sep.size());
        }
    }

    // 去掉模板参数（func<T> → func）
    auto angle = name.find('<');
    if (angle != std::string::npos) {
        name = name.substr(0, angle);
    }
    // 去掉首尾空白
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
        name.erase(name.begin());
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
        name.pop_back();
    }
    return name;
}

/**
 * 从 AST 节点提取源代码文本（与 get_node_text 相同，独立辅助函数）。
 */
static std::string node_text(TSNode node, const std::string& source) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start >= source.size()) return "";
    return source.substr(start, std::min(end, (uint32_t)source.size()) - start);
}

/**
 * 从 call_expression 的 function 字段提取被调用函数名。
 *
 * C++ 的调用表达式有多种 AST 形态，需要递归处理：
 *
 *   1. field_expression: obj.method() 或 ptr->method()
 *      → 递归提取 field 部分（即 method）
 *
 *   2. dependent_name: ref.template get<int>()
 *      → 递归提取第一个子节点
 *
 *   3. template_method / template_function / qualified_identifier:
 *      ns::func<T> 或 Class::method
 *      → 提取 name 字段（去掉限定符和模板参数）
 *
 *   4. 其他（简单标识符）：foo()
 *      → 直接取文本并清理
 *
 * 递归的原因：C++ 的调用可以嵌套，如 a.b().c() 会产生嵌套的 field_expression。
 */
static std::string extract_callee_name(TSNode node, const std::string& source) {
    if (ts_node_is_null(node)) return "";

    const char* type = ts_node_type(node);

    // ── case 1: field_expression ──
    // obj.method() 或 ptr->method()：提取 . 或 -> 后面的 field 名
    if (strcmp(type, "field_expression") == 0) {
        TSNode field = ts_node_child_by_field_name(node, "field", 5);
        if (!ts_node_is_null(field)) {
            return extract_callee_name(field, source);  // 递归提取
        }
    }

    // ── case 2: dependent_name ──
    // ref.template get<int>()：dependent_name 包含模板调用
    if (strcmp(type, "dependent_name") == 0) {
        uint32_t child_count = ts_node_named_child_count(node);
        if (child_count > 0) {
            return extract_callee_name(ts_node_named_child(node, 0), source);
        }
    }

    // ── case 3: template_method / template_function / qualified_identifier ──
    // ns::func<T> 或 Class::method：提取 name 字段（去掉限定符和模板参数）
    if (strcmp(type, "template_method") == 0 ||
        strcmp(type, "template_function") == 0 ||
        strcmp(type, "qualified_identifier") == 0) {
        TSNode name = ts_node_child_by_field_name(node, "name", 4);
        if (!ts_node_is_null(name) && !ts_node_eq(name, node)) {
            return extract_callee_name(name, source);
        }
    }

    // ── case 4: 简单标识符 ──
    return normalize_callee_name(node_text(node, source));
}

/**
 * 判断是否应该跳过某个被调用函数。
 *
 * 关键设计决策：
 *   field_expression 类型的调用（obj.method()）不跳过！
 *   原因：`get` 可能是 std::get，也可能是用户自定义的成员方法。
 *   没有类型信息时，保留成员调用比误杀更安全。
 *
 * 跳过条件：
 *   - 非 field_expression 的内置符号（如直接调用 printf、sort 等）
 */
static bool should_skip_callee(TSNode function_node, const std::string& callee) {
    if (ts_node_is_null(function_node)) return true;

    // field_expression: 成员调用，不经过 builtin 过滤
    // 因为 obj.method() 中的 method 可能是用户自定义的
    if (strcmp(ts_node_type(function_node), "field_expression") == 0) {
        return false;
    }

    return is_builtin_symbol(callee);
}

/**
 * 将 tree-sitter 的节点类型名映射为 codegraph 的 NodeKind 枚举。
 *
 * tree-sitter 的节点类型名是 C++ 语法结构的名称，如：
 *   - "function_definition"  → 函数定义
 *   - "class_specifier"      → 类定义
 *   - "declaration"          → 通用声明（需要进一步区分）
 *
 * 特殊处理 declaration：
 *   tree-sitter 把函数声明和变量声明都归类为 "declaration"，
 *   需要通过子节点结构来区分：
 *     - 有 function_declarator + parameter_declaration → 函数
 *     - 有 function_declarator + 无 parameter_declaration → 变量（构造函数初始化）
 *     - 其他 → 变量
 */
static NodeKind classify_cpp_node(const char* type_name, TSNode node) {
    if (strcmp(type_name, "function_definition") == 0) return NodeKind::Function;
    if (strcmp(type_name, "declaration") == 0) {
        // 区分函数声明和变量声明：
        //   void foo(int x);                   → 函数声明（有 parameter_declaration）
        //   void foo(int x) { ... }            → 函数定义（有函数体）
        //   RpcServer server(port, registry);  → 变量声明（无 parameter_declaration）
        //   UniqueFd fd(STDOUT_FILENO);        → 变量声明
        if (has_function_declarator(node)) {
            // 检查 function_declarator 是否有类型参数（真正的函数）
            uint32_t cnt = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < cnt; i++) {
                TSNode child = ts_node_named_child(node, i);
                if (strcmp(ts_node_type(child), "function_declarator") == 0) {
                    if (!has_type_parameters(child)) {
                        return NodeKind::Variable;  // RpcServer server(port, registry)
                    }
                    break;
                }
            }
            return has_function_body(node) ? NodeKind::Function : NodeKind::Variable;
        }
        return NodeKind::Variable;
    }
    if (strcmp(type_name, "class_specifier") == 0) return NodeKind::Class;
    if (strcmp(type_name, "struct_specifier") == 0) return NodeKind::Struct;
    if (strcmp(type_name, "enum_specifier") == 0) return NodeKind::Enum;
    if (strcmp(type_name, "enumerator") == 0) return NodeKind::EnumMember;
    if (strcmp(type_name, "namespace_definition") == 0) return NodeKind::Namespace;
    if (strcmp(type_name, "type_definition") == 0) return NodeKind::TypeAlias;
    if (strcmp(type_name, "field_declaration") == 0) return NodeKind::Field;
    if (strcmp(type_name, "parameter_declaration") == 0) return NodeKind::Parameter;
    if (strcmp(type_name, "preproc_include") == 0) return NodeKind::Import;
    if (strcmp(type_name, "using_declaration") == 0) return NodeKind::TypeAlias;
    return NodeKind::Variable;
}

/**
 * walk_tree 的入口：委托给 walk_tree_scoped（无初始作用域）。
 */
void CppExtractor::walk_tree(TSNode node, const std::string& source, const std::string& file_path,
                              int64_t parent_id, ExtractionResult& result) {
    walk_tree_scoped(node, source, file_path, parent_id, "", result);
}

/**
 * 递归遍历 AST，提取节点和调用关系。
 *
 * 这是 C++ 提取器的核心函数，执行两个任务：
 *
 * 1. 节点提取：
 *    - 遇到函数/类/结构体等"感兴趣"的节点时，创建 Node 对象
 *    - 提取名称、位置、签名、文档、修饰符等信息
 *    - 维护 qualified_name（限定名），如 "MyClass::my_method"
 *
 * 2. 调用边提取：
 *    - 对于函数/方法节点，递归搜索其函数体中的所有 call_expression
 *    - 提取被调用函数名，存为 UnresolvedRef（后续统一解析为 Edge）
 *
 * 作用域追踪：
 *    - namespace、class、struct 会更新 scope 字符串
 *    - 子节点继承父节点的 scope
 *    - qualified_name = scope + "::" + name
 *
 * ID 分配：
 *    - 临时 ID 为负数：-1, -2, -3, ...
 *    - 后续由 main.cpp 映射为数据库的真实 ID
 */
void CppExtractor::walk_tree_scoped(TSNode node, const std::string& source, const std::string& file_path,
                                     int64_t parent_id, const std::string& scope, ExtractionResult& result) {
    const char* type_name = ts_node_type(node);

    // 分类当前节点
    NodeKind kind = classify_cpp_node(type_name, node);
    bool is_interesting = (kind == NodeKind::Function || kind == NodeKind::Method ||
                           kind == NodeKind::Class || kind == NodeKind::Struct ||
                           kind == NodeKind::Enum || kind == NodeKind::Namespace ||
                           kind == NodeKind::TypeAlias || kind == NodeKind::Import);

    int64_t my_id = parent_id;
    std::string node_name;

    // ── 如果是"感兴趣"的节点，提取信息并创建 Node ──
    if (is_interesting) {
        Node n;
        n.kind = kind;
        n.file_path = file_path;
        n.language = "cpp";

        // 提取节点名称：
        //   1. 先尝试 "name" 字段（tree-sitter 的命名字段）
        //   2. 如果没有，遍历子节点找 function_declarator 或 identifier
        TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
        if (ts_node_is_null(name_node)) {
            uint32_t named_count = ts_node_named_child_count(node);
            for (uint32_t ni = 0; ni < named_count; ni++) {
                TSNode child = ts_node_named_child(node, ni);
                const char* child_type = ts_node_type(child);
                if (strcmp(child_type, "function_declarator") == 0) {
                    if (ts_node_named_child_count(child) > 0) {
                        name_node = ts_node_named_child(child, 0);
                    }
                    break;
                }
                if (strcmp(child_type, "identifier") == 0 || strcmp(child_type, "namespace_identifier") == 0) {
                    name_node = child;
                    break;
                }
            }
        }
        if (!ts_node_is_null(name_node)) {
            n.name = get_node_text(name_node, source);
        } else if (kind == NodeKind::Import) {
            // #include 的特殊处理：提取引号或尖括号中的路径
            n.name = get_node_text(node, source);
            auto pos = n.name.find('<');
            if (pos == std::string::npos) pos = n.name.find('"');
            if (pos != std::string::npos) n.name = n.name.substr(pos);
        }
        node_name = n.name;

        // 提取位置信息（行号从 1 开始，tree-sitter 从 0 开始）
        TSPoint start = ts_node_start_point(node);
        TSPoint end = ts_node_end_point(node);
        n.line = start.row + 1;
        n.col = start.column + 1;
        n.end_line = end.row + 1;
        n.end_col = end.column + 1;

        // 提取签名和文档
        if (kind == NodeKind::Function || kind == NodeKind::Method) {
            n.signature = extract_signature(node, source);
        }
        n.docstring = extract_docstring(node, source);

        // 检查修饰符（static、const）
        n.is_static = false;
        n.is_const = false;
        uint32_t child_count = ts_node_child_count(node);
        for (uint32_t i = 0; i < child_count; i++) {
            TSNode child = ts_node_child(node, i);
            const char* ctype = ts_node_type(child);
            if (strcmp(ctype, "storage_class_specifier") == 0) {
                std::string t = get_node_text(child, source);
                if (t == "static") n.is_static = true;
            }
            if (strcmp(ctype, "type_qualifier") == 0) {
                std::string t = get_node_text(child, source);
                if (t == "const") n.is_const = true;
            }
        }

        // 构建限定名（qualified_name）
        //   如果有 scope → "scope::name"（如 "MyClass::my_method"）
        //   如果没有 scope → "name"
        n.qualified_name = scope.empty() ? n.name : scope + "::" + n.name;

        // 分配临时 ID（负数），后续由 main.cpp 映射为数据库真实 ID
        n.id = 0;
        result.nodes.push_back(n);
        my_id = -(int64_t)result.nodes.size();
    }

    // ── 递归遍历子节点，更新作用域 ──
    // namespace、class、struct 会创建新的作用域层级
    std::string child_scope = scope;
    if (is_interesting && !node_name.empty()) {
        if (kind == NodeKind::Namespace || kind == NodeKind::Class || kind == NodeKind::Struct) {
            child_scope = scope.empty() ? node_name : scope + "::" + node_name;
        }
    }

    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        walk_tree_scoped(child, source, file_path, my_id, child_scope, result);
    }

    // ── 提取调用边：搜索函数体中的 call_expression ──
    // 只对函数/方法节点执行，递归搜索所有子节点
    if ((kind == NodeKind::Function || kind == NodeKind::Method) && !ts_node_is_null(node)) {
        // find_calls 是一个递归 lambda，遍历 AST 找所有 call_expression
        std::function<void(TSNode)> find_calls = [&](TSNode n) {
            const char* t = ts_node_type(n);
            if (strcmp(t, "call_expression") == 0) {
                // call_expression 的结构：
                //   call_expression
                //     ├── function  → 被调用的函数（可能是 identifier、field_expression 等）
                //     └── arguments → 参数列表
                TSNode fn = ts_node_child_by_field_name(n, "function", 8);
                if (!ts_node_is_null(fn)) {
                    std::string callee = extract_callee_name(fn, source);

                    // 过滤内置符号，但保留成员调用
                    if (!callee.empty() && !should_skip_callee(fn, callee)) {
                        UnresolvedRef ref;
                        ref.source_node_id = my_id;   // 调用者的临时 ID
                        ref.ref_name = callee;         // 被调用函数名
                        ref.ref_kind = "call";
                        TSPoint pt = ts_node_start_point(n);
                        ref.line = pt.row + 1;
                        ref.col = pt.column + 1;
                        result.unresolved.push_back(ref);
                    }
                }
            }
            // 递归搜索所有子节点
            uint32_t cnt = ts_node_child_count(n);
            for (uint32_t j = 0; j < cnt; j++) {
                find_calls(ts_node_child(n, j));
            }
        };
        find_calls(node);
    }
}

/**
 * C++ 提取器的主入口。
 *
 * 流程：
 *   1. 创建 tree-sitter parser，设置为 C++ 语言
 *   2. 解析源代码为 AST
 *   3. 检查是否有解析错误（有错误则跳过，避免垃圾节点）
 *   4. 递归遍历 AST，提取节点和调用关系
 *   5. 清理 tree-sitter 资源
 *
 * 性能考虑：
 *   - tree-sitter 是增量解析器，但这里每次都是全量解析（适合批量索引场景）
 *   - 解析 1000 行 C++ 代码约 1-5ms
 */
ExtractionResult CppExtractor::extract(const std::string& file_path, const std::string& source) {
    ExtractionResult result;

    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, lang_);

    TSTree* tree = ts_parser_parse_string(parser, nullptr, source.c_str(), source.size());
    if (!tree) {
        ts_parser_delete(parser);
        return result;
    }

    TSNode root = ts_tree_root_node(tree);

    // 有解析错误的文件跳过，避免产生垃圾节点
    // tree-sitter 的容错解析会产生 ERROR 节点，这些节点的文本不可靠
    if (!ts_node_has_error(root)) {
        walk_tree(root, source, file_path, 0, result);
    }

    // 清理 tree-sitter 资源（必须手动释放，tree-sitter 是 C 库）
    ts_tree_delete(tree);
    ts_parser_delete(parser);

    return result;
}

// ══════════════════════════════════════════════════════════════
//  Python 提取器实现
// ══════════════════════════════════════════════════════════════

/**
 * Python 提取器的构造函数。
 * tree_sitter_python() 返回 tree-sitter-python 库的语言描述符。
 */
PythonExtractor::PythonExtractor() : lang_(tree_sitter_python()) {}
PythonExtractor::~PythonExtractor() = default;

/**
 * Python 版本的 get_node_text（与 C++ 版本逻辑相同）。
 */
std::string PythonExtractor::get_node_text(TSNode node, const std::string& source) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start >= source.size()) return "";
    return source.substr(start, std::min(end, (uint32_t)source.size()) - start);
}

/**
 * Python 节点类型分类。
 * 比 C++ 简单得多，因为 Python 语法更简洁：
 *   - function_definition → Function
 *   - class_definition    → Class
 *   - import_statement    → Import
 *   - assignment          → Variable
 */
static NodeKind classify_python_node(const char* type_name) {
    if (strcmp(type_name, "function_definition") == 0) return NodeKind::Function;
    if (strcmp(type_name, "class_definition") == 0) return NodeKind::Class;
    if (strcmp(type_name, "import_statement") == 0) return NodeKind::Import;
    if (strcmp(type_name, "import_from_statement") == 0) return NodeKind::Import;
    if (strcmp(type_name, "assignment") == 0) return NodeKind::Variable;
    return NodeKind::Variable;
}

/**
 * Python AST 遍历，提取节点和调用关系。
 *
 * 与 C++ 版本的区别：
 *   - 没有作用域追踪（Python 用模块级导入，不需要限定名）
 *   - 装饰器检测：有装饰器的函数/类标记为 exported
 *   - 调用提取：Python 的 call 节点直接用 "function" 字段
 *   - 属性访问处理：obj.method → 提取 "method"（去掉 "obj."）
 */
void PythonExtractor::walk_tree(TSNode node, const std::string& source, const std::string& file_path,
                                 int64_t parent_id, ExtractionResult& result) {
    const char* type_name = ts_node_type(node);
    NodeKind kind = classify_python_node(type_name);
    bool is_interesting = (kind == NodeKind::Function || kind == NodeKind::Class ||
                           kind == NodeKind::Import);

    int64_t my_id = parent_id;

    if (is_interesting) {
        Node n;
        n.kind = kind;
        n.file_path = file_path;
        n.language = "python";

        // 提取名称
        TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
        if (!ts_node_is_null(name_node)) {
            n.name = get_node_text(name_node, source);
        } else {
            n.name = get_node_text(node, source);
            if (n.name.size() > 80) n.name = n.name.substr(0, 77) + "...";
        }

        // 提取位置
        TSPoint start = ts_node_start_point(node);
        TSPoint end = ts_node_end_point(node);
        n.line = start.row + 1;
        n.col = start.column + 1;
        n.end_line = end.row + 1;
        n.end_col = end.column + 1;

        // 检测装饰器（@decorator）
        if (kind == NodeKind::Function || kind == NodeKind::Class) {
            uint32_t child_count = ts_node_child_count(node);
            for (uint32_t i = 0; i < child_count; i++) {
                TSNode child = ts_node_child(node, i);
                if (strcmp(ts_node_type(child), "decorator") == 0 ||
                    strcmp(ts_node_type(child), "decorator_list") == 0) {
                    n.is_exported = true;
                    break;
                }
            }
        }

        n.qualified_name = n.name;
        result.nodes.push_back(n);
        my_id = -(int64_t)result.nodes.size();
    }

    // 提取调用边（与 C++ 版本类似，但更简单）
    if (kind == NodeKind::Function && my_id != parent_id) {
        std::function<void(TSNode)> find_calls = [&](TSNode n) {
            const char* t = ts_node_type(n);
            if (strcmp(t, "call") == 0) {
                TSNode fn = ts_node_child_by_field_name(n, "function", 8);
                if (!ts_node_is_null(fn)) {
                    std::string callee = get_node_text(fn, source);
                    // 属性访问处理：obj.method → method
                    auto pos = callee.rfind('.');
                    if (pos != std::string::npos) callee = callee.substr(pos + 1);

                    if (!callee.empty()) {
                        UnresolvedRef ref;
                        ref.source_node_id = my_id;
                        ref.ref_name = callee;
                        ref.ref_kind = "call";
                        TSPoint pt = ts_node_start_point(n);
                        ref.line = pt.row + 1;
                        ref.col = pt.column + 1;
                        result.unresolved.push_back(ref);
                    }
                }
            }
            uint32_t cnt = ts_node_child_count(n);
            for (uint32_t j = 0; j < cnt; j++) {
                find_calls(ts_node_child(n, j));
            }
        };
        find_calls(node);
    }

    // 递归遍历子节点
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        walk_tree(child, source, file_path, my_id, result);
    }
}

/**
 * Python 提取器的主入口（与 C++ 版本逻辑相同）。
 */
ExtractionResult PythonExtractor::extract(const std::string& file_path, const std::string& source) {
    ExtractionResult result;

    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, lang_);

    TSTree* tree = ts_parser_parse_string(parser, nullptr, source.c_str(), source.size());
    if (!tree) {
        ts_parser_delete(parser);
        return result;
    }

    TSNode root = ts_tree_root_node(tree);

    if (!ts_node_has_error(root)) {
        walk_tree(root, source, file_path, 0, result);
    }

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    return result;
}

// ══════════════════════════════════════════════════════════════
//  工厂函数
// ══════════════════════════════════════════════════════════════

/**
 * 根据语言名创建对应的提取器。
 *
 * 支持的语言标识：
 *   - "cpp" / "c" / "h" / "hpp" / "hxx" / "hh" → C++ 提取器
 *   - "python" / "py" → Python 提取器
 *   - 其他 → 返回 nullptr（不支持）
 */
std::unique_ptr<LanguageExtractor> create_extractor(const std::string& language) {
    if (language == "cpp" || language == "c" || language == "h" || language == "hpp" || language == "hxx" || language == "hh") {
        return std::make_unique<CppExtractor>();
    }
    if (language == "python" || language == "py") {
        return std::make_unique<PythonExtractor>();
    }
    return nullptr;
}

/**
 * 根据文件扩展名检测语言。
 *
 * 返回值：
 *   - "cpp" → C/C++ 源文件或头文件
 *   - "python" → Python 文件
 *   - "" → 不支持的语言
 */
std::string detect_language(const std::string& file_path) {
    auto dot = file_path.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = file_path.substr(dot + 1);
    if (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "c" || ext == "h" || ext == "hpp" || ext == "hxx" || ext == "hh") return "cpp";
    if (ext == "py" || ext == "pyi") return "python";
    return "";
}

}  // namespace codegraph
