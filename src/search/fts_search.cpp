/**
 * fts_search.cpp — 全文搜索的薄封装
 *
 * 本文件是 FtsSearch 类的实现，目前只是对 Database::search_fts() 的简单委托。
 *
 * 设计意图：
 *   - 提供独立的搜索接口，与数据库解耦
 *   - 未来可扩展搜索逻辑（如搜索历史、搜索建议、搜索过滤等）
 *   - 当前实现直接委托给 database，零额外开销
 */

#include "codegraph/search/fts_search.h"

namespace codegraph {

FtsSearch::FtsSearch(Database& db) : db_(db) {}

std::vector<Node> FtsSearch::search(const std::string& query, int limit) {
    return db_.search_fts(query, limit);
}

}  // namespace codegraph
