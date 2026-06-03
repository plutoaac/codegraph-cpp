#include "codegraph/core/types.h"
#include "codegraph/db/database.h"
#include "codegraph/diff/diff_parser.h"
#include "codegraph/extraction/extractor.h"
#include "codegraph/graph/traverser.h"
#include "codegraph/context/context_builder.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <vector>

using namespace codegraph;
namespace fs = std::filesystem;

namespace {

[[noreturn]] void fail_check(const char* expr, const char* file, int line) {
    std::ostringstream msg;
    msg << file << ":" << line << ": check failed: " << expr;
    throw std::runtime_error(msg.str());
}

#define CHECK(expr) \
    do { \
        if (!(expr)) fail_check(#expr, __FILE__, __LINE__); \
    } while (false)

std::string temp_db_path(const std::string& name) {
    return (fs::temp_directory_path() /
            ("codegraph_test_" + std::to_string(getpid()) + "_" + name)).string();
}

void remove_db_files(const std::string& path) {
    fs::remove_all(path);
    fs::remove_all(path + "-wal");
    fs::remove_all(path + "-shm");
}

struct TempDb {
    explicit TempDb(const std::string& name) : path(temp_db_path(name)) {
        remove_db_files(path);
    }

    ~TempDb() {
        remove_db_files(path);
    }

    std::string path;
};

fs::path find_codegraph_exe() {
    const fs::path cwd = fs::current_path();
    const std::vector<fs::path> candidates = {
        cwd / "build" / "codegraph",
        cwd / "codegraph",
        cwd.parent_path() / "codegraph"
    };

    for (const auto& candidate : candidates) {
        if (fs::exists(candidate) && fs::is_regular_file(candidate)) {
            return fs::absolute(candidate);
        }
    }

    throw std::runtime_error("Cannot find codegraph executable. Build target 'codegraph' first.");
}

}  // namespace

void test_types() {
    CHECK(strcmp(node_kind_str(NodeKind::Function), "function") == 0);
    CHECK(strcmp(edge_kind_str(EdgeKind::Calls), "calls") == 0);
    std::cout << "  [PASS] types\n";
}

void test_database() {
    TempDb temp_db("database.db");

    Database db(temp_db.path);
    db.init_schema();

    Node n;
    n.kind = NodeKind::Function;
    n.name = "test_func";
    n.qualified_name = "ns::test_func";
    n.file_path = "/tmp/test.cpp";
    n.language = "cpp";
    n.line = 10;
    n.col = 1;
    n.end_line = 20;
    n.end_col = 1;
    n.signature = "void test_func(int x)";
    int64_t id = db.insert_node(n);
    CHECK(id > 0);

    auto got = db.get_node(id);
    CHECK(got.has_value());
    CHECK(got->name == "test_func");
    CHECK(got->kind == NodeKind::Function);

    Node n2;
    n2.kind = NodeKind::Function;
    n2.name = "caller_func";
    n2.file_path = "/tmp/test.cpp";
    n2.language = "cpp";
    int64_t id2 = db.insert_node(n2);

    Edge e;
    e.source_id = id2;
    e.target_id = id;
    e.kind = EdgeKind::Calls;
    e.line = 15;
    int64_t eid = db.insert_edge(e);
    CHECK(eid > 0);

    auto callers = db.get_edges_to(id, EdgeKind::Calls);
    CHECK(callers.size() == 1);
    CHECK(callers[0].source_id == id2);

    auto results = db.search_fts("test_func");
    CHECK(!results.empty());

    auto qualified_results = db.search_fts("ns::test_func");
    CHECK(!qualified_results.empty());

    CHECK(db.count_nodes() == 2);
    CHECK(db.count_edges() == 1);

    std::cout << "  [PASS] database\n";
}

void test_database_lookup_and_errors() {
    TempDb temp_db("database_lookup.db");

    Database db(temp_db.path);
    db.init_schema();

    Node exact;
    exact.kind = NodeKind::Function;
    exact.name = "foo";
    exact.qualified_name = "ns::foo";
    exact.file_path = "/tmp/a.cpp";
    exact.language = "cpp";
    int64_t exact_id = db.insert_node(exact);
    CHECK(exact_id > 0);

    Node fuzzy;
    fuzzy.kind = NodeKind::Function;
    fuzzy.name = "foo_helper";
    fuzzy.qualified_name = "ns::foo_helper";
    fuzzy.file_path = "/tmp/b.cpp";
    fuzzy.language = "cpp";
    int64_t fuzzy_id = db.insert_node(fuzzy);
    CHECK(fuzzy_id > 0);

    auto exact_results = db.find_nodes_by_name("foo", 1);
    CHECK(exact_results.size() == 1);
    CHECK(exact_results[0].id == exact_id);

    Edge invalid;
    invalid.source_id = 999999;
    invalid.target_id = exact_id;
    invalid.kind = EdgeKind::Calls;
    CHECK(db.insert_edge(invalid) < 0);

    bool batch_failed = false;
    try {
        db.insert_edges_batch(std::vector<Edge>{invalid});
    } catch (const std::runtime_error&) {
        batch_failed = true;
    }
    CHECK(batch_failed);

    std::cout << "  [PASS] database_lookup_and_errors\n";
}

void test_cpp_extractor() {
    CppExtractor extractor;
    std::string source = R"(
#include <iostream>

namespace math {
    int add(int a, int b) {
        return a + b;
    }

    class Calculator {
    public:
        int multiply(int a, int b) {
            return a * b;
        }
    };
}
)";

    auto result = extractor.extract("/tmp/test.cpp", source);
    CHECK(!result.nodes.empty());

    bool found_add = false, found_calc = false, found_multiply = false;
    bool found_qualified_add = false, found_qualified_multiply = false;
    for (auto& n : result.nodes) {
        if (n.name == "add") found_add = true;
        if (n.name == "Calculator") found_calc = true;
        if (n.name == "multiply") found_multiply = true;
        if (n.qualified_name == "math::add") found_qualified_add = true;
        if (n.qualified_name == "math::Calculator::multiply") found_qualified_multiply = true;
    }
    CHECK(found_add);
    CHECK(found_calc);
    CHECK(found_multiply);
    CHECK(found_qualified_add);
    CHECK(found_qualified_multiply);

    std::cout << "  [PASS] cpp_extractor (" << result.nodes.size() << " nodes)\n";
}

void test_cpp_member_call_extraction() {
    CppExtractor extractor;
    std::string source = R"(
namespace math {
    int add(int a, int b) {
        return a + b;
    }

    struct Helper {
        void reset() {}
        template <typename T>
        T get() { return T{}; }
    };

    int use(Helper& ref, Helper* ptr) {
        add(1, 2);
        math::add(3, 4);
        ref.reset();
        ptr->reset();
        return ref.template get<int>();
    }
}
)";

    auto result = extractor.extract("/tmp/member_calls.cpp", source);
    std::unordered_set<std::string> calls;
    for (const auto& ref : result.unresolved) {
        calls.insert(ref.ref_name);
    }

    CHECK(calls.count("add") > 0);
    CHECK(calls.count("reset") > 0);
    CHECK(calls.count("get") > 0);
    std::cout << "  [PASS] cpp_member_call_extraction\n";
}

void test_run_git_diff_does_not_execute_shell() {
    const char* marker = "/tmp/codegraph_git_diff_shell_injection_marker";
    std::remove(marker);

    (void)run_git_diff(std::string("HEAD; touch ") + marker);

    FILE* file = std::fopen(marker, "r");
    CHECK(file == nullptr);
    if (file != nullptr) {
        std::fclose(file);
        std::remove(marker);
    }

    std::cout << "  [PASS] run_git_diff_does_not_execute_shell\n";
}

void test_context_builder_splits_callers_and_callees() {
    TempDb temp_db("context.db");

    Database db(temp_db.path);
    db.init_schema();

    Node caller;
    caller.kind = NodeKind::Function;
    caller.name = "caller";
    caller.file_path = "/tmp/context.cpp";
    caller.language = "cpp";
    int64_t caller_id = db.insert_node(caller);

    Node target;
    target.kind = NodeKind::Function;
    target.name = "target";
    target.file_path = "/tmp/context.cpp";
    target.language = "cpp";
    int64_t target_id = db.insert_node(target);

    Node callee;
    callee.kind = NodeKind::Function;
    callee.name = "callee";
    callee.file_path = "/tmp/context.cpp";
    callee.language = "cpp";
    int64_t callee_id = db.insert_node(callee);

    Edge caller_edge;
    caller_edge.source_id = caller_id;
    caller_edge.target_id = target_id;
    caller_edge.kind = EdgeKind::Calls;
    CHECK(db.insert_edge(caller_edge) > 0);

    Edge callee_edge;
    callee_edge.source_id = target_id;
    callee_edge.target_id = callee_id;
    callee_edge.kind = EdgeKind::Calls;
    CHECK(db.insert_edge(callee_edge) > 0);

    GraphTraverser traverser(db);
    ContextBuilder context(db, traverser);
    auto result = context.build_context("target");

    CHECK(result.contains("callers"));
    CHECK(result.contains("callees"));
    CHECK(result["callers"].size() == 1);
    CHECK(result["callees"].size() == 1);
    CHECK(result["callers"][0]["name"] == "caller");
    CHECK(result["callees"][0]["name"] == "callee");

    std::cout << "  [PASS] context_builder_splits_callers_and_callees\n";
}

void test_python_extractor() {
    PythonExtractor extractor;
    std::string source = R"(
import os

def hello(name):
    """Say hello"""
    print(f"Hello {name}")

class Greeter:
    def greet(self):
        hello("world")
)";

    auto result = extractor.extract("/tmp/test.py", source);
    CHECK(!result.nodes.empty());

    bool found_hello = false, found_greeter = false;
    for (auto& n : result.nodes) {
        if (n.name == "hello") found_hello = true;
        if (n.name == "Greeter") found_greeter = true;
    }
    CHECK(found_hello);
    CHECK(found_greeter);

    std::cout << "  [PASS] python_extractor (" << result.nodes.size() << " nodes)\n";
}

void test_python_call_extraction() {
    PythonExtractor extractor;
    std::string source = R"(
def foo():
    bar()

def bar():
    foo()
)";

    auto result = extractor.extract("/tmp/test_calls.py", source);
    CHECK(!result.unresolved.empty());

    bool found_bar_call = false, found_foo_call = false;
    for (auto& ref : result.unresolved) {
        if (ref.ref_name == "bar") found_bar_call = true;
        if (ref.ref_name == "foo") found_foo_call = true;
    }
    CHECK(found_bar_call);
    CHECK(found_foo_call);
    std::cout << "  [PASS] python_call_extraction\n";
}

void test_detect_language() {
    CHECK(detect_language("foo.cpp") == "cpp");
    CHECK(detect_language("foo.cc") == "cpp");
    CHECK(detect_language("foo.cxx") == "cpp");
    CHECK(detect_language("foo.c") == "cpp");
    CHECK(detect_language("foo.h") == "cpp");
    CHECK(detect_language("foo.hpp") == "cpp");
    CHECK(detect_language("foo.hxx") == "cpp");
    CHECK(detect_language("foo.hh") == "cpp");
    CHECK(detect_language("foo.py") == "python");
    CHECK(detect_language("foo.pyi") == "python");
    CHECK(detect_language("foo.txt") == "");
    CHECK(detect_language("foo.rs") == "");
    std::cout << "  [PASS] detect_language\n";
}

int run_codegraph_cli(const fs::path& exe, const fs::path& cwd, const std::vector<std::string>& args) {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        if (chdir(cwd.c_str()) != 0) {
            _exit(127);
        }

        std::vector<std::string> storage;
        storage.reserve(args.size() + 1);
        storage.push_back(exe.string());
        for (const auto& arg : args) {
            storage.push_back(arg);
        }

        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (auto& arg : storage) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return 1;
        }
    }
    if (!WIFEXITED(status)) return 1;
    return WEXITSTATUS(status);
}

void test_incremental_reindex() {
    const fs::path exe = find_codegraph_exe();

    const fs::path root = fs::path("/tmp") / ("codegraph_incremental_cli_" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    const fs::path changed_file = root / "changed.cpp";
    const fs::path caller_file = root / "caller.cpp";
    const fs::path stable_file = root / "stable.cpp";
    {
        std::ofstream out(changed_file);
        out << "void beta() {}\nvoid alpha() { beta(); }\n";
    }
    {
        std::ofstream out(caller_file);
        out << "void external() { alpha(); }\n";
    }
    {
        std::ofstream out(stable_file);
        out << "void stable() {}\n";
    }

    CHECK(run_codegraph_cli(exe, root, {"init", "-i", root.string()}) == 0);

    const fs::path db_path = root / ".codegraph" / "index";
    int64_t stable_id_before = 0;
    {
        Database db(db_path.string());
        auto stable_nodes = db.find_nodes_by_name("stable", 5);
        CHECK(stable_nodes.size() == 1);
        stable_id_before = stable_nodes[0].id;
        CHECK(!db.find_nodes_by_name("alpha", 5).empty());
        CHECK(!db.find_nodes_by_name("beta", 5).empty());
        CHECK(!db.find_nodes_by_name("external", 5).empty());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    {
        std::ofstream out(changed_file);
        out << "void gamma() {}\n";
    }

    CHECK(run_codegraph_cli(exe, root, {"index", root.string()}) == 0);

    {
        Database db(db_path.string());
        auto gamma_nodes = db.find_nodes_by_name("gamma", 5);
        CHECK(gamma_nodes.size() == 1);
        CHECK(gamma_nodes[0].file_path == changed_file.string());

        auto stable_nodes = db.find_nodes_by_name("stable", 5);
        CHECK(stable_nodes.size() == 1);
        CHECK(stable_nodes[0].id == stable_id_before);

        for (const auto& n : db.find_nodes_by_name("alpha", 5)) {
            CHECK(n.file_path != changed_file.string());
        }
        for (const auto& n : db.find_nodes_by_name("beta", 5)) {
            CHECK(n.file_path != changed_file.string());
        }

        bool found_unresolved_alpha_from_caller = false;
        for (const auto& ref : db.get_unresolved_refs()) {
            if (ref.ref_name != "alpha") continue;
            auto source = db.get_node(ref.source_node_id);
            if (source.has_value() && source->file_path == caller_file.string()) {
                found_unresolved_alpha_from_caller = true;
                break;
            }
        }
        CHECK(found_unresolved_alpha_from_caller);
    }

    fs::remove_all(root);
    std::cout << "  [PASS] incremental_reindex\n";
}

void test_context_aware_same_name_resolution() {
    const fs::path exe = find_codegraph_exe();

    const fs::path root = fs::path("/tmp") / ("codegraph_same_name_" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    const fs::path local_file = root / "local.cpp";
    const fs::path other_file = root / "other.cpp";
    {
        std::ofstream out(local_file);
        out << "void WaitServerReady() {}\n"
            << "void Use() { WaitServerReady(); }\n";
    }
    {
        std::ofstream out(other_file);
        out << "void WaitServerReady() {}\n";
    }

    CHECK(run_codegraph_cli(exe, root, {"init", "-i", root.string()}) == 0);

    {
        Database db((root / ".codegraph" / "index").string());
        auto uses = db.find_nodes_by_name("Use", 5);
        CHECK(uses.size() == 1);

        auto calls = db.get_edges_from(uses[0].id, EdgeKind::Calls);
        bool found_local_target = false;
        bool found_wrong_target = false;
        for (const auto& edge : calls) {
            auto target = db.get_node(edge.target_id);
            CHECK(target.has_value());
            if (target->name == "WaitServerReady" && target->file_path == local_file.string()) {
                found_local_target = true;
            }
            if (target->name == "WaitServerReady" && target->file_path == other_file.string()) {
                found_wrong_target = true;
            }
        }

        CHECK(found_local_target);
        CHECK(!found_wrong_target);
    }

    fs::remove_all(root);
    std::cout << "  [PASS] context_aware_same_name_resolution\n";
}

void test_tarjan_scc() {
    TempDb temp_db("scc.db");

    Database db(temp_db.path);
    db.init_schema();

    // Register the file so get_all_files() finds it
    FileRecord fr;
    fr.path = "/tmp/scc.cpp";
    fr.language = "cpp";
    fr.mtime = 0;
    fr.size = 100;
    db.insert_file(fr);

    // Create a cycle: A -> B -> C -> A
    Node a, b, c, d;
    a.kind = NodeKind::Function; a.name = "a"; a.file_path = "/tmp/scc.cpp"; a.language = "cpp";
    b.kind = NodeKind::Function; b.name = "b"; b.file_path = "/tmp/scc.cpp"; b.language = "cpp";
    c.kind = NodeKind::Function; c.name = "c"; c.file_path = "/tmp/scc.cpp"; c.language = "cpp";
    d.kind = NodeKind::Function; d.name = "d"; d.file_path = "/tmp/scc.cpp"; d.language = "cpp";

    int64_t aid = db.insert_node(a);
    int64_t bid = db.insert_node(b);
    int64_t cid = db.insert_node(c);
    int64_t did = db.insert_node(d);

    // A -> B -> C -> A (cycle)
    Edge e1; e1.source_id = aid; e1.target_id = bid; e1.kind = EdgeKind::Calls; db.insert_edge(e1);
    Edge e2; e2.source_id = bid; e2.target_id = cid; e2.kind = EdgeKind::Calls; db.insert_edge(e2);
    Edge e3; e3.source_id = cid; e3.target_id = aid; e3.kind = EdgeKind::Calls; db.insert_edge(e3);

    // D -> A (not part of the cycle, but reachable)
    Edge e4; e4.source_id = did; e4.target_id = aid; e4.kind = EdgeKind::Calls; db.insert_edge(e4);

    GraphTraverser traverser(db);

    // Find all SCCs
    auto sccs = traverser.find_sccs();
    // Should have at least 2 SCCs: {A,B,C} and {D}
    bool found_cycle = false;
    for (const auto& scc : sccs) {
        if (scc.size() == 3) {
            std::unordered_set<int64_t> ids(scc.begin(), scc.end());
            if (ids.count(aid) && ids.count(bid) && ids.count(cid)) {
                found_cycle = true;
            }
        }
    }
    CHECK(found_cycle);

    // Find circular dependencies (SCCs with size > 1)
    auto cycles = traverser.find_circular_dependencies();
    CHECK(cycles.size() == 1);
    CHECK(cycles[0].size() == 3);

    std::cout << "  [PASS] tarjan_scc\n";
}

void test_find_path() {
    TempDb temp_db("path.db");

    Database db(temp_db.path);
    db.init_schema();

    // A -> B -> C -> D, 有路径 A→D
    Node a, b, c, d;
    a.kind = NodeKind::Function; a.name = "a"; a.file_path = "/tmp/path.cpp"; a.language = "cpp";
    b.kind = NodeKind::Function; b.name = "b"; b.file_path = "/tmp/path.cpp"; b.language = "cpp";
    c.kind = NodeKind::Function; c.name = "c"; c.file_path = "/tmp/path.cpp"; c.language = "cpp";
    d.kind = NodeKind::Function; d.name = "d"; d.file_path = "/tmp/path.cpp"; d.language = "cpp";

    int64_t aid = db.insert_node(a);
    int64_t bid = db.insert_node(b);
    int64_t cid = db.insert_node(c);
    int64_t did = db.insert_node(d);

    Edge e1; e1.source_id = aid; e1.target_id = bid; e1.kind = EdgeKind::Calls; db.insert_edge(e1);
    Edge e2; e2.source_id = bid; e2.target_id = cid; e2.kind = EdgeKind::Calls; db.insert_edge(e2);
    Edge e3; e3.source_id = cid; e3.target_id = did; e3.kind = EdgeKind::Calls; db.insert_edge(e3);

    GraphTraverser traverser(db);

    // 有路径：A → B → C → D
    auto p = traverser.find_path(aid, did);
    CHECK(p.size() == 4);
    CHECK(p[0] == aid);
    CHECK(p[3] == did);

    // 无反向路径：D → A
    auto no_path = traverser.find_path(did, aid);
    CHECK(no_path.empty());

    // 自身到自身
    auto self = traverser.find_path(aid, aid);
    CHECK(self.size() == 1);
    CHECK(self[0] == aid);

    std::cout << "  [PASS] find_path\n";
}

void test_compute_metrics() {
    TempDb temp_db("metrics.db");

    Database db(temp_db.path);
    db.init_schema();

    FileRecord fr; fr.path = "/tmp/metrics.cpp"; fr.language = "cpp"; fr.mtime = 0; fr.size = 100;
    db.insert_file(fr);

    // main -> A -> B -> C
    // main -> B (B 被调用 2 次)
    Node main_n, a, b, c;
    main_n.kind = NodeKind::Function; main_n.name = "main"; main_n.file_path = "/tmp/metrics.cpp"; main_n.language = "cpp";
    a.kind = NodeKind::Function; a.name = "a"; a.file_path = "/tmp/metrics.cpp"; a.language = "cpp";
    b.kind = NodeKind::Function; b.name = "b"; b.file_path = "/tmp/metrics.cpp"; b.language = "cpp";
    c.kind = NodeKind::Function; c.name = "c"; c.file_path = "/tmp/metrics.cpp"; c.language = "cpp";

    int64_t mid = db.insert_node(main_n);
    int64_t aid = db.insert_node(a);
    int64_t bid = db.insert_node(b);
    int64_t cid = db.insert_node(c);

    Edge e1; e1.source_id = mid; e1.target_id = aid; e1.kind = EdgeKind::Calls; db.insert_edge(e1);
    Edge e2; e2.source_id = mid; e2.target_id = bid; e2.kind = EdgeKind::Calls; db.insert_edge(e2);
    Edge e3; e3.source_id = aid; e3.target_id = bid; e3.kind = EdgeKind::Calls; db.insert_edge(e3);
    Edge e4; e4.source_id = bid; e4.target_id = cid; e4.kind = EdgeKind::Calls; db.insert_edge(e4);

    GraphTraverser traverser(db);
    auto metrics = traverser.compute_metrics(5);

    CHECK(metrics.total_nodes == 4);
    CHECK(metrics.total_edges == 4);
    CHECK(metrics.circular_deps == 0);
    // BFS 从 main 出发：main(0) → A(1), B(1) → C(2)。B 先被 main 访问，深度 1。
    // 所以最大深度是 2（main → B → C），不是 3（main → A → B → C 不是最短路径）
    CHECK(metrics.max_call_depth == 2);

    // b 被调用 2 次（main 和 a 各调用一次），应该排第一
    CHECK(!metrics.most_called.empty());
    CHECK(metrics.most_called[0].first.name == "b");
    CHECK(metrics.most_called[0].second == 2);

    // main 调用 2 个函数，a 调用 1 个，b 调用 1 个
    CHECK(!metrics.most_calling.empty());
    CHECK(metrics.most_calling[0].first.name == "main");
    CHECK(metrics.most_calling[0].second == 2);

    std::cout << "  [PASS] compute_metrics\n";
}

void test_impact_chain() {
    TempDb temp_db("impact.db");

    Database db(temp_db.path);
    db.init_schema();

    // A -> B -> C (改 C 会影响 A 和 B)
    Node a, b, c;
    a.kind = NodeKind::Function; a.name = "a"; a.file_path = "/tmp/impact.cpp"; a.language = "cpp";
    b.kind = NodeKind::Function; b.name = "b"; b.file_path = "/tmp/impact.cpp"; b.language = "cpp";
    c.kind = NodeKind::Function; c.name = "c"; c.file_path = "/tmp/impact.cpp"; c.language = "cpp";

    int64_t aid = db.insert_node(a);
    int64_t bid = db.insert_node(b);
    int64_t cid = db.insert_node(c);

    Edge e1; e1.source_id = aid; e1.target_id = bid; e1.kind = EdgeKind::Calls; db.insert_edge(e1);
    Edge e2; e2.source_id = bid; e2.target_id = cid; e2.kind = EdgeKind::Calls; db.insert_edge(e2);

    GraphTraverser traverser(db);

    // 改 C：影响 B 和 A
    auto impact = traverser.get_impact_chain(cid, 5);
    CHECK(impact.size() == 2);

    // 验证每个受影响节点都有反向路径
    // A → B → C，impact(C) = {A, B}
    // 反向路径：C → B（C 被 B 调用），C → B → A（B 被 A 调用）
    bool found_b = false, found_a = false;
    for (const auto& in : impact) {
        if (in.node.name == "b") {
            found_b = true;
            CHECK(in.path.size() == 2);
            CHECK(in.path[0] == cid);  // 从 C 开始
            CHECK(in.path[1] == bid);  // 到 B（B 调用了 C）
        }
        if (in.node.name == "a") {
            found_a = true;
            CHECK(in.path.size() == 3);
            CHECK(in.path[0] == cid);  // 从 C 开始
            CHECK(in.path[2] == aid);  // 到 A（A 调用了 B 调用了 C）
        }
    }
    CHECK(found_b);
    CHECK(found_a);

    std::cout << "  [PASS] impact_chain\n";
}

int main() {
    std::cout << "Running tests...\n";
    test_types();
    test_database();
    test_database_lookup_and_errors();
    test_cpp_extractor();
    test_cpp_member_call_extraction();
    test_run_git_diff_does_not_execute_shell();
    test_python_extractor();
    test_python_call_extraction();
    test_detect_language();
    test_context_builder_splits_callers_and_callees();
    test_incremental_reindex();
    test_context_aware_same_name_resolution();
    test_tarjan_scc();
    test_find_path();
    test_compute_metrics();
    test_impact_chain();
    std::cout << "All tests passed!\n";
    return 0;
}
