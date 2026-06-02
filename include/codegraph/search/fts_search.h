#pragma once

#include "codegraph/db/database.h"
#include <string>
#include <vector>

namespace codegraph {

class FtsSearch {
public:
    FtsSearch(Database& db);
    std::vector<Node> search(const std::string& query, int limit = 20);

private:
    Database& db_;
};

}  // namespace codegraph
