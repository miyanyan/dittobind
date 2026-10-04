#include <dittobind/subinterpreter.h>
#include <mutex>
#include <stdexcept>
#include <utility>

#if PY_VERSION_HEX >= 0x030C0000
namespace dittobind {
namespace {
PyThreadState *current_thread() noexcept {
#if PY_VERSION_HEX >= 0x030D0000
    return PyThreadState_GetUnchecked();
#else
    return _PyThreadState_UncheckedGet();
#endif
}
void bootstrap() {
    PyObject *module = PyImport_ImportModule("_dittobind_runtime");
    if (!module) {
        PyErr_Print();
        throw std::runtime_error("Could not initialize the subinterpreter backend");
    }
    Py_DECREF(module);
}
}

subinterpreter subinterpreter::create() {
    PyInterpreterConfig config{};
    config.allow_threads = 1;
    config.check_multi_interp_extensions = 1;
    config.gil = PyInterpreterConfig_OWN_GIL;
    return create(config);
}
subinterpreter subinterpreter::create(const PyInterpreterConfig &config) {
    // CPython 3.12 can return from these early failures without reacquiring
    // the caller's GIL. Reject deterministic errors before entering CPython.
    if (!config.use_main_obmalloc && !config.check_multi_interp_extensions)
        throw std::invalid_argument("A private allocator requires extension compatibility checks");
    if (config.gil != PyInterpreterConfig_DEFAULT_GIL &&
        config.gil != PyInterpreterConfig_SHARED_GIL && config.gil != PyInterpreterConfig_OWN_GIL)
        throw std::invalid_argument("Invalid subinterpreter GIL configuration");
    detail::reserve_subinterpreter();
    subinterpreter result;
    try {
        subinterpreter_scoped_activate main_scope(main());
        PyThreadState *previous = PyThreadState_Get();
        PyStatus status;
        {
            // Serialize CPython's interpreter startup (not user execution).
            static std::mutex creation_mutex;
            std::lock_guard<std::mutex> lock(creation_mutex);
            status = Py_NewInterpreterFromConfig(&result.creation_, &config);
        }
        if (PyStatus_Exception(status) || !result.creation_)
            throw std::runtime_error(status.err_msg ? status.err_msg : "Subinterpreter creation failed");
        result.interpreter_ = PyThreadState_GetInterpreter(result.creation_);
        result.owner_ = std::this_thread::get_id();
        try { bootstrap(); }
        catch (...) {
            auto *cleanup = nanobind::detail::prepare_interpreter_cleanup();
            Py_EndInterpreter(result.creation_);
            result.creation_ = nullptr;
            nanobind::detail::finish_interpreter_cleanup(cleanup);
            PyEval_RestoreThread(previous);
            throw;
        }
        PyEval_SaveThread();
        PyEval_RestoreThread(previous);
    } catch (...) {
        detail::release_subinterpreter();
        throw;
    }
    return result;
}

subinterpreter subinterpreter::main() {
    subinterpreter result;
    if (Py_IsInitialized()) result.interpreter_ = PyInterpreterState_Main();
    return result;
}
subinterpreter subinterpreter::current() {
    subinterpreter result;
    if (auto *thread = current_thread()) result.interpreter_ = PyThreadState_GetInterpreter(thread);
    return result;
}
int64_t subinterpreter::id() const noexcept {
    return interpreter_ ? PyInterpreterState_GetID(interpreter_) : -1;
}
nanobind::dict subinterpreter::state_dict() const {
    if (!interpreter_ || current().interpreter_ != interpreter_)
        throw std::logic_error("state_dict requires this interpreter to be active");
    return nanobind::borrow<nanobind::dict>(PyInterpreterState_GetDict(interpreter_));
}
subinterpreter::subinterpreter(subinterpreter &&other) noexcept {
    std::swap(interpreter_, other.interpreter_);
    std::swap(creation_, other.creation_);
    std::swap(owner_, other.owner_);
}
subinterpreter &subinterpreter::operator=(subinterpreter &&other) noexcept {
    std::swap(interpreter_, other.interpreter_);
    std::swap(creation_, other.creation_);
    std::swap(owner_, other.owner_);
    return *this;
}
subinterpreter::~subinterpreter() noexcept { destroy(); }
void subinterpreter::destroy() noexcept {
    if (!creation_) return;
    PyThreadState *previous = current_thread();
    if (previous && PyThreadState_GetInterpreter(previous) == interpreter_) std::terminate();
    if (previous) PyEval_SaveThread();
    if (owner_ == std::this_thread::get_id()) {
        PyEval_RestoreThread(creation_);
    } else {
        auto *replacement = PyThreadState_New(interpreter_);
        if (!replacement) std::terminate();
        PyEval_RestoreThread(replacement);
        PyThreadState_Clear(creation_);
        PyThreadState_Delete(creation_);
        creation_ = replacement;
    }
    auto *cleanup = nanobind::detail::prepare_interpreter_cleanup();
    Py_EndInterpreter(creation_);
    creation_ = nullptr;
    nanobind::detail::finish_interpreter_cleanup(cleanup);
    if (previous) PyEval_RestoreThread(previous);
    detail::release_subinterpreter();
}

subinterpreter_scoped_activate::subinterpreter_scoped_activate(const subinterpreter &target) {
    if (!target.interpreter_) throw std::invalid_argument("Cannot activate an empty interpreter");
    previous_ = current_thread();
    if (previous_ && PyThreadState_GetInterpreter(previous_) == target.interpreter_) {
        previous_ = nullptr;
        return;
    }
    active_ = PyThreadState_New(target.interpreter_);
    if (!active_) throw std::bad_alloc();
    if (previous_) PyEval_SaveThread();
    PyEval_RestoreThread(active_);
}
subinterpreter_scoped_activate::~subinterpreter_scoped_activate() noexcept {
    if (!active_) return;
    if (current_thread() != active_) std::terminate();
    PyThreadState_Clear(active_);
    PyThreadState_DeleteCurrent();
    if (previous_) PyEval_RestoreThread(previous_);
}
} // namespace dittobind
#endif
