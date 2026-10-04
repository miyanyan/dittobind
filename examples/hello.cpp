#include <dittobind/embed.h>
#include <nanobind/stl/string.h>
#include <iostream>

namespace nb = nanobind;

struct Counter {
    int value = 0;
    void add(int amount) { value += amount; }
};

DITTOBIND_EMBEDDED_MODULE(host, m) {
    m.def("greet", [](const std::string &name) { return "Hello, " + name; });
    nb::class_<Counter>(m, "Counter")
        .def(nb::init<>())
        .def("add", &Counter::add)
        .def_rw("value", &Counter::value);
}

int main() {
    try {
        dittobind::interpreter_config config;
        config.program_name = DITTOBIND_PYTHON_EXECUTABLE;
        config.python_home = DITTOBIND_PYTHON_HOME;
        dittobind::scoped_interpreter python(config);
        // Catch Python errors INSIDE the interpreter's lifetime.
        try {
            auto globals = dittobind::main_globals();
            dittobind::exec(R"(
import host
counter = host.Counter()
counter.add(40)
def answer(x):
    return counter.value + x
print(host.greet("embedded Python"), flush=True)
)", globals);
            std::cout << "Python answer: " << nb::cast<int>(globals["answer"](2)) << '\n';
        } catch (const nb::python_error &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
