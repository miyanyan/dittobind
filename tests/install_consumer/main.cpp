#include <dittobind/embed.h>
DITTOBIND_EMBEDDED_MODULE(installed_host, m) {
    m.def("answer", [] { return 42; });
}
int main() {
    dittobind::interpreter_config config;
    config.python_home = PYTHON_HOME;
    config.isolated = true;
    dittobind::scoped_interpreter python(config);
    try {
        dittobind::exec("import installed_host\nassert installed_host.answer() == 42");
    } catch (const nanobind::python_error &) { return 1; }
}
