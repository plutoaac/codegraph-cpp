#include "codegraph/core/types.h"
#include "codegraph/db/database.h"
#include "codegraph/diff/diff_parser.h"
#include "codegraph/extraction/extractor.h"
#include "codegraph/graph/traverser.h"
#include "codegraph/context/context_builder.h"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <vector>

using namespace codegraph;
namespace fs = std::filesystem;

void test_types() {
    assert(strcmp(node_kind_str(NodeKind::Function), "function") == 0);
    assert(strcmp(edge_kind_str(EdgeKind::Calls), "calls") == 0);
    std::cout << "  [PASS] types\n";
}

void test_database() {
    const char* path = "/tmp/test_codegraph.db";
    std::remove(path);

    Database db(path);
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
    assert(id > 0);

    auto got = db.get_node(id);
    assert(got.has_value());
    assert(got->name == "test_func");
    assert(got->kind == NodeKind::Function);

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
    assert(eid > 0);

    auto callers = db.get_edges_to(id, EdgeKind::Calls);
    assert(callers.size() == 1);
    assert(callers[0].source_id == id2);

    auto results = db.search_fts("test_func");
    assert(!results.empty());

    auto qualified_results = db.search_fts("ns::test_func");
    assert(!qualified_results.empty());

    assert(db.count_nodes() == 2);
    assert(db.count_edges() == 1);

    std::remove(path);
    std::cout << "  [PASS] database\n";
}

void test_database_lookup_and_errors() {
    const char* path = "/tmp/test_codegraph_lookup.db";
    std::remove(path);

    Database db(path);
    db.init_schema();

    Node exact;
    exact.kind = NodeKind::Function;
    exact.name = "foo";
    exact.qualified_name = "ns::foo";
    exact.file_path = "/tmp/a.cpp";
    exact.language = "cpp";
    int64_t exact_id = db.insert_node(exact);
    assert(exact_id > 0);

    Node fuzzy;
    fuzzy.kind = NodeKind::Function;
    fuzzy.name = "foo_helper";
    fuzzy.qualified_name = "ns::foo_helper";
    fuzzy.file_path = "/tmp/b.cpp";
    fuzzy.language = "cpp";
    int64_t fuzzy_id = db.insert_node(fuzzy);
    assert(fuzzy_id > 0);

    auto exact_results = db.find_nodes_by_name("foo", 1);
    assert(exact_results.size() == 1);
    assert(exact_results[0].id == exact_id);

    Edge invalid;
    invalid.source_id = 999999;
    invalid.target_id = exact_id;
    invalid.kind = EdgeKind::Calls;
    assert(db.insert_edge(invalid) < 0);

    bool batch_failed = false;
    try {
        db.insert_edges_batch(std::vector<Edge>{invalid});
    } catch (const std::runtime_error&) {
        batch_failed = true;
    }
    assert(batch_failed);

    std::remove(path);
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
    assert(!result.nodes.empty());

    bool found_add = false, found_calc = false, found_multiply = false;
    bool found_qualified_add = false, found_qualified_multiply = false;
    for (auto& n : result.nodes) {
        if (n.name == "add") found_add = true;
        if (n.name == "Calculator") found_calc = true;
        if (n.name == "multiply") found_multiply = true;
        if (n.qualified_name == "math::add") found_qualified_add = true;
        if (n.qualified_name == "math::Calculator::multiply") found_qualified_multiply = true;
    }
    assert(found_add);
    assert(found_calc);
    assert(found_multiply);
    assert(found_qualified_add);
    assert(found_qualified_multiply);

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

    assert(calls.count("add") > 0);
    assert(calls.count("reset") > 0);
    assert(calls.count("get") > 0);
    std::cout << "  [PASS] cpp_member_call_extraction\n";
}

void test_run_git_diff_does_not_execute_shell() {
    const char* marker = "/tmp/codegraph_git_diff_shell_injection_marker";
    std::remove(marker);

    (void)run_git_diff(std::string("HEAD; touch ") + marker);

    FILE* file = std::fopen(marker, "r");
    assert(file == nullptr);
    if (file != nullptr) {
        std::fclose(file);
        std::remove(marker);
    }

    std::cout << "  [PASS] run_git_diff_does_not_execute_shell\n";
}

void test_context_builder_splits_callers_and_callees() {
    const char* path = "/tmp/test_codegraph_context.db";
    std::remove(path);

    Database db(path);
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
    assert(db.insert_edge(caller_edge) > 0);

    Edge callee_edge;
    callee_edge.source_id = target_id;
    callee_edge.target_id = callee_id;
    callee_edge.kind = EdgeKind::Calls;
    assert(db.insert_edge(callee_edge) > 0);

    GraphTraverser traverser(db);
    ContextBuilder context(db, traverser);
    auto result = context.build_context("target");

    assert(result.contains("callers"));
    assert(result.contains("callees"));
    assert(result["callers"].size() == 1);
    assert(result["callees"].size() == 1);
    assert(result["callers"][0]["name"] == "caller");
    assert(result["callees"][0]["name"] == "callee");

    std::remove(path);
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
    assert(!result.nodes.empty());

    bool found_hello = false, found_greeter = false;
    for (auto& n : result.nodes) {
        if (n.name == "hello") found_hello = true;
        if (n.name == "Greeter") found_greeter = true;
    }
    assert(found_hello);
    assert(found_greeter);

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
    assert(!result.unresolved.empty());

    bool found_bar_call = false, found_foo_call = false;
    for (auto& ref : result.unresolved) {
        if (ref.ref_name == "bar") found_bar_call = true;
        if (ref.ref_name == "foo") found_foo_call = true;
    }
    assert(found_bar_call);
    assert(found_foo_call);
    std::cout << "  [PASS] python_call_extraction\n";
}

void test_detect_language() {
    assert(detect_language("foo.cpp") == "cpp");
    assert(detect_language("foo.cc") == "cpp");
    assert(detect_language("foo.cxx") == "cpp");
    assert(detect_language("foo.c") == "cpp");
    assert(detect_language("foo.h") == "cpp");
    assert(detect_language("foo.hpp") == "cpp");
    assert(detect_language("foo.hxx") == "cpp");
    assert(detect_language("foo.hh") == "cpp");
    assert(detect_language("foo.py") == "python");
    assert(detect_language("foo.pyi") == "python");
    assert(detect_language("foo.txt") == "");
    assert(detect_language("foo.rs") == "");
    std::cout << "  [PASS] detect_language\n";
}

int run_codegraph_cli(const fs::path& exe, const fs::path& cwd, const std::vector<std::string>& args) {
    pid_t pid = fork();
    assert(pid >= 0);
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
        assert(errno == EINTR);
    }
    if (!WIFEXITED(status)) return 1;
    return WEXITSTATUS(status);
}

void test_incremental_reindex() {
    const fs::path exe = fs::current_path() / "codegraph";
    assert(fs::exists(exe));

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

    assert(run_codegraph_cli(exe, root, {"init", "-i", root.string()}) == 0);

    const fs::path db_path = root / ".codegraph" / "index";
    int64_t stable_id_before = 0;
    {
        Database db(db_path.string());
        auto stable_nodes = db.find_nodes_by_name("stable", 5);
        assert(stable_nodes.size() == 1);
        stable_id_before = stable_nodes[0].id;
        assert(!db.find_nodes_by_name("alpha", 5).empty());
        assert(!db.find_nodes_by_name("beta", 5).empty());
        assert(!db.find_nodes_by_name("external", 5).empty());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    {
        std::ofstream out(changed_file);
        out << "void gamma() {}\n";
    }

    assert(run_codegraph_cli(exe, root, {"index", root.string()}) == 0);

    {
        Database db(db_path.string());
        auto gamma_nodes = db.find_nodes_by_name("gamma", 5);
        assert(gamma_nodes.size() == 1);
        assert(gamma_nodes[0].file_path == changed_file.string());

        auto stable_nodes = db.find_nodes_by_name("stable", 5);
        assert(stable_nodes.size() == 1);
        assert(stable_nodes[0].id == stable_id_before);

        for (const auto& n : db.find_nodes_by_name("alpha", 5)) {
            assert(n.file_path != changed_file.string());
        }
        for (const auto& n : db.find_nodes_by_name("beta", 5)) {
            assert(n.file_path != changed_file.string());
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
        assert(found_unresolved_alpha_from_caller);
    }

    fs::remove_all(root);
    std::cout << "  [PASS] incremental_reindex\n";
}

void test_context_aware_same_name_resolution() {
    const fs::path exe = fs::current_path() / "codegraph";
    assert(fs::exists(exe));

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

    assert(run_codegraph_cli(exe, root, {"init", "-i", root.string()}) == 0);

    {
        Database db((root / ".codegraph" / "index").string());
        auto uses = db.find_nodes_by_name("Use", 5);
        assert(uses.size() == 1);

        auto calls = db.get_edges_from(uses[0].id, EdgeKind::Calls);
        bool found_local_target = false;
        bool found_wrong_target = false;
        for (const auto& edge : calls) {
            auto target = db.get_node(edge.target_id);
            assert(target.has_value());
            if (target->name == "WaitServerReady" && target->file_path == local_file.string()) {
                found_local_target = true;
            }
            if (target->name == "WaitServerReady" && target->file_path == other_file.string()) {
                found_wrong_target = true;
            }
        }

        assert(found_local_target);
        assert(!found_wrong_target);
    }

    fs::remove_all(root);
    std::cout << "  [PASS] context_aware_same_name_resolution\n";
}

void test_tarjan_scc() {
    const char* path = "/tmp/test_codegraph_scc.db";
    std::remove(path);

    Database db(path);
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
    assert(found_cycle);

    // Find circular dependencies (SCCs with size > 1)
    auto cycles = traverser.find_circular_dependencies();
    assert(cycles.size() == 1);
    assert(cycles[0].size() == 3);

    std::remove(path);
    std::cout << "  [PASS] tarjan_scc\n";
}

void test_find_path() {
    const char* path = "/tmp/test_codegraph_path.db";
    std::remove(path);

    Database db(path);
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
    assert(p.size() == 4);
    assert(p[0] == aid);
    assert(p[3] == did);

    // 无反向路径：D → A
    auto no_path = traverser.find_path(did, aid);
    assert(no_path.empty());

    // 自身到自身
    auto self = traverser.find_path(aid, aid);
    assert(self.size() == 1);
    assert(self[0] == aid);

    std::remove(path);
    std::cout << "  [PASS] find_path\n";
}

void test_compute_metrics() {
    const char* path = "/tmp/test_codegraph_metrics.db";
    std::remove(path);

    Database db(path);
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

    assert(metrics.total_nodes == 4);
    assert(metrics.total_edges == 4);
    assert(metrics.circular_deps == 0);
    // BFS 从 main 出发：main(0) → A(1), B(1) → C(2)。B 先被 main 访问，深度 1。
    // 所以最大深度是 2（main → B → C），不是 3（main → A → B → C 不是最短路径）
    assert(metrics.max_call_depth == 2);

    // b 被调用 2 次（main 和 a 各调用一次），应该排第一
    assert(!metrics.most_called.empty());
    assert(metrics.most_called[0].first.name == "b");
    assert(metrics.most_called[0].second == 2);

    // main 调用 2 个函数，a 调用 1 个，b 调用 1 个
    assert(!metrics.most_calling.empty());
    assert(metrics.most_calling[0].first.name == "main");
    assert(metrics.most_calling[0].second == 2);

    std::remove(path);
    std::cout << "  [PASS] compute_metrics\n";
}

void test_impact_chain() {
    const char* path = "/tmp/test_codegraph_impact.db";
    std::remove(path);

    Database db(path);
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
    assert(impact.size() == 2);

    // 验证每个受影响节点都有反向路径
    // A → B → C，impact(C) = {A, B}
    // 反向路径：C → B（C 被 B 调用），C → B → A（B 被 A 调用）
    bool found_b = false, found_a = false;
    for (const auto& in : impact) {
        if (in.node.name == "b") {
            found_b = true;
            assert(in.path.size() == 2);
            assert(in.path[0] == cid);  // 从 C 开始
            assert(in.path[1] == bid);  // 到 B（B 调用了 C）
        }
        if (in.node.name == "a") {
            found_a = true;
            assert(in.path.size() == 3);
            assert(in.path[0] == cid);  // 从 C 开始
            assert(in.path[2] == aid);  // 到 A（A 调用了 B 调用了 C）
        }
    }
    assert(found_b);
    assert(found_a);

    std::remove(path);
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
