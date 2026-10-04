#ifdef DITTOBIND_REFERENCE
#include <pybind11/embed.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
namespace binding = pybind11;
namespace execution = pybind11;
using policy = pybind11::return_value_policy;
#define MODULE PYBIND11_EMBEDDED_MODULE
#else
#include <dittobind/embed.h>
#include <nanobind/trampoline.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/variant.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/function.h>
namespace binding = nanobind;
namespace execution = dittobind;
using policy = nanobind::rv_policy;
#define MODULE DITTOBIND_EMBEDDED_MODULE
#endif
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

struct Animal {
    virtual ~Animal() = default;
    virtual std::string speak() const = 0;
};
struct PyAnimal : Animal {
#ifdef DITTOBIND_REFERENCE
    using Animal::Animal;
    std::string speak() const override { PYBIND11_OVERRIDE_PURE(std::string, Animal, speak); }
#else
    NB_TRAMPOLINE(Animal, 1);
    std::string speak() const override { NB_OVERRIDE_PURE(speak); }
#endif
};
struct Dog : Animal { std::string speak() const override { return "woof"; } };
struct Item {
    static std::atomic<int> alive;
    int value;
    explicit Item(int value) : value(value) { ++alive; }
    ~Item() { --alive; }
    int get() const { return value; }
};
std::atomic<int> Item::alive{0};
struct Parent {
    Item child{42};
    Item &get() { return child; }
};
struct Keeper {
    Item *item = nullptr;
    void set(Item *value) { item = value; }
    int get() const { return item->value; }
};
struct Unique {
    static std::atomic<int> alive;
    int value;
    explicit Unique(int v) : value(v) { ++alive; }
    ~Unique() { --alive; }
    int get() const { return value; }
};
std::atomic<int> Unique::alive{0};
struct DomainError : std::runtime_error { using std::runtime_error::runtime_error; };
static std::atomic<int> arrival{0};

MODULE(semantics, m, execution::mod_gil_not_used()) {
    m.def("parameters", [](int a, int b, int c) { return a + b * 10 + c * 100; },
#ifdef DITTOBIND_REFERENCE
          binding::arg("a"), binding::pos_only(),
#else
          binding::arg(),
#endif
          binding::arg("b") = 2,
          binding::kw_only(), binding::arg("c") = 3);
    m.def("overload", [](int) { return "integer"; });
    m.def("overload", [](const std::string &) { return "text"; });
    m.def("strict", [](int value) { return value; }, binding::arg("value").noconvert());
    m.def("containers", [](const std::map<std::string, std::vector<int>> &x) { return x; });
    m.def("optional", [](std::optional<int> x) { return x; }, binding::arg("x").none());
    m.def("variant", [](std::variant<int, std::string> x) { return x; });
    m.def("tuple", [](std::tuple<int, std::string> x) { return x; });
    m.def("args", [](binding::args args, binding::kwargs kwargs) {
        return binding::make_tuple(binding::len(args), binding::len(kwargs));
    });
    binding::class_<Animal, PyAnimal>(m, "Animal").def(binding::init<>()).def("speak", &Animal::speak);
    binding::class_<Dog, Animal>(m, "Dog").def(binding::init<>());
    m.def("speak", [](const Animal &animal) { return animal.speak(); });
    m.def("nullable", [](const Animal *animal) { return animal ? animal->speak() : "none"; }, binding::arg("animal").none());
    m.def("required", [](const Animal &animal) { return animal.speak(); },
#ifdef DITTOBIND_REFERENCE
          binding::arg("animal").none(false)
#else
          binding::arg("animal")
#endif
    );
    binding::class_<Item>(m, "Item").def(binding::init<int>()).def("get", &Item::get);
    binding::class_<Parent>(m, "Parent").def(binding::init<>()).def("get", &Parent::get, policy::reference_internal);
    binding::class_<Keeper>(m, "Keeper").def(binding::init<>()).def("set", &Keeper::set, binding::keep_alive<1, 2>()).def("get", &Keeper::get);
    m.def("alive", [] { return Item::alive.load(); });
#ifdef DITTOBIND_REFERENCE
    binding::class_<Unique, binding::smart_holder>(m, "Unique").def(binding::init<int>()).def("get", &Unique::get);
    m.def("consume", [](std::unique_ptr<Unique> x) { return x->value; });
    binding::register_exception<DomainError>(m, "DomainError", PyExc_ValueError);
#else
    binding::class_<Unique>(m, "Unique").def(binding::init<int>()).def("get", &Unique::get);
    m.def("consume", [](std::unique_ptr<Unique, binding::deleter<Unique>> x) { return x->value; });
    binding::exception<DomainError>(m, "DomainError", PyExc_ValueError);
#endif
    m.def("make_unique", [] { return std::make_unique<Unique>(42); });
    m.def("unique_alive", [] { return Unique::alive.load(); });
    m.def("fail", [](int kind) {
        if (kind == 0) throw DomainError("domain");
        if (kind == 1) throw std::invalid_argument("argument");
        if (kind == 2) throw std::out_of_range("range");
        throw std::bad_alloc();
    });
    m.def("meet", [] {
#ifdef Py_GIL_DISABLED
        execution::gil_scoped_release allow_collection;
#endif
        ++arrival;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (arrival != 2) {
            if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("GIL unexpectedly enabled");
            std::this_thread::yield();
        }
    });
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    try {
        PyConfig config;
        PyConfig_InitIsolatedConfig(&config);
        config.parse_argv = 0;
        auto status = PyConfig_SetBytesString(&config, &config.home, DITTOBIND_PYTHON_HOME);
        if (PyStatus_Exception(status)) throw std::runtime_error("Python home");
        execution::scoped_interpreter python(&config, 0, nullptr, false);
        try {
            execution::globals()["scenario"] = binding::str(argv[1]);
            execution::eval_file(DITTOBIND_FIXTURES "/bindings.py");
        } catch (const std::exception &e) { throw std::runtime_error(e.what()); }
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
    if (Item::alive != 0 || Unique::alive != 0) return 3;
    std::cout << "PASS " << argv[1] << '\n';
}
