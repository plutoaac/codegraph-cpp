#include "codegraph/extraction/extractor.h"
#include <tree_sitter/api.h>
#include <cstring>
#include <functional>
#include <unordered_set>

// Forward declarations from tree-sitter grammars
extern "C" TSLanguage* tree_sitter_cpp();
extern "C" TSLanguage* tree_sitter_python();

namespace codegraph {

static bool is_builtin_symbol(const std::string& name) {
    // Quick reject: template instantiations end with '>'
    if (!name.empty() && name.back() == '>') return true;

    static const std::unordered_set<std::string> builtins = {
        // C++ casts and operators
        "static_cast", "dynamic_cast", "reinterpret_cast", "const_cast",
        "sizeof", "alignof", "typeid", "noexcept",
        // C++ types
        "size_t", "int8_t", "uint8_t", "int16_t", "uint16_t",
        "int32_t", "uint32_t", "int64_t", "uint64_t",
        "intptr_t", "uintptr_t", "ptrdiff_t", "nullptr_t",
        // C++ standard library
        "to_string", "sort", "min", "max", "swap", "move", "forward",
        "make_shared", "make_unique", "memcpy", "memset", "memmove",
        "fprintf", "printf", "snprintf", "abort", "exit",
        "setw", "setprecision", "setfill", "left", "right",
        "begin", "end", "rbegin", "rend", "data", "size", "empty",
        "get", "holds_alternative", "visit", "any_cast",
        "stoi", "stol", "stoul", "stod", "stof",
        "atoi", "atol", "strtol", "strtoul", "strtod",
        // POSIX / Linux syscalls
        "sleep_for", "sleep_until",
        "strerror", "perror",
        "waitpid", "fork", "exec", "execv", "execvp",
        "kill", "getpid", "getppid",
        "socket", "setsockopt", "getsockopt", "bind", "listen",
        "accept", "accept4", "connect", "close", "shutdown",
        "send", "recv", "sendto", "recvfrom",
        "htons", "htonl", "ntohs", "ntohl",
        "inet_pton", "inet_ntop", "inet_addr",
        "epoll_create", "epoll_ctl", "epoll_wait",
        "fcntl", "ioctl", "pipe", "pipe2", "socketpair",
        "open", "read", "write", "lseek", "dup", "dup2",
        "mmap", "munmap", "mprotect",
        "WEXITSTATUS", "WIFEXITED", "WIFSIGNALED", "WTERMSIG",
        "eventfd", "timerfd_create", "signalfd",
        // Protobuf internals
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

// ── C++ Extractor ──

CppExtractor::CppExtractor() : lang_(tree_sitter_cpp()) {}
CppExtractor::~CppExtractor() = default;

std::string CppExtractor::get_node_text(TSNode node, const std::string& source) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start >= source.size()) return "";
    return source.substr(start, std::min(end, (uint32_t)source.size()) - start);
}

std::string CppExtractor::extract_signature(TSNode node, const std::string& source) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    std::string full = source.substr(start, std::min(end, (uint32_t)source.size()) - start);
    auto pos = full.find('{');
    if (pos != std::string::npos) {
        while (pos > 0 && (full[pos-1] == ' ' || full[pos-1] == '\t' || full[pos-1] == '\n')) pos--;
        return full.substr(0, pos);
    }
    return full;
}

std::string CppExtractor::extract_docstring(TSNode node, const std::string& source) {
    TSPoint start_point = ts_node_start_point(node);
    if (start_point.row == 0) return "";

    uint32_t node_start = ts_node_start_byte(node);
    uint32_t pos = node_start;
    while (pos > 0 && (source[pos-1] == ' ' || source[pos-1] == '\t' || source[pos-1] == '\n')) pos--;

    if (pos >= 2 && source[pos-2] == '/' && source[pos-1] == '*') {
        uint32_t comment_end = pos;
        uint32_t comment_start = source.rfind("/*", comment_end - 2);
        if (comment_start != std::string::npos) {
            return source.substr(comment_start, comment_end - comment_start);
        }
    }

    uint32_t line_start = pos;
    while (line_start > 0 && source[line_start-1] != '\n') line_start--;
    if (line_start < pos && source[line_start] == '/' && line_start+1 < pos && source[line_start+1] == '/') {
        return source.substr(line_start, pos - line_start);
    }

    return "";
}

static bool has_function_declarator(TSNode node) {
    uint32_t cnt = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode child = ts_node_named_child(node, i);
        if (strcmp(ts_node_type(child), "function_declarator") == 0) return true;
    }
    return false;
}

// Check if a function_declarator contains parameter_declaration children.
// Real function: void foo(int x, std::string s) -> parameters are parameter_declaration
// Variable with ctor args: RpcServer server(port, registry) -> parameters are identifiers/expressions
static bool has_type_parameters(TSNode func_decl_node) {
    uint32_t cnt = ts_node_named_child_count(func_decl_node);
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode child = ts_node_named_child(func_decl_node, i);
        if (strcmp(ts_node_type(child), "parameter_list") == 0) {
            // Check if any child of parameter_list is a parameter_declaration
            uint32_t param_cnt = ts_node_named_child_count(child);
            for (uint32_t j = 0; j < param_cnt; j++) {
                TSNode param = ts_node_named_child(child, j);
                if (strcmp(ts_node_type(param), "parameter_declaration") == 0) return true;
            }
        }
    }
    return false;
}

static bool has_function_body(TSNode node) {
    // Check if declaration has a compound_statement (function body)
    uint32_t cnt = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < cnt; i++) {
        TSNode child = ts_node_named_child(node, i);
        if (strcmp(ts_node_type(child), "compound_statement") == 0) return true;
    }
    return false;
}

static std::string normalize_callee_name(std::string name) {
    const std::string template_prefix = "template ";
    if (name.rfind(template_prefix, 0) == 0) {
        name = name.substr(template_prefix.size());
    }

    for (const std::string sep : {"->", ".", "::"}) {
        auto pos = name.rfind(sep);
        if (pos != std::string::npos) {
            name = name.substr(pos + sep.size());
        }
    }

    auto angle = name.find('<');
    if (angle != std::string::npos) {
        name = name.substr(0, angle);
    }
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
        name.erase(name.begin());
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
        name.pop_back();
    }
    return name;
}

static std::string node_text(TSNode node, const std::string& source) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start >= source.size()) return "";
    return source.substr(start, std::min(end, (uint32_t)source.size()) - start);
}

// 从 call_expression 的 function 字段提取被调用函数名。
// 递归处理各种 AST 节点类型：
//   - field_expression: obj.method() → 递归提取 field 部分
//   - dependent_name: ref.template get<int>() → 递归提取子节点
//   - template_method/template_function/qualified_identifier: 提取 name 字段
//   - 其他: 直接取文本并清理（去模板参数、去限定符前缀）
static std::string extract_callee_name(TSNode node, const std::string& source) {
    if (ts_node_is_null(node)) return "";

    const char* type = ts_node_type(node);
    // obj.method() 或 ptr->method()：提取 . 或 -> 后面的 field 名
    if (strcmp(type, "field_expression") == 0) {
        TSNode field = ts_node_child_by_field_name(node, "field", 5);
        if (!ts_node_is_null(field)) {
            return extract_callee_name(field, source);
        }
    }

    // ref.template get<int>()：dependent_name 包含模板调用
    if (strcmp(type, "dependent_name") == 0) {
        uint32_t child_count = ts_node_named_child_count(node);
        if (child_count > 0) {
            return extract_callee_name(ts_node_named_child(node, 0), source);
        }
    }

    // ns::func<T> 或 Class::method：提取 name 字段（去掉限定符和模板参数）
    if (strcmp(type, "template_method") == 0 ||
        strcmp(type, "template_function") == 0 ||
        strcmp(type, "qualified_identifier") == 0) {
        TSNode name = ts_node_child_by_field_name(node, "name", 4);
        if (!ts_node_is_null(name) && !ts_node_eq(name, node)) {
            return extract_callee_name(name, source);
        }
    }

    return normalize_callee_name(node_text(node, source));
}

// 判断是否应该跳过某个被调用函数。
// 关键设计：field_expression 类型的调用（obj.method()）不跳过，
// 因为 `get` 可能是 std::get 也可能是用户自定义的成员方法。
// 没有类型信息时，保留成员调用比误杀更安全。
static bool should_skip_callee(TSNode function_node, const std::string& callee) {
    if (ts_node_is_null(function_node)) return true;

    // field_expression: 成员调用，不经过 builtin 过滤
    if (strcmp(ts_node_type(function_node), "field_expression") == 0) {
        return false;
    }

    return is_builtin_symbol(callee);
}

static NodeKind classify_cpp_node(const char* type_name, TSNode node) {
    if (strcmp(type_name, "function_definition") == 0) return NodeKind::Function;
    if (strcmp(type_name, "declaration") == 0) {
        // Distinguish function declarations from variable declarations with constructor args:
        //   void foo(int x);                   // function declaration (has type params)
        //   void foo(int x) { ... }            // function definition (has body)
        //   RpcServer server(port, registry);  // variable, NOT function (no type params)
        //   UniqueFd fd(STDOUT_FILENO);        // variable, NOT function
        if (has_function_declarator(node)) {
            // Check if the function_declarator has type parameters (real function)
            // or expression parameters (variable with constructor args)
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

void CppExtractor::walk_tree(TSNode node, const std::string& source, const std::string& file_path,
                              int64_t parent_id, ExtractionResult& result) {
    walk_tree_scoped(node, source, file_path, parent_id, "", result);
}

void CppExtractor::walk_tree_scoped(TSNode node, const std::string& source, const std::string& file_path,
                                     int64_t parent_id, const std::string& scope, ExtractionResult& result) {
    const char* type_name = ts_node_type(node);

    NodeKind kind = classify_cpp_node(type_name, node);
    bool is_interesting = (kind == NodeKind::Function || kind == NodeKind::Method ||
                           kind == NodeKind::Class || kind == NodeKind::Struct ||
                           kind == NodeKind::Enum || kind == NodeKind::Namespace ||
                           kind == NodeKind::TypeAlias || kind == NodeKind::Import);

    int64_t my_id = parent_id;
    std::string node_name;

    if (is_interesting) {
        Node n;
        n.kind = kind;
        n.file_path = file_path;
        n.language = "cpp";

        // Extract name: try "name" field first, then walk named children for functions
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
            n.name = get_node_text(node, source);
            auto pos = n.name.find('<');
            if (pos == std::string::npos) pos = n.name.find('"');
            if (pos != std::string::npos) n.name = n.name.substr(pos);
        }
        node_name = n.name;

        TSPoint start = ts_node_start_point(node);
        TSPoint end = ts_node_end_point(node);
        n.line = start.row + 1;
        n.col = start.column + 1;
        n.end_line = end.row + 1;
        n.end_col = end.column + 1;

        if (kind == NodeKind::Function || kind == NodeKind::Method) {
            n.signature = extract_signature(node, source);
        }
        n.docstring = extract_docstring(node, source);

        // Check direct child nodes for storage class / type qualifiers
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

        n.qualified_name = scope.empty() ? n.name : scope + "::" + n.name;

        n.id = 0;
        result.nodes.push_back(n);
        my_id = -(int64_t)result.nodes.size();
    }

    // Walk children with updated scope for namespace/class/struct
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

    // Extract call edges for function bodies
    if ((kind == NodeKind::Function || kind == NodeKind::Method) && !ts_node_is_null(node)) {
        std::function<void(TSNode)> find_calls = [&](TSNode n) {
            const char* t = ts_node_type(n);
            if (strcmp(t, "call_expression") == 0) {
                TSNode fn = ts_node_child_by_field_name(n, "function", 8);
                if (!ts_node_is_null(fn)) {
                    std::string callee = extract_callee_name(fn, source);

                    if (!callee.empty() && !should_skip_callee(fn, callee)) {
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
}

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

    // Skip files with parse errors to avoid garbage nodes
    if (!ts_node_has_error(root)) {
        walk_tree(root, source, file_path, 0, result);
    }

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    return result;
}

// ── Python Extractor ──

PythonExtractor::PythonExtractor() : lang_(tree_sitter_python()) {}
PythonExtractor::~PythonExtractor() = default;

std::string PythonExtractor::get_node_text(TSNode node, const std::string& source) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start >= source.size()) return "";
    return source.substr(start, std::min(end, (uint32_t)source.size()) - start);
}

static NodeKind classify_python_node(const char* type_name) {
    if (strcmp(type_name, "function_definition") == 0) return NodeKind::Function;
    if (strcmp(type_name, "class_definition") == 0) return NodeKind::Class;
    if (strcmp(type_name, "import_statement") == 0) return NodeKind::Import;
    if (strcmp(type_name, "import_from_statement") == 0) return NodeKind::Import;
    if (strcmp(type_name, "assignment") == 0) return NodeKind::Variable;
    return NodeKind::Variable;
}

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

        TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
        if (!ts_node_is_null(name_node)) {
            n.name = get_node_text(name_node, source);
        } else {
            n.name = get_node_text(node, source);
            if (n.name.size() > 80) n.name = n.name.substr(0, 77) + "...";
        }

        TSPoint start = ts_node_start_point(node);
        TSPoint end = ts_node_end_point(node);
        n.line = start.row + 1;
        n.col = start.column + 1;
        n.end_line = end.row + 1;
        n.end_col = end.column + 1;

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

    // Extract call edges for function bodies (analogous to C++ find_calls)
    if (kind == NodeKind::Function && my_id != parent_id) {
        std::function<void(TSNode)> find_calls = [&](TSNode n) {
            const char* t = ts_node_type(n);
            if (strcmp(t, "call") == 0) {
                TSNode fn = ts_node_child_by_field_name(n, "function", 8);
                if (!ts_node_is_null(fn)) {
                    std::string callee = get_node_text(fn, source);
                    // Strip attribute access: obj.method -> method
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

    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        walk_tree(child, source, file_path, my_id, result);
    }
}

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

    // Skip files with parse errors to avoid garbage nodes
    if (!ts_node_has_error(root)) {
        walk_tree(root, source, file_path, 0, result);
    }

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    return result;
}

// ── Factory ──

std::unique_ptr<LanguageExtractor> create_extractor(const std::string& language) {
    if (language == "cpp" || language == "c" || language == "h" || language == "hpp" || language == "hxx" || language == "hh") {
        return std::make_unique<CppExtractor>();
    }
    if (language == "python" || language == "py") {
        return std::make_unique<PythonExtractor>();
    }
    return nullptr;
}

std::string detect_language(const std::string& file_path) {
    auto dot = file_path.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = file_path.substr(dot + 1);
    if (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "c" || ext == "h" || ext == "hpp" || ext == "hxx" || ext == "hh") return "cpp";
    if (ext == "py" || ext == "pyi") return "python";
    return "";
}

}  // namespace codegraph
