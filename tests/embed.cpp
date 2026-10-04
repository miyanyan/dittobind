#ifdef DITTOBIND_REFERENCE
#include <pybind11/embed.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>
namespace binding = pybind11;
namespace execution = pybind11;
using python_error = pybind11::error_already_set;
using return_policy = pybind11::return_value_policy;
#define EMBEDDED_MODULE PYBIND11_EMBEDDED_MODULE
#else
#include <dittobind/embed.h>
#include <nanobind/stl/function.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
namespace binding = nanobind;
namespace execution = dittobind;
using python_error = nanobind::python_error;
using return_policy = nanobind::rv_policy;
#define EMBEDDED_MODULE DITTOBIND_EMBEDDED_MODULE
#endif

#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

static binding::module_ import_module(const char *name) {
#ifdef DITTOBIND_REFERENCE
    return binding::module_::import(name);
#else
    return binding::module_::import_(name);
#endif
}

struct Counter {
    int value;
    explicit Counter(int initial) : value(initial) {}
    void add(int amount) { value += amount; }
};

struct Tracked {
    static std::atomic<int> live;
    Tracked() { ++live; }
    ~Tracked() { --live; }
};
std::atomic<int> Tracked::live{0};

EMBEDDED_MODULE(test_host, m) {
    m.def("add", [](int a, int b) { return a + b; });
    m.def("callback", [](const std::function<int(int)> &fn, int value) { return fn(value); });
    m.def("sum", [](const std::vector<int> &items) {
        int total = 0;
        for (int item : items) total += item;
        return total;
    });
    m.def("fail", [] { throw std::runtime_error("C++ failure"); });
    m.def("frame_eval", [] { return execution::eval("frame_marker"); });
    m.def("frame_exec", [] { execution::exec("frame_written = frame_marker + 1"); });
    auto counter = binding::class_<Counter>(m, "Counter");
    counter.def(binding::init<int>()).def("add", &Counter::add);
#ifdef DITTOBIND_REFERENCE
    counter.def_readwrite("value", &Counter::value);
    binding::class_<Tracked, std::shared_ptr<Tracked>>(m, "Tracked");
#else
    counter.def_rw("value", &Counter::value);
    binding::class_<Tracked>(m, "Tracked");
#endif
    m.def("make_tracked", [] { return std::make_shared<Tracked>(); });
}

EMBEDDED_MODULE(broken_host, m) {
    m.def("partial", [] { return 1; });
    throw std::runtime_error("intentional module failure");
}

static void run_scenario(const std::string &scenario) {
    auto globals = binding::cast<binding::dict>(import_module("__main__").attr("__dict__"));
    if (scenario == "bootstrap") {
        require(binding::cast<int>(execution::eval("6 * 7", globals)) == 42, "bootstrap eval");
        return;
    }
    auto host = import_module("test_host");
    globals["host"] = host;

    if (scenario == "eval") {
        execution::exec("default_answer = 42");
        require(binding::cast<int>(execution::eval("default_answer")) == 42, "default __main__ globals");
        execution::exec(R"(
            indented_answer = 42
        )");
        require(binding::cast<int>(execution::eval("indented_answer")) == 42, "dedent string literal");
        execution::exec(std::string("dynamic_answer = 42"));
        require(binding::cast<int>(execution::eval(std::string("dynamic_answer"))) == 42, "runtime source string");
        execution::exec("answer = 40 + 2", globals);
        require(binding::cast<int>(execution::eval("answer", globals)) == 42, "exec/eval");
        binding::dict separate;
        separate["x"] = 17;
        require(binding::cast<int>(execution::eval("x + 1", globals, separate)) == 18, "locals");
        require(!globals.contains("x"), "locals must not pollute globals");
        require(binding::cast<std::string>(binding::str("中文测试")) == "中文测试", "Unicode");
    } else if (scenario == "calls") {
        execution::exec("def twice(x):\n    return x * 2\n", globals);
        require(binding::cast<int>(globals["twice"](21)) == 42, "C++ calls Python");
        require(binding::cast<int>(execution::eval("host.add(20, 22)", globals)) == 42, "Python calls C++");
        require(binding::cast<int>(execution::eval("host.callback(twice, 21)", globals)) == 42, "callback");
        require(binding::cast<int>(host.attr("sum")(std::vector<int>{10, 12, 20})) == 42, "STL conversion");
    } else if (scenario == "classes") {
        execution::exec("counter = host.Counter(40)\ncounter.add(2)", globals);
        auto &counter = binding::cast<Counter &>(globals["counter"]);
        require(counter.value == 42, "Python owned C++ class");
        Counter owned(20);
        globals["owned"] = binding::cast(&owned, return_policy::reference);
        execution::exec("owned.add(22)", globals);
        require(owned.value == 42, "host owned object");
        globals.attr("pop")("owned");
    } else if (scenario == "exceptions") {
        bool caught = false;
        try {
            execution::exec("raise ValueError('Python failure')", globals);
        } catch (const python_error &error) {
            auto message = std::string(error.what());
            caught = message.find("ValueError") != std::string::npos &&
                     message.find("Python failure") != std::string::npos;
        }
        require(caught, "Python exception translation");
        execution::exec("try:\n    host.fail()\nexcept RuntimeError as e:\n    assert str(e) == 'C++ failure'\nelse:\n    raise AssertionError('missing C++ exception')", globals);
        execution::exec("def bad(x):\n    raise LookupError('callback failure')", globals);
        caught = false;
        try { host.attr("callback")(globals["bad"], 1); }
        catch (const python_error &error) { caught = std::string(error.what()).find("LookupError") != std::string::npos; }
        require(caught, "callback exception round trip");
        require(binding::cast<int>(execution::eval("40 + 2", globals)) == 42, "recovery after exception");
    } else if (scenario == "ownership") {
        {
            auto object = host.attr("make_tracked")();
            auto shared = binding::cast<std::shared_ptr<Tracked>>(object);
            require(Tracked::live == 1, "one shared instance");
            object = binding::none();
            require(Tracked::live == 1, "C++ shared_ptr retains instance");
        }
        require(Tracked::live == 0, "shared ownership released");
        execution::exec("retained = host.make_tracked()", globals);
        require(Tracked::live == 1, "Python owns instance until shutdown");
    } else if (scenario == "threads") {
        int result = 0;
        std::string error;
        {
            binding::gil_scoped_release release;
            std::thread worker([&] {
                binding::gil_scoped_acquire acquire;
                try {
                    result = binding::cast<int>(import_module("test_host").attr("add")(20, 22));
                } catch (const std::exception &e) { error = e.what(); }
            });
            worker.join();
        }
        require(error.empty() && result == 42, "worker thread call under GIL");
    } else if (scenario == "config") {
        execution::exec("import sys, fixture_module\nassert sys.argv == ['embedded', '--flag', 'value']\nassert fixture_module.answer == 42\nassert sys.flags.isolated == 1", globals);
    } else if (scenario == "file") {
        std::string path = std::string(DITTOBIND_FIXTURES) + "/script.py";
#ifdef DITTOBIND_REFERENCE
        binding::eval_file(path, globals);
#else
        dittobind::exec_file(path, globals);
#endif
        require(binding::cast<int>(globals["file_answer"]) == 42, "exec_file result");
        require(binding::cast<std::string>(globals["__file__"]) == path, "file name");
#ifndef DITTOBIND_REFERENCE
        bool caught = false;
        try { dittobind::exec_file(path + ".missing", globals); }
        catch (const std::runtime_error &e) { caught = std::string(e.what()).find("could not be opened") != std::string::npos; }
        require(caught, "missing script");
        caught = false;
        try { dittobind::exec_file(std::string(DITTOBIND_FIXTURES) + "/broken.py", globals); }
        catch (const python_error &e) {
            auto message = std::string(e.what());
            caught = message.find("broken.py") != std::string::npos && message.find("file failure") != std::string::npos;
        }
        require(caught, "script traceback");
#endif
    } else if (scenario == "module_failure") {
        bool caught = false;
        try { import_module("broken_host"); }
        catch (const python_error &e) { caught = std::string(e.what()).find("intentional module failure") != std::string::npos; }
        require(caught, "module import failure");
        require(binding::cast<int>(host.attr("add")(20, 22)) == 42, "healthy module still works");
    } else if (scenario == "lifecycle") {
#ifndef DITTOBIND_REFERENCE
        bool rejected = false;
        try { dittobind::scoped_interpreter duplicate; }
        catch (const std::logic_error &) { rejected = true; }
        require(rejected && Py_IsInitialized(), "duplicate guard must not finalize owner");
        rejected = false;
        try { dittobind::detail::embedded_module late("late", &PyInit_test_host); }
        catch (const std::logic_error &) { rejected = true; }
        require(rejected, "late registration rejected");
#endif
    } else if (scenario != "invalid_config") {
        throw std::runtime_error("Unknown test scenario: " + scenario);
    }
}

#include "parity.h"

int main(int argc, char **argv) {
    try {
        require(argc == 2, "Specify a test scenario");
        const std::string scenario = argv[1];
        if (scenario.rfind("parity_", 0) == 0) return run_parity(scenario);
        int result = 0;
#ifdef DITTOBIND_REFERENCE
        PyConfig config;
        PyConfig_InitIsolatedConfig(&config);
        config.parse_argv = 0;
        auto status = PyConfig_SetBytesString(&config, &config.program_name, DITTOBIND_PYTHON_EXECUTABLE);
        require(!PyStatus_Exception(status), "reference program_name");
        status = PyConfig_SetBytesString(&config, &config.home, DITTOBIND_PYTHON_HOME);
        require(!PyStatus_Exception(status), "reference home");
        const char *python_argv[] = {"embedded", "--flag", "value"};
        {
            binding::scoped_interpreter python(&config, 3, python_argv, false);
            try {
                import_module("sys").attr("path").attr("insert")(0, DITTOBIND_FIXTURES);
                run_scenario(scenario);
            } catch (const std::exception &e) { std::cerr << e.what() << '\n'; result = 1; }
        }
#else
        dittobind::interpreter_config config;
        config.program_name = DITTOBIND_PYTHON_EXECUTABLE;
        config.python_home = DITTOBIND_PYTHON_HOME;
        config.argv = {"embedded", "--flag", "value"};
        config.search_paths = {DITTOBIND_FIXTURES};
        config.isolated = true;
        config.add_program_dir_to_path = false;
        if (scenario == "external") {
            PyConfig external;
            PyConfig_InitIsolatedConfig(&external);
            require(!PyStatus_Exception(PyConfig_SetBytesString(&external, &external.program_name,
                        DITTOBIND_PYTHON_EXECUTABLE)), "external config");
            require(!PyStatus_Exception(PyConfig_SetBytesString(&external, &external.home,
                        DITTOBIND_PYTHON_HOME)), "external home");
            auto status = Py_InitializeFromConfig(&external);
            PyConfig_Clear(&external);
            require(!PyStatus_Exception(status), "external initialize");
            bool rejected = false;
            try { dittobind::scoped_interpreter python(config); }
            catch (const std::logic_error &) { rejected = true; }
            bool alive = Py_IsInitialized();
            Py_FinalizeEx();
            require(rejected && alive, "external interpreter ownership");
            return 0;
        }
        if (scenario == "invalid_config") {
            auto invalid = config;
            invalid.program_name = std::string("bad\0path", 8);
            bool rejected = false;
            try { dittobind::scoped_interpreter python(invalid); }
            catch (const std::invalid_argument &) { rejected = true; }
            require(rejected && !Py_IsInitialized(), "invalid configuration rejected before startup");
            invalid = config;
            invalid.executable = std::string("bad\0exe", 7);
            rejected = false;
            try { dittobind::scoped_interpreter python(invalid); }
            catch (const std::invalid_argument &) { rejected = true; }
            require(rejected && !Py_IsInitialized(), "invalid executable rejected before startup");
        }
        {
            dittobind::scoped_interpreter python(config);
            try { run_scenario(scenario); }
            catch (const std::exception &e) { std::cerr << e.what() << '\n'; result = 1; }
        }
        require(!Py_IsInitialized(), "interpreter finalized");
        if (scenario == "lifecycle") {
            dittobind::scoped_interpreter restart(config);
            try { run_scenario("calls"); }
            catch (const std::exception &e) { std::cerr << e.what() << '\n'; result = 1; }
        }
#endif
        require(Tracked::live == 0, "Python owned instances released at shutdown");
        if (!result) std::cout << "PASS " << scenario << '\n';
        return result;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
