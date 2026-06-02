#pragma once

#include "codegraph/context/context_builder.h"
#include "codegraph/db/database.h"
#include "codegraph/graph/traverser.h"
#include <nlohmann/json.hpp>

namespace codegraph {

class McpServer {
public:
    McpServer(Database& db, GraphTraverser& traverser, ContextBuilder& context);
    void run();

private:
    Database& db_;
    GraphTraverser& traverser_;
    ContextBuilder& context_;

    nlohmann::json handle_request(const nlohmann::json& request);
    nlohmann::json handle_initialize(const nlohmann::json& params);
    nlohmann::json handle_tools_list();
    nlohmann::json handle_tools_call(const nlohmann::json& params);

    nlohmann::json tool_search(const nlohmann::json& args);
    nlohmann::json tool_context(const nlohmann::json& args);
    nlohmann::json tool_callers(const nlohmann::json& args);
    nlohmann::json tool_callees(const nlohmann::json& args);
    nlohmann::json tool_impact(const nlohmann::json& args);
    nlohmann::json tool_node(const nlohmann::json& args);
    nlohmann::json tool_status(const nlohmann::json& args);
    nlohmann::json tool_files(const nlohmann::json& args);
    nlohmann::json tool_diff(const nlohmann::json& args);
    nlohmann::json tool_semantic_search(const nlohmann::json& args);

    nlohmann::json make_result(const std::string& text);
    nlohmann::json make_error(const std::string& message);
};

}  // namespace codegraph
