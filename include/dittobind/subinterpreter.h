#pragma once
#include <dittobind/embed.h>

#if PY_VERSION_HEX >= 0x030C0000
namespace dittobind {

class subinterpreter final {
public:
    subinterpreter() = default;
    static subinterpreter create();
    static subinterpreter create(const PyInterpreterConfig &config);
    static subinterpreter main();
    static subinterpreter current();
    ~subinterpreter() noexcept;
    subinterpreter(const subinterpreter &) = delete;
    subinterpreter &operator=(const subinterpreter &) = delete;
    subinterpreter(subinterpreter &&other) noexcept;
    subinterpreter &operator=(subinterpreter &&other) noexcept;
    bool empty() const noexcept { return interpreter_ == nullptr; }
    explicit operator bool() const noexcept { return !empty(); }
    int64_t id() const noexcept;
    nanobind::dict state_dict() const;
private:
    friend class subinterpreter_scoped_activate;
    PyInterpreterState *interpreter_ = nullptr;
    PyThreadState *creation_ = nullptr;
    std::thread::id owner_;
    void destroy() noexcept;
};

class subinterpreter_scoped_activate final {
public:
    explicit subinterpreter_scoped_activate(const subinterpreter &interpreter);
    ~subinterpreter_scoped_activate() noexcept;
    subinterpreter_scoped_activate(const subinterpreter_scoped_activate &) = delete;
    subinterpreter_scoped_activate &operator=(const subinterpreter_scoped_activate &) = delete;
private:
    PyThreadState *previous_ = nullptr;
    PyThreadState *active_ = nullptr;
};

class scoped_subinterpreter final {
public:
    scoped_subinterpreter() : interpreter_(subinterpreter::create()), activation_(interpreter_) {}
    explicit scoped_subinterpreter(const PyInterpreterConfig &config)
        : interpreter_(subinterpreter::create(config)), activation_(interpreter_) {}
private:
    subinterpreter interpreter_;
    subinterpreter_scoped_activate activation_;
};
} // namespace dittobind
#endif
