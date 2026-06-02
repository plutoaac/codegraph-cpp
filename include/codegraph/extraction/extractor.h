#pragma once

#include "codegraph/core/types.h"
#include <memory>
#include <string>
#include <tree_sitter/api.h>
#include <vector>

namespace codegraph {

struct ExtractionResult {
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::vector<UnresolvedRef> unresolved;
};

class LanguageExtractor {
public:
    virtual ~LanguageExtractor() = default;
    virtual ExtractionResult extract(const std::string& file_path, const std::string& source) = 0;
    virtual const char* language_name() const = 0;
};

// ── C++ extractor ──
class CppExtractor : public LanguageExtractor {
public:
    CppExtractor();
    ~CppExtractor() override;

    ExtractionResult extract(const std::string& file_path, const std::string& source) override;
    const char* language_name() const override { return "cpp"; }

private:
    TSLanguage* lang_;
    void walk_tree(TSNode node, const std::string& source, const std::string& file_path,
                   int64_t parent_id, ExtractionResult& result);
    void walk_tree_scoped(TSNode node, const std::string& source, const std::string& file_path,
                          int64_t parent_id, const std::string& scope, ExtractionResult& result);
    std::string get_node_text(TSNode node, const std::string& source);
    std::string extract_signature(TSNode node, const std::string& source);
    std::string extract_docstring(TSNode node, const std::string& source);
};

// ── Python extractor ──
class PythonExtractor : public LanguageExtractor {
public:
    PythonExtractor();
    ~PythonExtractor() override;

    ExtractionResult extract(const std::string& file_path, const std::string& source) override;
    const char* language_name() const override { return "python"; }

private:
    TSLanguage* lang_;
    void walk_tree(TSNode node, const std::string& source, const std::string& file_path,
                   int64_t parent_id, ExtractionResult& result);
    std::string get_node_text(TSNode node, const std::string& source);
};

// ── Factory ──
std::unique_ptr<LanguageExtractor> create_extractor(const std::string& language);
std::string detect_language(const std::string& file_path);

}  // namespace codegraph
