#include "codegraph/search/fts_search.h"

namespace codegraph {

FtsSearch::FtsSearch(Database& db) : db_(db) {}

std::vector<Node> FtsSearch::search(const std::string& query, int limit) {
    return db_.search_fts(query, limit);
}

}  // namespace codegraph
