// dittobind adaptation: current interpreter lookup and post-destruction sweep.
size_t interpreter_record_count() noexcept {
    std::lock_guard<std::mutex> lock(interpreter_registry_mutex);
    return internals_created.size();
}

nb_internals *current_internals(const char *domain) noexcept {
    PyObject *dict = PyInterpreterState_GetDict(PyInterpreterState_Get());
    if (!dict) return nullptr;
    PyObject *key = PyUnicode_FromFormat("__nb_internals_%s_%s__", NB_INTERNALS_KEY, domain);
    if (!key) return nullptr;
    PyObject *capsule = PyDict_GetItemWithError(dict, key);
    Py_DECREF(key);
    if (!capsule) return nullptr;
    return (nb_internals *) PyCapsule_GetPointer(capsule, "nb_internals");
}

void release_interpreter(int64_t id) noexcept {
    // Do not allocate in the post-interpreter cleanup path.
    for (;;) {
        nb_internals *retired = nullptr;
        {
            std::lock_guard<std::mutex> lock(interpreter_registry_mutex);
            for (auto it = internals_created.begin(); it != internals_created.end(); ++it) {
                if ((*it)->interpreter_id == id) {
                    retired = *it;
                    internals_created.erase(it);
                    break;
                }
            }
        }
        if (!retired) break;
        internals_cleanup_one(retired);
    }
}

// All adapted backend DLLs of this ABI share a registry in the interpreter
// dictionary. Keep native ownership across Py_EndInterpreter: module and type
// destructors must finish before the backend's leak check/free, and an atexit
// handler may still import another backend after teardown has begun.
struct interpreter_cleanup_registry {
    std::mutex mutex;
    int64_t id = PyInterpreterState_GetID(PyInterpreterState_Get());
    std::vector<void (*)(int64_t) noexcept> callbacks;
};
using cleanup_registry_ptr = std::shared_ptr<interpreter_cleanup_registry>;
static constexpr const char *cleanup_registry_key = "__dittobind_cleanup_" NB_INTERNALS_KEY "__";
static constexpr const char *cleanup_capsule_name = "dittobind.cleanup.2202";

static void cleanup_registry_free(PyObject *capsule) {
    delete (cleanup_registry_ptr *) PyCapsule_GetPointer(capsule, cleanup_capsule_name);
}

static cleanup_registry_ptr get_cleanup_registry() {
    PyObject *dict = PyInterpreterState_GetDict(PyInterpreterState_Get());
    object key = steal(PyUnicode_FromString(cleanup_registry_key));
    if (!dict || !key.is_valid()) raise_python_error();
    object capsule = steal(dict_getitem_or_default(dict, key.ptr(), nullptr));
    if (!capsule.is_valid()) {
        if (PyErr_Occurred()) raise_python_error();
        auto holder = std::make_unique<cleanup_registry_ptr>(
            std::make_shared<interpreter_cleanup_registry>());
        capsule = steal(PyCapsule_New(holder.get(), cleanup_capsule_name, cleanup_registry_free));
        if (!capsule.is_valid()) raise_python_error();
        holder.release(); // capsule owns the shared_ptr wrapper
        PyObject *found = nullptr;
#if PY_VERSION_HEX >= 0x030D0000
        if (PyDict_SetDefaultRef(dict, key.ptr(), capsule.ptr(), &found) < 0)
            raise_python_error();
#else
        found = PyDict_SetDefault(dict, key.ptr(), capsule.ptr());
        if (!found) raise_python_error();
        Py_INCREF(found);
#endif
        capsule = steal(found);
    }
    auto *holder = (cleanup_registry_ptr *) PyCapsule_GetPointer(capsule.ptr(), cleanup_capsule_name);
    if (!holder) raise_python_error();
    return *holder;
}

static void register_interpreter_cleanup() {
    auto registry = get_cleanup_registry();
    std::lock_guard<std::mutex> lock(registry->mutex);
    for (auto callback : registry->callbacks)
        if (callback == &release_interpreter) return;
    registry->callbacks.push_back(&release_interpreter);
}

void *prepare_interpreter_cleanup() {
    return new cleanup_registry_ptr(get_cleanup_registry());
}

void finish_interpreter_cleanup(void *token) noexcept {
    std::unique_ptr<cleanup_registry_ptr> owner((cleanup_registry_ptr *) token);
    // No attached thread state or Python API calls beyond this point.
    for (auto callback : (*owner)->callbacks) callback((*owner)->id);
}
