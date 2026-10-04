"""Create a version-checked nanobind source overlay; never edit site-packages."""
from pathlib import Path
import hashlib
import shutil
import sys
import nanobind

EXPECTED = {
    'src/nb_func.cpp': '24750ec0352e28f366beb3d5c79ce4c37ed38978891a4877797575d4b4db4929',
    'src/nb_type.cpp': '8d261f534c2282bea3d9599005d5d579f6fde3219ada78341aa8bdf3b95e05c3',
    'include/nanobind/nb_misc.h': 'e5a7f4be98711eb597bbd6eafda08f49b7398ac44101df6c522b8e3b6f5232e4',
    'include/nanobind/nb_backend.h': 'db58adab983b7aff8bdaf512890b7fe86d3a78959bec61b62f5467f2e82bb34a',
    'include/nanobind/nb_defs.h': '84ce067aff79e300c406a01c8e3c41b3b161b9677aef85b9d642a6d67d9d82c8',
    'include/nanobind/nb_types.h': '96cc8e6b2d0f4fb0896bb8ff9c86aa8b58fd115d7556ea98b85e2f4a0bdca33b',
    'src/nb_internals.cpp': '8b6b675e15d6dcb0c0a16f5f4d0fd8852effa7b3e80c56d1449cceaf1295c5b2',
    'src/nb_internals.h': 'd08c0679178e24ca472c65eb6a6978b070a7ba388409a9cf73b53883890dc420',
}
source = Path(nanobind.__file__).parent.resolve()
destination = Path(sys.argv[1]).resolve()
if source == destination or source in destination.parents:
    raise SystemExit('Overlay must be outside the installed package')
for name, digest in EXPECTED.items():
    # Normalize distribution line endings, then verify the pinned source.
    raw = (source / name).read_bytes().replace(b'\r\n', b'\n')
    if hashlib.sha256(raw).hexdigest() != digest:
        raise SystemExit(f'Unexpected nanobind 3.1.0 source: {name}')
root = Path(__file__).resolve().parent.parent
stamp = hashlib.sha256(Path(__file__).read_bytes() + (root / 'backend/interpreter_hooks.inc').read_bytes()).hexdigest()
stamp_path = destination / '.dittobind-overlay'
if stamp_path.is_file() and stamp_path.read_text() == stamp:
    print(destination / 'cmake')
    raise SystemExit(0)
shutil.copytree(source, destination, dirs_exist_ok=True,
                ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))

def edit(name, replacements):
    path = destination / name
    text = path.read_text(encoding='utf-8')
    for old, new in replacements:
        if text.count(old) != 1:
            raise SystemExit(f'Patch anchor changed: {name}: {old[:70]}')
        text = text.replace(old, new)
    path.write_text(text, encoding='utf-8', newline='\n')

edit('include/nanobind/nb_backend.h', [
    ('struct nb_internals;\n', '''struct nb_internals;
NB_CORE nb_internals *current_internals(const char *domain) noexcept;
NB_CORE void release_interpreter(int64_t id) noexcept;
NB_CORE size_t interpreter_record_count() noexcept;
NB_CORE void *prepare_interpreter_cleanup();
NB_CORE void finish_interpreter_cleanup(void *token) noexcept;
NB_CORE PyThreadState *remember_released_tstate(PyThreadState *state) noexcept;
'''),
    ('inline nb_internals *internals = nullptr;', 'inline thread_local nb_internals *internals = nullptr;'),
    ('#  define NB_CTX ::nanobind::detail::internals', '#  define NB_CTX ::nanobind::detail::current_internals(NB_DOMAIN_STR)'),
    ('((void) (cleanup), ::nanobind::detail::internals)', '((void) (cleanup), NB_CTX)'),
])
edit('include/nanobind/nb_types.h', [
    ('''        PyObject *v = load();
        if (NB_UNLIKELY(!v))
            v = raise_if_null(NB_CALL(import_cached)(NB_CTX, this));
        return v;''', '''        return raise_if_null(NB_CALL(import_cached)(NB_CTX, this));'''),
])
edit('src/nb_type.cpp', [
    ('''    constexpr const char *errors[4] = {''', '''    // A moved-out object is an ordinary API error, not a warning.
    if (state == nb_inst_state::state_relinquished) return false;
    constexpr const char *errors[4] = {'''),
])
edit('src/nb_func.cpp', [
    ('''    if (f->flags & (uint32_t) func_flags::is_operator)
        return not_implemented().release().ptr();''', '''    if ((f->flags & (uint32_t) func_flags::is_method) && nargs_in &&
        nb_type_check(nb_func_internals(self), (PyObject *) Py_TYPE(args_in[0])) &&
        ((nb_inst *) args_in[0])->state.state == nb_inst_state::state_relinquished) {
        PyErr_SetString(PyExc_ValueError, "The Python instance no longer owns its C++ object");
        return nullptr;
    }
    if (f->flags & (uint32_t) func_flags::is_operator)
        return not_implemented().release().ptr();'''),
])
edit('src/nb_internals.h', [
    ('#include <vector>', '#include <vector>\n#include <unordered_map>\n#include <atomic>'),
    ('#  define NB_INTERNALS_VERSION 22', '#  define NB_INTERNALS_VERSION 2202'),
    ('bool *is_alive_ptr = nullptr;', 'std::atomic<bool> *is_alive_ptr = nullptr;\n    int64_t interpreter_id = -1;'),
    ('std::vector<import_cache *> import_slots;', 'std::vector<import_cache *> import_slots;\n    std::unordered_map<import_cache *, PyObject *> import_objects;'),
])
hooks = (root / 'backend/interpreter_hooks.inc').read_text(encoding='utf-8')
edit('src/nb_internals.cpp', [
    ('#include <thread>', '#include <thread>\n#include <mutex>\n#include <memory>'),
    ('''    nb_meta_slots[0].pfunc = (PyObject *) &PyType_Type;
    PyTypeObject *nb_meta = new_type(p, &nb_meta_spec);''', '''    PyType_Slot local_meta_slots[] = {{Py_tp_base, &PyType_Type}, {0, nullptr}};
    PyType_Spec local_meta_spec = nb_meta_spec;
    local_meta_spec.slots = local_meta_slots;
    PyTypeObject *nb_meta = new_type(p, &local_meta_spec);'''),
    ('static std::vector<nb_internals *> internals_created;', 'static std::vector<nb_internals *> internals_created;\nstatic std::mutex interpreter_registry_mutex;\nstatic bool cleanup_registered = false;'),
    ('PyObject *import_cached(nb_internals *p, import_cache *c) noexcept {', '''PyObject *import_cached(nb_internals *p, import_cache *c) noexcept {
    {
        lock_internals guard(p);
        auto found = p->import_objects.find(c);
        if (found != p->import_objects.end()) return found->second;
    }'''),
    ('if (PyObject *cur = c->load()) { // lost the race', 'if (auto found = p->import_objects.find(c); found != p->import_objects.end()) { // lost the race'),
    ('        return cur;\n', '        return found->second;\n'),
    ('    p->import_slots.push_back(c);\n    c->store(o);', '    p->import_objects.emplace(c, o);'),
    ('    p->import_slots.clear();', '    p->import_slots.clear();\n    p->import_objects.clear();'),
    ('static bool is_alive_value = false;\nstatic bool *is_alive_ptr = &is_alive_value;', 'static std::atomic<bool> is_alive_value{false};\nstatic thread_local std::atomic<bool> *is_alive_ptr = &is_alive_value;'),
    ('    nb_internals *p = new nb_internals();', '    nb_internals *p = new nb_internals();\n    p->interpreter_id = PyInterpreterState_GetID(PyInterpreterState_Get());'),
    ('    // Track the published record for the exit-time sweep\n    {', '    // Serialize registry updates across independent interpreter GILs.\n    {\n        std::lock_guard<std::mutex> registry_lock(interpreter_registry_mutex);'),
    ('static void internals_cleanup() {', hooks + '\nstatic void internals_cleanup() {'),
    ('    internals_created.clear();', '    internals_created.clear();\n    cleanup_registered = false;'),
    ('static nb_internals *nb_module_init_impl(const char *domain, PyObject *m) {',
     'static nb_internals *nb_module_init_impl(const char *domain, PyObject *m) {\n    register_interpreter_cleanup();'),
    ('bool need_atexit = internals_created.empty();', 'bool need_atexit = !cleanup_registered;'),
    ('if (need_atexit && Py_AtExit(internals_cleanup))',
     'if (need_atexit && !(cleanup_registered = (Py_AtExit(internals_cleanup) == 0)))'),
    ('void *tstate_ensure() noexcept { return attach_tstate(); }', '''static thread_local PyThreadState *released_tstate = nullptr;
PyThreadState *remember_released_tstate(PyThreadState *state) noexcept {
    auto *previous = released_tstate;
    released_tstate = state;
    return previous;
}
void *tstate_ensure() noexcept {
#if PY_VERSION_HEX >= 0x030D0000
    if (PyThreadState_GetUnchecked()) return (void *) 1;
#elif PY_VERSION_HEX >= 0x030C0000
    if (_PyThreadState_UncheckedGet()) return (void *) 1;
#else
    // Before 3.12 the unchecked current state is process-global, so another
    // worker holding the GIL must not make this thread appear attached.
    if (PyGILState_Check()) return (void *) 1;
#endif
    if (released_tstate) {
        PyEval_RestoreThread(released_tstate);
        return (void *) 2;
    }
    return (void *) (uintptr_t) (4 + (int) PyGILState_Ensure());
}'''),
    ('''    if (token)
        detach_tstate(token);''', '''    if (token == (void *) 2) PyEval_SaveThread();
    else if ((uintptr_t) token >= 4)
        PyGILState_Release((PyGILState_STATE) ((uintptr_t) token - 4));'''),
])
edit('include/nanobind/nb_misc.h', [
    ('gil_scoped_release() noexcept : state(PyEval_SaveThread()) { }',
     'gil_scoped_release() noexcept : state(PyEval_SaveThread()), previous(detail::remember_released_tstate(state)) { }'),
    ('~gil_scoped_release() { PyEval_RestoreThread(state); }',
     '~gil_scoped_release() { detail::remember_released_tstate(previous); PyEval_RestoreThread(state); }'),
    ('PyThreadState *state;', 'PyThreadState *state;\n    PyThreadState *previous;'),
])
# Module definitions are process-lived immutable objects once constructed.
# Serializing their first construction avoids races between independent GILs.
edit('include/nanobind/nb_defs.h', [
    ('#pragma once', '#pragma once\n#include <mutex>'),
    ('#define NB_MODULE_IMPL2(name, variable) ', '''#define NB_MODULE_CONFIG_DEFAULT(value) (value)
#define NB_MODULE_IMPL2(name, variable) NB_MODULE_IMPL2_CONFIG(name, variable, NB_MODULE_CONFIG_DEFAULT)
#define NB_MODULE_IMPL2_CONFIG(name, variable, configure) '''),
    ('nanobind_##name##_def = NB_CALL(module_new)(', 'nanobind_##name##_def = configure(NB_CALL(module_new)('),
    ('NB_ABI_MINOR_TAG); ', 'NB_ABI_MINOR_TAG)); '),
    ('        if (!nanobind_##name##_def)', '''        static std::mutex definition_mutex;                                  \\
        std::lock_guard<std::mutex> definition_lock(definition_mutex);        \\
        if (!nanobind_##name##_def)'''),
])
stamp_path.write_text(stamp)
print(destination / 'cmake')
