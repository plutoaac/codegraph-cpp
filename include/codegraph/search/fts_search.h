/**
 * fts_search.h — 全文搜索的薄封装
 *
 * 对 Database::search_fts() 的简单委托。
 * 提供独立的搜索接口，与数据库解耦。
 * 未来可扩展搜索历史、搜索建议、搜索过滤等功能。
 */

#pragma once

#include "codegraph/db/database.h"
#include <string>
#include <vector>

namespace codegraph {

class FtsSearch {
public:
    FtsSearch(Database& db);

    /**
     * 全文搜索。
     *
     * @param query 搜索关键词
     * @param limit 最大返回数量
     * @return 匹配的节点列表（按 kind 优先级排序）
     */
    std::vector<Node> search(const std::string& query, int limit = 20);

private:
    Database& db_;
};

}  // namespace codegraph
