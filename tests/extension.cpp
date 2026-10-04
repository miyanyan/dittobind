#ifdef DITTOBIND_REFERENCE
#include <pybind11/pybind11.h>
namespace binding = pybind11;
#define TEST_MODULE(m) PYBIND11_MODULE(pybind11_ext, m, binding::multiple_interpreters::per_interpreter_gil(), binding::mod_gil_not_used())
#else
#include <dittobind/embed.h>
namespace binding = nanobind;
#define TEST_MODULE(m) DITTOBIND_MODULE(dittobind_ext, m, dittobind::multiple_interpreters::per_interpreter_gil(), dittobind::mod_gil_not_used())
#endif

struct Value {
    explicit Value(int value) : number(value) {}
    int number;
    int twice() const { return number * 2; }
    int add(int amount) const { return number + amount; }
};

TEST_MODULE(m) {
    m.def("add", [](int a, int b) { return a + b; });
    binding::class_<Value>(m, "Value").def(binding::init<int>())
        .def("twice", &Value::twice).def("add", &Value::add);
}
