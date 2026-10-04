#include <dittobind/embed.h>
#include <nanobind/stl/string.h>
#include <cstdio>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#if PY_VERSION_HEX >= 0x030D0000
// Removed from headers in 3.13, but retained as a Stable ABI symbol. Passing
// nullptr clears CPython's legacy process-wide path cache without Python refs.
extern "C" PyAPI_FUNC(void) Py_SetPath(const wchar_t *);
#endif

NB_MODULE(_dittobind_runtime, m) { m.attr("version") = "0.2.0"; }
static PyObject *runtime_module_init() {
    static PyObject *definition = dittobind::detail::configure_module(
        PyInit__dittobind_runtime(), dittobind::detail::module_options{
            dittobind::multiple_interpreters::per_interpreter_gil(), dittobind::mod_gil_not_used()});
    return definition;
}

namespace dittobind {
namespace {
enum class phase { idle, starting, running, stopping, failed };
struct runtime_state {
    std::mutex mutex;
    phase status = phase::idle;
    std::thread::id owner;
    bool bootstrap_registered = false;
    size_t registered_modules = 0;
    size_t subinterpreters = 0;
    // inittab borrows these names. deque preserves them across registrations.
    std::deque<std::pair<std::string, detail::embedded_module::init_function>> modules;
};
runtime_state &state() { static runtime_state value; return value; }
struct config_owner {
    PyConfig *value;
    ~config_owner() { PyConfig_Clear(value); }
};
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
void clear_path_cache() noexcept { Py_SetPath(nullptr); }
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
void check_status(PyStatus status) {
    if (PyStatus_Exception(status))
        throw std::runtime_error(std::string("Python initialization failed: ") +
            (status.err_msg ? status.err_msg : "unexpected exit request"));
}
void validate_string(const std::string &value) {
    if (value.find('\0') != std::string::npos)
        throw std::invalid_argument("Python configuration contains an embedded NUL");
}
void validate_argv(int argc, const char *const *argv) {
    if (argc < 0 || (argc && !argv)) throw std::invalid_argument("Invalid Python argv");
    for (int i = 0; i < argc; ++i)
        if (!argv[i]) throw std::invalid_argument("Null entry in Python argv");
}
// Never hold this mutex while executing Python, including startup/shutdown.
void claim_startup() {
    auto &runtime = state();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (runtime.status != phase::idle || Py_IsInitialized())
        throw std::logic_error("Python is already running, changing state, or a previous startup failed");
    if (!runtime.bootstrap_registered) {
        if (PyImport_AppendInittab("_dittobind_runtime", &runtime_module_init) == -1)
            throw std::runtime_error("Could not register the dittobind bootstrap module");
        runtime.bootstrap_registered = true;
    }
    for (; runtime.registered_modules < runtime.modules.size(); ++runtime.registered_modules) {
        const auto &entry = runtime.modules[runtime.registered_modules];
        if (PyImport_AppendInittab(entry.first.c_str(), entry.second) == -1)
            throw std::runtime_error("Could not register embedded module: " + entry.first);
    }
    runtime.owner = std::this_thread::get_id();
    runtime.status = phase::starting;
}
void set_phase(phase next) {
    auto &runtime = state();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    runtime.status = next;
}
void start_claimed(PyConfig *config, bool add_program_dir,
                   const std::vector<std::string> &search_paths = {}) {
    bool initialized = false;
    try {
        check_status(Py_InitializeFromConfig(config));
        initialized = true;
        PyObject *bootstrap = PyImport_ImportModule("_dittobind_runtime");
        if (!bootstrap) {
            PyErr_Print();
            throw std::runtime_error("Could not import the dittobind bootstrap module");
        }
        Py_DECREF(bootstrap);
        try {
            auto sys = nanobind::module_::import_("sys");
            nanobind::object path = sys.attr("path");
            if (add_program_dir) {
                nanobind::object directory = nanobind::str("");
                nanobind::list args = sys.attr("argv");
                auto os_path = nanobind::module_::import_("os.path");
                if (nanobind::len(args) && nanobind::cast<bool>(os_path.attr("exists")(args[0])))
                    directory = os_path.attr("abspath")(os_path.attr("dirname")(args[0]));
                path.attr("insert")(0, directory);
            }
            for (auto it = search_paths.rbegin(); it != search_paths.rend(); ++it)
                path.attr("insert")(0, *it);
        } catch (const nanobind::python_error &error) {
            throw std::runtime_error(error.what()); // Drop Python refs before finalizing.
        }
        set_phase(phase::running);
    } catch (...) {
        if (Py_IsInitialized()) {
            Py_FinalizeEx();
            clear_path_cache();
        }
        // A partially initialized CPython runtime has no general retry guarantee.
        set_phase(initialized ? phase::idle : phase::failed);
        throw;
    }
}
int parser_mode(eval_mode mode) {
    switch (mode) {
        case eval_expr: return Py_eval_input;
        case eval_single_statement: return Py_single_input;
        case eval_statements: return Py_file_input;
        default: throw std::runtime_error("invalid evaluation mode");
    }
}
struct namespaces {
    nanobind::object owner;
    nanobind::handle global, local;
    namespaces(nanobind::handle g, nanobind::handle l) : global(g), local(l) {
        if (!global.is_valid()) { owner = dittobind::globals(); global = owner; }
        if (!local.is_valid()) local = global;
    }
};
struct file_owner {
    FILE *file;
    ~file_owner() {
        if (file) {
#if PY_VERSION_HEX >= 0x030E0000
            Py_fclose(file);
#else
            std::fclose(file); // CPython and host use the shared C runtime on Windows.
#endif
        }
    }
};
} // namespace

void detail::reserve_subinterpreter() {
    auto &runtime = state();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (runtime.status != phase::running) throw std::logic_error("Main interpreter must be running");
    ++runtime.subinterpreters;
}
void detail::release_subinterpreter() noexcept {
    auto &runtime = state();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    --runtime.subinterpreters;
}

detail::embedded_module::embedded_module(const char *name, init_function init) {
    auto &runtime = state();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (runtime.status != phase::idle || Py_IsInitialized())
        throw std::logic_error("Embedded modules must be registered while Python is stopped");
    if (!name || !*name || !init || std::string(name) == "_dittobind_runtime")
        throw std::invalid_argument("Invalid or reserved embedded module registration");
    for (const auto &entry : runtime.modules)
        if (entry.first == name) throw std::invalid_argument("Duplicate embedded module registration");
    runtime.modules.emplace_back(name, init);
}
void initialize_interpreter(const interpreter_config &options) {
    validate_string(options.program_name);
    validate_string(options.python_home);
    validate_string(options.executable);
    for (const auto &arg : options.argv) validate_string(arg);
    for (const auto &path : options.search_paths) validate_string(path);
    if (options.argv.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("Too many Python arguments");
    PyConfig config;
    if (options.isolated) PyConfig_InitIsolatedConfig(&config);
    else PyConfig_InitPythonConfig(&config);
    config_owner cleanup{&config};
    config.parse_argv = 0;
    config.install_signal_handlers = options.install_signal_handlers;
    config.site_import = options.import_site;
    if (!options.program_name.empty())
        check_status(PyConfig_SetBytesString(&config, &config.program_name, options.program_name.c_str()));
    if (!options.python_home.empty())
        check_status(PyConfig_SetBytesString(&config, &config.home, options.python_home.c_str()));
    if (!options.executable.empty())
        check_status(PyConfig_SetBytesString(&config, &config.executable, options.executable.c_str()));
    std::vector<char *> argv;
    for (const auto &arg : options.argv) argv.push_back(const_cast<char *>(arg.c_str()));
    check_status(PyConfig_SetBytesArgv(&config, static_cast<int>(argv.size()), argv.data()));
    claim_startup();
    start_claimed(&config, options.add_program_dir_to_path, options.search_paths);
}
void initialize_interpreter(PyConfig *config, int argc, const char *const *argv, bool add_program_dir) {
    if (!config) throw std::invalid_argument("Null PyConfig");
    validate_argv(argc, argv);
    claim_startup();
    config_owner cleanup{config};
    try {
        check_status(PyConfig_SetBytesArgv(config, argc, const_cast<char **>(argv)));
    } catch (...) { set_phase(phase::idle); throw; }
    start_claimed(config, add_program_dir);
}
void initialize_interpreter(bool signals, int argc, const char *const *argv, bool add_program_dir) {
    validate_argv(argc, argv);
    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config_owner cleanup{&config};
    config.parse_argv = 0;
    config.install_signal_handlers = signals;
    check_status(PyConfig_SetBytesArgv(&config, argc, const_cast<char **>(argv)));
    claim_startup();
    start_claimed(&config, add_program_dir);
}
int finalize_interpreter() {
    auto &runtime = state();
    {
        std::lock_guard<std::mutex> lock(runtime.mutex);
#if PY_VERSION_HEX >= 0x030D0000
        auto *thread = PyThreadState_GetUnchecked();
#elif PY_VERSION_HEX >= 0x030C0000
        auto *thread = _PyThreadState_UncheckedGet();
#else
        auto *thread = Py_IsInitialized() && PyGILState_Check() ? PyThreadState_Get() : nullptr;
#endif
        if (runtime.status != phase::running || runtime.owner != std::this_thread::get_id() ||
            !Py_IsInitialized() || !thread ||
            PyThreadState_GetInterpreter(thread) != PyInterpreterState_Main())
            throw std::logic_error("Finalize requires the running interpreter's owner thread and GIL");
        if (runtime.subinterpreters) throw std::logic_error("Destroy all subinterpreters before finalizing main");
        runtime.status = phase::stopping;
    }
    const int result = Py_FinalizeEx();
    // Otherwise the next fresh PyConfig can inherit the previous venv's
    // prefix/home/executable (notably base_prefix on CPython 3.14).
    clear_path_cache();
    set_phase(phase::idle);
    return result;
}
scoped_interpreter::scoped_interpreter(const interpreter_config &config) { initialize_interpreter(config); }
scoped_interpreter::scoped_interpreter(bool signals, int argc, const char *const *argv, bool add_dir) {
    initialize_interpreter(signals, argc, argv, add_dir);
}
scoped_interpreter::scoped_interpreter(PyConfig *config, int argc, const char *const *argv, bool add_dir) {
    initialize_interpreter(config, argc, argv, add_dir);
}
scoped_interpreter::scoped_interpreter(scoped_interpreter &&other) noexcept
    : active_(std::exchange(other.active_, false)) {}
scoped_interpreter::~scoped_interpreter() noexcept {
    if (!active_) return;
    try {
        if (finalize_interpreter() != 0)
            std::fputs("dittobind: Python finalization reported an error\n", stderr);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "dittobind: %s\n", error.what());
        std::terminate();
    }
}
nanobind::dict main_globals() {
    return nanobind::borrow<nanobind::dict>(nanobind::module_::import_("__main__").attr("__dict__"));
}
nanobind::dict globals() {
    PyObject *current = PyEval_GetGlobals();
    return current ? nanobind::borrow<nanobind::dict>(current) : main_globals();
}
nanobind::object detail::evaluate(nanobind::str source, eval_mode mode,
                                 nanobind::handle global, nanobind::handle local) {
    namespaces ns(global, local);
    const int start = parser_mode(mode);
    std::string text = "# -*- coding: utf-8 -*-\n";
    text += source.c_str();
    PyObject *result = PyRun_StringFlags(text.c_str(), start, ns.global.ptr(), ns.local.ptr(), nullptr);
    if (!result) throw nanobind::python_error();
    return nanobind::steal(result);
}
nanobind::object detail::evaluate_file(nanobind::str filename, eval_mode mode,
                                      nanobind::handle global, nanobind::handle local) {
    namespaces ns(global, local);
    const int start = parser_mode(mode);
    const std::string path = nanobind::cast<std::string>(filename);
    validate_string(path);
#if PY_VERSION_HEX >= 0x030E0000
    file_owner stream{Py_fopen(filename.ptr(), "r")};
#else
    file_owner stream{_Py_fopen_obj(filename.ptr(), "r")};
#endif
    if (!stream.file) {
        PyErr_Clear();
        throw std::runtime_error("File \"" + path + "\" could not be opened!");
    }
    if (!PyDict_Check(ns.global.ptr())) {
        PyErr_SetString(PyExc_TypeError, "globals must be a dict");
        throw nanobind::python_error();
    }
    auto global_dict = nanobind::borrow<nanobind::dict>(ns.global);
    if (!global_dict.contains("__file__")) global_dict["__file__"] = filename;
    FILE *file = std::exchange(stream.file, nullptr); // PyRun_FileEx owns/closes it now.
    PyObject *result = PyRun_FileExFlags(file, path.c_str(), start, ns.global.ptr(), ns.local.ptr(), 1, nullptr);
    if (!result) throw nanobind::python_error();
    return nanobind::steal(result);
}
void exec_file(const std::string &filename, nanobind::handle global, nanobind::handle local) {
    (void) eval_file(filename, global, local);
}
} // namespace dittobind
