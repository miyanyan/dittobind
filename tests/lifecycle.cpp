#ifdef DITTOBIND_REFERENCE
#include <pybind11/embed.h>
#if PY_VERSION_HEX >= 0x030C0000
#include <pybind11/subinterpreter.h>
#endif
namespace binding = pybind11;
namespace execution = pybind11;
#define MODULE PYBIND11_EMBEDDED_MODULE
#else
#include <dittobind/subinterpreter.h>
namespace binding = nanobind;
namespace execution = dittobind;
#define MODULE DITTOBIND_EMBEDDED_MODULE
#endif
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static std::atomic<int> calls{0}, unraisable{0}, destroyed{0}, rejections{0};
static void require(bool result, const char *message) {
    if (!result) throw std::runtime_error(message);
}
static PyConfig config() {
    PyConfig result;
    PyConfig_InitIsolatedConfig(&result);
    result.parse_argv = 0;
    auto status = PyConfig_SetBytesString(&result, &result.home, DITTOBIND_PYTHON_HOME);
    require(!PyStatus_Exception(status), "configure Python home");
    return result;
}
MODULE(lifecycle_host, m, execution::multiple_interpreters::per_interpreter_gil(),
       execution::mod_gil_not_used()) {
    m.def("touch", [] { ++calls; return 42; });
    m.def("unraisable", [](binding::handle) { ++unraisable; });
    m.def("destroyed", [] { ++destroyed; });
#ifndef DITTOBIND_REFERENCE
    m.def("reenter", [] {
        try { execution::finalize_interpreter(); }
        catch (const std::logic_error &) { ++rejections; }
        auto options = config();
        try { execution::initialize_interpreter(&options); }
        catch (const std::logic_error &) { ++rejections; }
        PyConfig_Clear(&options); // rejected initialization does not consume it
    });
#endif
}
static void body(const std::string &scenario) {
    execution::exec("import lifecycle_host as host");
    if (scenario == "shutdown_callbacks" || scenario == "shutdown_reentry") {
        execution::exec(R"(
            import sys, atexit
            sys.unraisablehook = host.unraisable
            class Finalizer:
                def __del__(self):
                    host.destroyed()
                    assert host.touch() == 42
                    raise ValueError('expected destructor error')
            retained = Finalizer()
            def cleanup():
                global retained
                del retained
                assert host.touch() == 42
            def failing_callback():
                raise RuntimeError('expected atexit error')
            atexit.register(cleanup)
            atexit.register(failing_callback)
        )");
#ifndef DITTOBIND_REFERENCE
        if (scenario == "shutdown_reentry")
            execution::exec("atexit.register(host.reenter)");
#endif
    } else if (scenario == "workers_join") {
        std::vector<std::thread> workers;
        std::vector<std::string> errors(8);
        {
            execution::gil_scoped_release release;
            for (int i = 0; i < 8; ++i) workers.emplace_back([&, i] {
                execution::gil_scoped_acquire acquire;
                try {
                    binding::dict local;
                    execution::exec("import lifecycle_host as host\nfor _ in range(100):\n    assert host.touch() == 42\n    try:\n        raise LookupError('worker error')\n    except LookupError:\n        pass", local);
                } catch (const std::exception &e) { errors[i] = e.what(); }
            });
            for (auto &worker : workers) worker.join();
        }
        for (auto &error : errors) require(error.empty(), error.c_str());
    }
#ifndef DITTOBIND_REFERENCE
    else if (scenario == "shutdown_guards") {
        auto reject = [] {
            try { execution::finalize_interpreter(); }
            catch (const std::logic_error &) { ++rejections; }
        };
        {
            execution::gil_scoped_release release;
            reject(); // owning OS thread, but no attached Python thread state
            std::atomic<bool> worker_attached{false}, owner_checked{false};
            std::thread worker([&] {
                execution::gil_scoped_acquire acquire;
                reject(); // attached, but not the owning OS thread
                worker_attached = true;
                while (!owner_checked.load()) std::this_thread::yield();
            });
            while (!worker_attached.load()) std::this_thread::yield();
            reject(); // another worker owns the GIL; owner must still be rejected
            owner_checked = true;
            worker.join();
        }
#if PY_VERSION_HEX >= 0x030C0000
        {
            auto child = execution::subinterpreter::create();
            reject(); // live child prevents main shutdown
            execution::subinterpreter_scoped_activate active(child);
            reject(); // owner thread attached to the wrong interpreter
        }
        require(rejections == 5, "all invalid shutdowns rejected");
#else
        require(rejections == 3, "all invalid shutdowns rejected");
#endif
        require(binding::cast<int>(execution::eval("host.touch()")) == 42, "rejection preserves runtime");
    }
#endif
    else throw std::runtime_error("unknown lifecycle scenario");
}

#ifndef DITTOBIND_REFERENCE
static int reject_bootstrap(const char *event, PyObject *args, void *) {
    if (std::strcmp(event, "import") != 0 || !PyTuple_Check(args) || PyTuple_Size(args) == 0) return 0;
    auto *name = PyTuple_GetItem(args, 0);
    if (PyUnicode_Check(name) && PyUnicode_CompareWithASCIIString(name, "_dittobind_runtime") == 0) {
        PyErr_SetString(PyExc_RuntimeError, "intentional bootstrap import failure");
        return -1;
    }
    return 0;
}
static void startup_failure(const std::string &scenario) {
    auto invalid = config();
    auto valid = config();
    if (scenario == "startup_partial_failure") {
        auto status = PyConfig_SetString(&invalid, &invalid.filesystem_encoding, L"dittobind-invalid-codec");
        require(!PyStatus_Exception(status), "configure invalid codec");
    } else {
        require(PySys_AddAuditHook(&reject_bootstrap, nullptr) == 0, "install startup fault");
    }
    bool rejected = false;
    try { execution::initialize_interpreter(&invalid); }
    catch (const std::runtime_error &) { rejected = true; }
    require(rejected && !Py_IsInitialized(), "startup failure leaves no running interpreter");
    if (scenario == "startup_partial_failure") {
        rejected = false;
        try { execution::initialize_interpreter(&valid); }
        catch (const std::logic_error &) { rejected = true; }
        require(rejected && valid.home, "partial CPython initialization forbids unsafe retry and preserves config");
        PyConfig_Clear(&valid);
    } else {
        execution::scoped_interpreter python(&valid);
        try {
            require(binding::cast<int>(execution::eval("6 * 7")) == 42, "retry after bootstrap rollback");
        } catch (const std::exception &e) { throw std::runtime_error(e.what()); }
    }
}
#endif
int main(int argc, char **argv) {
    try {
        require(argc == 2, "scenario required");
        const std::string scenario = argv[1];
#ifndef DITTOBIND_REFERENCE
        if (scenario.rfind("startup_", 0) == 0) {
            startup_failure(scenario);
            std::cout << "PASS " << scenario << '\n';
            return 0;
        }
#endif
        for (int cycle = 0; cycle < 10; ++cycle) {
            calls = 0; unraisable = 0; destroyed = 0; rejections = 0;
            {
                auto options = config();
                execution::scoped_interpreter python(&options);
                try {
#if PY_VERSION_HEX >= 0x030C0000
                    if (scenario == "sub_shutdown_callbacks") {
                        {
                            execution::scoped_subinterpreter child;
                            try { body("shutdown_callbacks"); }
                            catch (const std::exception &e) { throw std::runtime_error(e.what()); }
                        }
                        require(calls == 2 && destroyed == 1 && unraisable == 2, "child shutdown callbacks completed before main shutdown");
                    } else
#endif
                    body(scenario);
                }
                catch (const std::exception &e) { throw std::runtime_error(e.what()); }
            }
            require(!Py_IsInitialized(), "runtime stopped");
            if (scenario == "shutdown_callbacks" || scenario == "shutdown_reentry") {
                require(calls == 2 && destroyed == 1 && unraisable == 2, "shutdown callbacks and error reporting completed");
                if (scenario == "shutdown_reentry") require(rejections == 2, "shutdown reentry rejected");
            } else if (scenario == "workers_join") require(calls == 800, "all workers finished before shutdown");
        }
        std::cout << "PASS " << scenario << '\n';
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
