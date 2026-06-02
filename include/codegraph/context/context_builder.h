#pragma once

#include "codegraph/db/database.h"
#include "codegraph/graph/traverser.h"
#include <nlohmann/json.hpp>

namespace codegraph {

class ContextBuilder {
public:
    ContextBuilder(Database& db, GraphTraverser& traverser);

    nlohmann::json build_context(const std::string& symbol, int limit = 10, int max_depth = 3);
    nlohmann::json search_symbols(const std::string& query, int limit = 20);
    nlohmann::json get_callers(const std::string& symbol, int max_depth = 3);
    nlohmann::json get_callees(const std::string& symbol, int max_depth = 3);
    nlohmann::json get_impact(const std::string& symbol, int max_depth = 5);
    nlohmann::json get_status();

private:
    Database& db_;
    GraphTraverser& traverser_;
    nlohmann::json node_to_json(const Node& node);
    nlohmann::json edge_to_json(const Edge& edge);
};

}  // namespace codegraph
