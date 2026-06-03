/**
 * extractor.h — 语言提取器接口
 *
 * 定义了代码提取器的抽象接口和具体实现（C++、Python）。
 *
 * 提取器的职责：
 *   1. 将源代码解析为 AST（通过 tree-sitter）
 *   2. 遍历 AST，提取代码符号（Node）和调用关系（UnresolvedRef）
 *   3. 返回 ExtractionResult，供后续批量写入数据库
 *
 * 继承体系：
 *   LanguageExtractor（抽象基类）
 *     ├── CppExtractor   — C/C++ 语言提取器
 *     └── PythonExtractor — Python 语言提取器
 *
 * 工厂函数：
 *   create_extractor("cpp")  → CppExtractor
 *   create_extractor("python") → PythonExtractor
 *   detect_language("foo.cpp") → "cpp"
 *
 * tree-sitter 依赖：
 *   只有本头文件和 cpp_extractor.cpp 依赖 tree-sitter/api.h。
 *   其他模块只使用 ExtractionResult、Node、Edge 等输出类型。
 */

#pragma once

#include "codegraph/core/types.h"
#include <memory>
#include <string>
#include <tree_sitter/api.h>
#include <vector>

namespace codegraph {

/**
 * 提取结果：节点列表 + 边列表 + 未解析引用列表。
 *
 * 节点（Node）：从源代码中提取的符号（函数、类、变量等）
 * 边（Edge）：当前未使用（边在后续解析阶段生成）
 * 未解析引用（UnresolvedRef）：函数调用目标还未解析为正式的边
 */
struct ExtractionResult {
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::vector<UnresolvedRef> unresolved;
};

/**
 * 语言提取器的抽象基类。
 *
 * 所有语言提取器都实现这个接口。
 * extract() 方法接收文件路径和源代码，返回 ExtractionResult。
 */
class LanguageExtractor {
public:
    virtual ~LanguageExtractor() = default;

    /**
     * 提取源代码中的符号和调用关系。
     *
     * @param file_path 文件路径（用于记录符号位置）
     * @param source 源代码文本
     * @return 提取结果（节点 + 未解析引用）
     */
    virtual ExtractionResult extract(const std::string& file_path, const std::string& source) = 0;

    /** 返回语言名（如 "cpp"、"python"）。 */
    virtual const char* language_name() const = 0;
};

/**
 * C++ 语言提取器。
 *
 * 使用 tree-sitter 解析 C/C++ 源代码，提取：
 *   - 函数/方法定义和声明
 *   - 类/结构体/枚举定义
 *   - 命名空间
 *   - #include 导入
 *   - 函数调用关系（作为 UnresolvedRef）
 *
 * tree-sitter 的 C++ 语言描述符通过 tree_sitter_cpp() 获取。
 */
class CppExtractor : public LanguageExtractor {
public:
    CppExtractor();
    ~CppExtractor() override;

    ExtractionResult extract(const std::string& file_path, const std::string& source) override;
    const char* language_name() const override { return "cpp"; }

private:
    TSLanguage* lang_;  // tree-sitter 的 C++ 语言描述符

    /** 递归遍历 AST（入口）。 */
    void walk_tree(TSNode node, const std::string& source, const std::string& file_path,
                   int64_t parent_id, ExtractionResult& result);

    /** 递归遍历 AST（带作用域追踪）。 */
    void walk_tree_scoped(TSNode node, const std::string& source, const std::string& file_path,
                          int64_t parent_id, const std::string& scope, ExtractionResult& result);

    /** 从 AST 节点提取源代码文本。 */
    std::string get_node_text(TSNode node, const std::string& source);

    /** 提取函数签名（去掉函数体）。 */
    std::string extract_signature(TSNode node, const std::string& source);

    /** 提取文档注释。 */
    std::string extract_docstring(TSNode node, const std::string& source);
};

/**
 * Python 语言提取器。
 *
 * 使用 tree-sitter 解析 Python 源代码，提取：
 *   - 函数定义（def）
 *   - 类定义（class）
 *   - import 语句
 *   - 函数调用关系
 *
 * 比 C++ 提取器简单（没有作用域追踪、没有签名提取）。
 */
class PythonExtractor : public LanguageExtractor {
public:
    PythonExtractor();
    ~PythonExtractor() override;

    ExtractionResult extract(const std::string& file_path, const std::string& source) override;
    const char* language_name() const override { return "python"; }

private:
    TSLanguage* lang_;  // tree-sitter 的 Python 语言描述符

    void walk_tree(TSNode node, const std::string& source, const std::string& file_path,
                   int64_t parent_id, ExtractionResult& result);
    std::string get_node_text(TSNode node, const std::string& source);
};

/**
 * 根据语言名创建对应的提取器。
 *
 * @param language 语言标识（"cpp"/"c"/"h"/"hpp"/"hxx"/"hh"/"python"/"py"）
 * @return 提取器实例，不支持的语言返回 nullptr
 */
std::unique_ptr<LanguageExtractor> create_extractor(const std::string& language);

/**
 * 根据文件扩展名检测语言。
 *
 * @param file_path 文件路径
 * @return 语言标识（"cpp"/"python"/""）
 */
std::string detect_language(const std::string& file_path);

}  // namespace codegraph
