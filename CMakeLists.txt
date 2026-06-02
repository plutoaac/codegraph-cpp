cmake_minimum_required(VERSION 3.20)
project(codegraph-cpp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── tree-sitter from npm (matching version for grammars) ──
set(TS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/node_modules/tree-sitter/vendor/tree-sitter)

add_library(tree_sitter STATIC
    ${TS_DIR}/lib/src/parser.c
    ${TS_DIR}/lib/src/lexer.c
    ${TS_DIR}/lib/src/subtree.c
    ${TS_DIR}/lib/src/stack.c
    ${TS_DIR}/lib/src/node.c
    ${TS_DIR}/lib/src/tree.c
    ${TS_DIR}/lib/src/tree_cursor.c
    ${TS_DIR}/lib/src/query.c
    ${TS_DIR}/lib/src/language.c
    ${TS_DIR}/lib/src/alloc.c
    ${TS_DIR}/lib/src/get_changed_ranges.c
    ${TS_DIR}/lib/src/wasm_store.c
)
target_include_directories(tree_sitter PUBLIC ${TS_DIR}/lib/include ${TS_DIR}/lib/src)

# ── tree-sitter grammars (from npm install) ──
set(GRAMMAR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/node_modules)

add_library(ts_cpp STATIC
    ${GRAMMAR_DIR}/tree-sitter-cpp/src/parser.c
    ${GRAMMAR_DIR}/tree-sitter-cpp/src/scanner.c
)
target_include_directories(ts_cpp PUBLIC ${GRAMMAR_DIR}/tree-sitter-cpp/src ${TS_DIR}/lib/include)
target_link_libraries(ts_cpp PUBLIC tree_sitter)

add_library(ts_python STATIC
    ${GRAMMAR_DIR}/tree-sitter-python/src/parser.c
    ${GRAMMAR_DIR}/tree-sitter-python/src/scanner.c
)
target_include_directories(ts_python PUBLIC ${GRAMMAR_DIR}/tree-sitter-python/src ${TS_DIR}/lib/include)
target_link_libraries(ts_python PUBLIC tree_sitter)

# ── SQLite3 ──
find_package(SQLite3 REQUIRED)

# ── nlohmann/json ──
find_package(nlohmann_json 3.10 QUIET)
if(NOT TARGET nlohmann_json::nlohmann_json)
    add_library(nlohmann_json_fallback INTERFACE)
    target_include_directories(nlohmann_json_fallback INTERFACE /usr/include)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json_fallback)
endif()

# ── Main library ──
add_library(codegraph_core STATIC
    src/core/types.cpp
    src/db/database.cpp
    src/extraction/extractor.cpp
    src/extraction/cpp_extractor.cpp
    src/graph/traverser.cpp
    src/search/fts_search.cpp
    src/context/context_builder.cpp
    src/sync/file_watcher.cpp
    src/mcp/mcp_server.cpp
    src/diff/diff_parser.cpp
)

target_include_directories(codegraph_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${TS_DIR}/lib/include
)

target_link_libraries(codegraph_core PUBLIC
    SQLite::SQLite3
    nlohmann_json::nlohmann_json
    tree_sitter
    ts_cpp
    ts_python
    pthread
)

# ── CLI executable ──
add_executable(codegraph src/main.cpp)
target_link_libraries(codegraph PRIVATE codegraph_core)
target_compile_definitions(codegraph PRIVATE CMAKE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# ── Tests ──
enable_testing()
add_executable(test_basic tests/test_basic.cpp)
target_link_libraries(test_basic PRIVATE codegraph_core)
add_test(NAME basic COMMAND test_basic)
