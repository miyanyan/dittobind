#pragma once

#include <nanobind/nanobind.h>
#include <nanobind/eval.h>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(PYPY_VERSION) || defined(Py_LIMITED_API)
#error "dittobind requires the full CPython API"
#endif

namespace dittobind {

struct multiple_interpreters {
    enum class level { unsupported, shared, per_interpreter };
    level value;
    static constexpr multiple_interpreters not_supported() { return {level::unsupported}; }
    static constexpr multiple_interpreters shared_gil() { return {level::shared}; }
    static constexpr multiple_interpreters per_interpreter_gil() { return {level::per_interpreter}; }
};
struct mod_gil_not_used {
    bool value;
    explicit constexpr mod_gil_not_used(bool enabled = true) : value(enabled) {}
};

struct interpreter_config {
    // program_name/home/executable/argv use PyConfig_SetBytes* decoding rules.
    // python_home locates the standard library independently of the host exe;
    // program_name alone is insufficient on Windows. search_paths use UTF-8.
    std::string program_name;
    std::string python_home;
    std::vector<std::string> argv;
    std::vector<std::string> search_paths;
    bool isolated = false;
    bool import_site = true;
    bool install_signal_handlers = true;
    bool add_program_dir_to_path = true;
    // Explicit sys.executable/path-discovery input, e.g. a venv's Python binary.
    // Leave python_home empty when discovering pyvenv.cfg from this path.
    std::string executable;
};

void initialize_interpreter(const interpreter_config &config);
void initialize_interpreter(bool init_signal_handlers = true, int argc = 0,
                            const char *const *argv = nullptr, bool add_program_dir_to_path = true);
// Consumes/clears config after accepting startup; argc/argv replace config.argv.
// A rejected duplicate startup leaves the caller's config intact.
void initialize_interpreter(PyConfig *config, int argc = 0,
                            const char *const *argv = nullptr, bool add_program_dir_to_path = true);
int finalize_interpreter(); // CPython shutdown status; Python references must be gone.

// One live owning instance per process, created/destroyed on the same thread.
// All Python references (including python_error) must die before this guard.
// Join workers and reacquire the GIL before destroying it.
class scoped_interpreter final {
public:
    explicit scoped_interpreter(const interpreter_config &config);
    explicit scoped_interpreter(bool init_signal_handlers = true, int argc = 0,
                                const char *const *argv = nullptr, bool add_program_dir_to_path = true);
    explicit scoped_interpreter(PyConfig *config, int argc = 0,
                                const char *const *argv = nullptr, bool add_program_dir_to_path = true);
    ~scoped_interpreter() noexcept;
    scoped_interpreter(const scoped_interpreter &) = delete;
    scoped_interpreter &operator=(const scoped_interpreter &) = delete;
    scoped_interpreter(scoped_interpreter &&other) noexcept;
    scoped_interpreter &operator=(scoped_interpreter &&) = delete;

private:
    bool active_ = true;
};

using nanobind::gil_scoped_acquire;
using nanobind::gil_scoped_release;

// Same globals dictionary used by __main__, unlike an isolated script namespace.
nanobind::dict main_globals();
nanobind::dict globals(); // Python caller's globals, otherwise __main__.
using nanobind::eval_mode;
using nanobind::eval_expr;
using nanobind::eval_single_statement;
using nanobind::eval_statements;

namespace detail {
nanobind::object evaluate(nanobind::str source, eval_mode mode,
                          nanobind::handle global, nanobind::handle local);
nanobind::object evaluate_file(nanobind::str filename, eval_mode mode,
                               nanobind::handle global, nanobind::handle local);
}

template <nanobind::eval_mode mode = nanobind::eval_expr, typename Source>
nanobind::object eval(const Source &source, nanobind::handle globals = {},
                      nanobind::handle locals = {}) {
    if constexpr (std::is_same_v<std::decay_t<Source>, std::string>)
        return detail::evaluate(nanobind::str(source.data(), source.size()), mode, globals, locals);
    else {
        nanobind::str text(source);
        if constexpr (std::is_array_v<Source>) {
            if (source[0] == '\n')
                text = nanobind::str(nanobind::module_::import_("textwrap").attr("dedent")(text));
        }
        return detail::evaluate(text, mode, globals, locals);
    }
}

template <typename Source>
void exec(const Source &source, nanobind::handle globals = {}, nanobind::handle locals = {}) {
    (void) dittobind::eval<nanobind::eval_statements>(source, globals, locals);
}

template <eval_mode mode = eval_statements, typename Filename>
nanobind::object eval_file(const Filename &filename, nanobind::handle global = {},
                           nanobind::handle local = {}) {
    if constexpr (std::is_same_v<std::decay_t<Filename>, std::string>)
        return detail::evaluate_file(nanobind::str(filename.data(), filename.size()), mode, global, local);
    else
        return detail::evaluate_file(nanobind::str(filename), mode, global, local);
}

// Convenience alias for statement-mode eval_file. Preserve existing __file__.
void exec_file(const std::string &filename, nanobind::handle global = {}, nanobind::handle local = {});

namespace detail {
struct module_options {
    multiple_interpreters::level interpreters = multiple_interpreters::level::unsupported;
    bool gil_not_used = false;
    template <typename... Tags> explicit module_options(Tags... tags) { (set(tags), ...); }
    void set(multiple_interpreters tag) { interpreters = tag.value; }
    void set(mod_gil_not_used tag) { gil_not_used = tag.value; }
};
inline PyObject *configure_module(PyObject *definition, module_options options) {
    if (!definition) return nullptr;
    auto *module = reinterpret_cast<PyModuleDef *>(definition);
    size_t end = 0;
    for (; module->m_slots[end].slot; ++end) {
#if PY_VERSION_HEX >= 0x030C0000
        if (module->m_slots[end].slot == Py_mod_multiple_interpreters) {
            switch (options.interpreters) {
                case multiple_interpreters::level::unsupported:
                    module->m_slots[end].value = Py_MOD_MULTIPLE_INTERPRETERS_NOT_SUPPORTED; break;
                case multiple_interpreters::level::shared:
                    module->m_slots[end].value = Py_MOD_MULTIPLE_INTERPRETERS_SUPPORTED; break;
                case multiple_interpreters::level::per_interpreter:
                    module->m_slots[end].value = Py_MOD_PER_INTERPRETER_GIL_SUPPORTED; break;
            }
        }
#endif
    }
#if PY_VERSION_HEX >= 0x030D0000
    bool found = false;
    for (size_t i = 0; i < end; ++i) {
        if (module->m_slots[i].slot == Py_mod_gil) {
            module->m_slots[i].value = options.gil_not_used ? Py_MOD_GIL_NOT_USED : Py_MOD_GIL_USED;
            found = true;
        }
    }
    if (!found) module->m_slots[end] = {Py_mod_gil, options.gil_not_used ? Py_MOD_GIL_NOT_USED : Py_MOD_GIL_USED};
#endif
    return definition;
}
void reserve_subinterpreter();
void release_subinterpreter() noexcept;
class embedded_module final {
public:
    using init_function = PyObject *(*)();
    embedded_module(const char *name, init_function init);
};
} // namespace detail
} // namespace dittobind

// External extension modules use the same interpreter/GIL capability tags.
#define DITTOBIND_MODULE(name, variable, ...)                                 \
    static PyObject *dittobind_configure_##name(PyObject *definition) {       \
        return ::dittobind::detail::configure_module(definition,              \
            ::dittobind::detail::module_options{__VA_ARGS__});                 \
    }                                                                       \
    NB_MODULE_IMPL2_CONFIG(name, variable, dittobind_configure_##name)

// Define in a .cpp linked directly into the executable (or an OBJECT library).
#define DITTOBIND_EMBEDDED_MODULE(name, variable, ...)                         \
    extern "C" NB_EXPORT PyObject *PyInit_##name();                           \
    static const ::dittobind::detail::embedded_module                         \
        dittobind_module_##name(#name, &PyInit_##name);                        \
    DITTOBIND_MODULE(name, variable, __VA_ARGS__)
