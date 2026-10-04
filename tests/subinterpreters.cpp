#ifdef DITTOBIND_REFERENCE
#include <pybind11/embed.h>
#include <pybind11/subinterpreter.h>
#include <pybind11/functional.h>
namespace binding = pybind11;
namespace execution = pybind11;
#define MODULE PYBIND11_EMBEDDED_MODULE
#else
#include <dittobind/subinterpreter.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/function.h>
namespace binding = nanobind;
namespace execution = dittobind;
#define MODULE DITTOBIND_EMBEDDED_MODULE
#endif
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <cstring>
#include "domain_api.h"

static std::atomic<int> rendezvous{0};
#ifndef DITTOBIND_REFERENCE
static bool reject_child_bootstrap = false;
static int child_startup_fault(const char *event, PyObject *args, void *) {
    if (!reject_child_bootstrap || std::strcmp(event, "import") != 0 ||
        !PyTuple_Check(args) || PyTuple_Size(args) == 0) return 0;
    auto *name = PyTuple_GetItem(args, 0);
    if (PyUnicode_Check(name) && PyUnicode_CompareWithASCIIString(name, "_dittobind_runtime") == 0) {
        PyErr_SetString(PyExc_RuntimeError, "intentional child bootstrap failure");
        return -1;
    }
    return 0;
}
#endif
struct Box { int value; explicit Box(int v) : value(v) {} int get() const { return value; } };
MODULE(isolated_host, m, execution::multiple_interpreters::per_interpreter_gil(), execution::mod_gil_not_used()) {
    binding::class_<Box>(m, "Box").def(binding::init<int>()).def("get", &Box::get);
    m.def("add", [](int a, int b) { return a + b; });
    m.def("invoke", [](const std::function<int()> &callback) { return callback(); },
          binding::call_guard<binding::gil_scoped_release>());
    m.def("meet", [] {
#ifdef Py_GIL_DISABLED
        // Let a stop-the-world collection in the other interpreter complete
        // while waiting. Conventional builds keep their own GIL here.
        execution::gil_scoped_release allow_collection;
#endif
        ++rendezvous;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (rendezvous.load() != 2) {
            if (std::chrono::steady_clock::now() > end)
                throw std::runtime_error("Interpreters did not progress concurrently while holding their GILs");
            std::this_thread::yield();
        }
    });
}
MODULE(unsupported_host, m) { m.def("answer", [] { return 42; }); }
MODULE(shared_host, m, execution::multiple_interpreters::shared_gil()) {
    m.def("answer", [] { return 42; });
}
static void require(bool result, const char *message) {
    if (!result) throw std::runtime_error(message);
}
static void script(const char *source) { execution::exec(binding::str(source)); }
static void exercise() {
    execution::globals()["extension_dir"] = binding::str(DITTOBIND_EXTENSION_DIR);
    script("import sys\nsys.path.insert(0, extension_dir)");
#ifdef DITTOBIND_REFERENCE
    script("import pybind11_ext as ext");
#else
    script("import dittobind_ext as ext");
#endif
    script("assert ext.Value(21).twice() == 42\nassert ext.add(20, 22) == 42");
#ifdef Py_GIL_DISABLED
    script("assert not sys._is_gil_enabled()");
#endif
    const auto interpreter_id = execution::subinterpreter::current().id();
    {
        execution::gil_scoped_release release;
        {
            execution::gil_scoped_acquire acquire;
            require(execution::subinterpreter::current().id() == interpreter_id,
                    "GIL acquire must reattach the released subinterpreter");
        }
    }
    script("import isolated_host as host\nassert host.add(20, 22) == 42\nbox = host.Box(42)\nassert box.get() == 42");
    script("assert host.invoke(lambda: box.get()) == 42");
    // Exercise nanobind's formerly process-global dedent import cache as well.
    binding::eval<binding::eval_statements>(R"(
        from_cache = 42
    )", execution::globals());
    script("assert from_cache == 42");
}
static void test(const std::string &scenario) {
    const auto main_id = execution::subinterpreter::current().id();
    require(main_id == 0, "main is active");
#ifndef DITTOBIND_REFERENCE
    if (scenario == "sub_bootstrap_failure") {
        require(PySys_AddAuditHook(child_startup_fault, nullptr) == 0, "install child startup fault");
        for (int i = 0; i < 5; ++i) {
            reject_child_bootstrap = true;
            bool failed = false;
            try { auto child = execution::subinterpreter::create(); }
            catch (const std::runtime_error &) { failed = true; }
            reject_child_bootstrap = false;
            require(failed, "child bootstrap import failed");
            require(execution::subinterpreter::current().id() == main_id, "failed bootstrap restores main");
            require(nanobind::detail::interpreter_record_count() == 1, "failed child leaves no backend record");
            auto recovered = execution::subinterpreter::create();
            execution::subinterpreter_scoped_activate active(recovered);
            try { exercise(); }
            catch (const std::exception &e) { throw std::runtime_error(e.what()); }
        }
        return;
    }
    if (scenario == "sub_domains_late") {
        domain_diagnostics *api = nullptr;
        {
            auto sub = execution::subinterpreter::create();
            execution::subinterpreter_scoped_activate active(sub);
            try {
                execution::globals()["extension_dir"] = binding::str(DITTOBIND_EXTENSION_DIR);
                script("import sys\nsys.path.insert(0, extension_dir)\nimport dittobind_domain_ext as domain");
                auto capsule = execution::globals()["domain"].attr("diagnostics");
                api = static_cast<domain_diagnostics *>(PyCapsule_GetPointer(capsule.ptr(), "dittobind.test.domain"));
                require(api, "native domain diagnostics");
            } catch (const std::exception &e) { throw std::runtime_error(e.what()); }
        }
        require(api->records() == 0, "child-only DLL released before main shutdown");
        for (int i = 0; i < 40; ++i) {
            int before = api->constructed();
            {
                auto sub = execution::subinterpreter::create();
                execution::subinterpreter_scoped_activate active(sub);
                try {
                    execution::globals()["extension_dir"] = binding::str(DITTOBIND_EXTENSION_DIR);
                    script("import sys, atexit\nsys.path.insert(0, extension_dir)\ndef late_import():\n    import dittobind_domain_ext as domain\n    assert domain.Value(42).value == 42\natexit.register(late_import)");
                } catch (const std::exception &e) { throw std::runtime_error(e.what()); }
            }
            require(api->constructed() == before + 1 && api->live() == 0, "atexit import and object destruction ran");
            require(api->records() == 0, "backend imported during shutdown was swept");
        }
        return;
    }
    if (scenario == "sub_domains") {
        execution::globals()["extension_dir"] = binding::str(DITTOBIND_EXTENSION_DIR);
        script("import sys\nsys.path.insert(0, extension_dir)\nimport dittobind_domain_ext as domain");
        script("assert domain.records() == 1");
        for (int i = 0; i < 40; ++i) {
            {
                auto sub = execution::subinterpreter::create();
                execution::subinterpreter_scoped_activate active(sub);
                try {
                    execution::globals()["extension_dir"] = binding::str(DITTOBIND_EXTENSION_DIR);
                    script("import sys\nsys.path.insert(0, extension_dir)\nimport dittobind_domain_ext as domain\nassert domain.Value(42).value == 42\nassert domain.records() == 2");
                } catch (const std::exception &e) { throw std::runtime_error(e.what()); }
            }
            script("assert domain.records() == 1, 'dead interpreter backend retained'");
            require(nanobind::detail::interpreter_record_count() == 1, "host child backend released");
        }
        return;
    }
#endif
    if (scenario == "sub_lifecycle") {
        execution::subinterpreter empty;
        require(empty.empty() && !empty && empty.id() == -1, "empty handle");
        bool rejected = false;
        try { execution::subinterpreter_scoped_activate invalid(empty); }
        catch (const std::exception &) { rejected = true; }
        require(rejected, "empty activation rejected");
        auto original = execution::subinterpreter::create();
        auto id = original.id();
        auto moved = std::move(original);
        require(original.empty() && moved.id() == id, "move construction");
        empty = std::move(moved);
        require(moved.empty() && empty.id() == id, "move assignment");
        {
            execution::scoped_subinterpreter scoped;
            try { exercise(); }
            catch (const std::exception &e) { throw std::runtime_error(e.what()); }
        }
        require(execution::subinterpreter::current().id() == main_id, "scoped restoration");
        return;
    }
    if (scenario == "sub_failure") {
        PyInterpreterConfig invalid{};
        // A private allocator requires extension compatibility checks.
        invalid.use_main_obmalloc = 0;
        invalid.check_multi_interp_extensions = 0;
        invalid.gil = PyInterpreterConfig_OWN_GIL;
        bool rejected = false;
        try { auto broken = execution::subinterpreter::create(invalid); }
        catch (const std::exception &) { rejected = true; }
        require(rejected, "incompatible subinterpreter config rejected");
        require(execution::subinterpreter::current().id() == main_id, "failed creation restored main");
        auto recovered = execution::subinterpreter::create();
        execution::subinterpreter_scoped_activate active(recovered);
        try { exercise(); }
        catch (const std::exception &e) { throw std::runtime_error(e.what()); }
        return;
    }
    if (scenario == "sub_worker") {
        std::string error;
        auto cross_thread = execution::subinterpreter::create();
        {
            execution::gil_scoped_release release;
            std::thread worker([sub = std::move(cross_thread), &error]() mutable {
                try {
                    {
                        execution::subinterpreter_scoped_activate active(sub);
                        try { exercise(); }
                        catch (const std::exception &e) { throw std::runtime_error(e.what()); }
                    }
                    auto local = execution::subinterpreter::create();
                    {
                        execution::subinterpreter_scoped_activate active(local);
                        try { exercise(); }
                        catch (const std::exception &e) { throw std::runtime_error(e.what()); }
                    }
                    // Destroy an interpreter created on another OS thread.
                } catch (const std::exception &e) { error = e.what(); }
            });
            worker.join();
        }
        require(error.empty(), error.c_str());
        require(execution::subinterpreter::current().id() == main_id, "main restored after worker destruction");
        return;
    }
    auto first = execution::subinterpreter::create();
    auto second = execution::subinterpreter::create();
    require(first.id() != second.id(), "distinct interpreter ids");
    if (scenario == "sub_concurrent") {
        std::string errors[2];
        auto run = [&](int index, execution::subinterpreter &sub) {
            execution::subinterpreter_scoped_activate active(sub);
            try {
                exercise();
                script("host.meet()\nfor i in range(200):\n    assert host.Box(i).get() == i\n    assert host.add(i, 1) == i + 1");
            } catch (const std::exception &e) { errors[index] = e.what(); }
        };
        {
            execution::gil_scoped_release release;
            std::thread a(run, 0, std::ref(first));
            std::thread b(run, 1, std::ref(second));
            a.join(); b.join();
        }
        if (!errors[0].empty() || !errors[1].empty()) throw std::runtime_error(errors[0] + errors[1]);
    } else {
        {
            execution::subinterpreter_scoped_activate active(first);
            try {
                exercise();
                script("marker = 101\nimport sys\nsys.modules['private_marker'] = host");
                first.state_dict()["marker"] = binding::int_(1);
                {
                    execution::subinterpreter_scoped_activate nested(second);
                    try {
                        exercise();
                        script("assert 'marker' not in globals()\nimport sys\nassert 'private_marker' not in sys.modules");
                        require(!second.state_dict().contains("marker"), "state dictionaries are isolated");
                        {
                            execution::subinterpreter_scoped_activate back(execution::subinterpreter::main());
                            try { script("assert 'marker' not in globals()"); }
                            catch (const std::exception &e) { throw std::runtime_error(e.what()); }
                        }
                        require(execution::subinterpreter::current().id() == second.id(), "restore nested subinterpreter");
                    } catch (const std::exception &e) { throw std::runtime_error(e.what()); }
                }
                script("assert marker == 101");
                require(execution::subinterpreter::current().id() == first.id(), "restore outer subinterpreter");
                script("try:\n    import unsupported_host\nexcept ImportError:\n    pass\nelse:\n    raise AssertionError('unsupported module imported')");
                script("try:\n    import shared_host\nexcept ImportError:\n    pass\nelse:\n    raise AssertionError('shared-GIL module imported into own-GIL interpreter')");
            } catch (const std::exception &e) { throw std::runtime_error(e.what()); }
        }
        require(execution::subinterpreter::current().id() == main_id, "restore main interpreter");
        PyInterpreterConfig config{};
        config.allow_threads = 1;
        config.check_multi_interp_extensions = 1;
        config.gil = PyInterpreterConfig_SHARED_GIL;
        auto shared = execution::subinterpreter::create(config);
        {
            execution::subinterpreter_scoped_activate active(shared);
            try { script("import shared_host\nassert shared_host.answer() == 42"); }
            catch (const std::exception &e) { throw std::runtime_error(e.what()); }
        }
    }
}
int main(int argc, char **argv) {
    try {
        require(argc >= 2, "scenario required");
        const int cycles = argc == 3 ? std::stoi(argv[2]) : 3;
        for (int cycle = 0; cycle < cycles; ++cycle) {
            PyConfig config;
            PyConfig_InitIsolatedConfig(&config);
            config.parse_argv = 0;
            auto status = PyConfig_SetBytesString(&config, &config.home, DITTOBIND_PYTHON_HOME);
            require(!PyStatus_Exception(status), "Python home");
            execution::scoped_interpreter python(&config, 0, nullptr, false);
            rendezvous = 0;
            try { test(argv[1]); }
            catch (const std::exception &e) { throw std::runtime_error(e.what()); }
        }
        std::cout << "PASS " << argv[1] << '\n';
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
