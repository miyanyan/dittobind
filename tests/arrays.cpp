#ifdef DITTOBIND_REFERENCE
#include <pybind11/embed.h>
#include <pybind11/numpy.h>
namespace binding = pybind11;
namespace execution = pybind11;
#define MODULE PYBIND11_EMBEDDED_MODULE
#else
#include <dittobind/embed.h>
#include <nanobind/ndarray.h>
namespace binding = nanobind;
namespace execution = dittobind;
#define MODULE DITTOBIND_EMBEDDED_MODULE
#endif
#include <atomic>
#include <iostream>
#include <stdexcept>

static std::atomic<int> alive{0};
MODULE(arrays, m, execution::mod_gil_not_used()) {
    m.def("alive", [] { return alive.load(); });
#ifdef DITTOBIND_REFERENCE
    using Array = binding::array_t<double, 0>;
    m.def("sum", [](Array value) {
        auto data = value.unchecked<2>();
        double total = 0;
        for (binding::ssize_t i = 0; i < data.shape(0); ++i)
            for (binding::ssize_t j = 0; j < data.shape(1); ++j) total += data(i, j);
        return total;
    }, binding::arg("value").noconvert());
    m.def("scale", [](Array value, double scale) {
        auto data = value.mutable_unchecked<2>();
        for (binding::ssize_t i = 0; i < data.shape(0); ++i)
            for (binding::ssize_t j = 0; j < data.shape(1); ++j) data(i, j) *= scale;
    }, binding::arg("value").noconvert(), binding::arg("scale"));
    m.def("make", [] {
        auto *data = new double[6]{0, 1, 2, 3, 4, 5};
        ++alive;
        binding::capsule owner(data, [](void *ptr) { delete[] static_cast<double *>(ptr); --alive; });
        return binding::array_t<double>({2, 3}, {3 * sizeof(double), sizeof(double)}, data, owner);
    });
#else
    using ReadArray = binding::ndarray<binding::numpy, const double, binding::ndim<2>>;
    using WriteArray = binding::ndarray<binding::numpy, double, binding::ndim<2>>;
    m.def("sum", [](ReadArray data) {
        double total = 0;
        for (size_t i = 0; i < data.shape(0); ++i)
            for (size_t j = 0; j < data.shape(1); ++j) total += data(i, j);
        return total;
    }, binding::arg("value").noconvert());
    m.def("scale", [](WriteArray data, double scale) {
        for (size_t i = 0; i < data.shape(0); ++i)
            for (size_t j = 0; j < data.shape(1); ++j) data(i, j) *= scale;
    }, binding::arg("value").noconvert(), binding::arg("scale"));
    m.def("make", [] {
        auto *data = new double[6]{0, 1, 2, 3, 4, 5};
        ++alive;
        binding::capsule owner(data, [](void *ptr) noexcept { delete[] static_cast<double *>(ptr); --alive; });
        return WriteArray(data, {2, 3}, owner);
    });
#endif
}
int main() {
    try {
        PyConfig config;
        PyConfig_InitIsolatedConfig(&config);
        auto status = PyConfig_SetBytesString(&config, &config.home, DITTOBIND_PYTHON_HOME);
        if (PyStatus_Exception(status)) throw std::runtime_error("Python home");
        execution::scoped_interpreter python(&config, 0, nullptr, false);
        try {
            execution::globals()["packages"] = binding::str(DITTOBIND_SITE_PACKAGES);
            execution::exec("import sys\nsys.path.insert(0, packages)");
            execution::eval_file(DITTOBIND_FIXTURES "/arrays.py");
        } catch (const std::exception &e) { throw std::runtime_error(e.what()); }
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
    return alive == 0 ? 0 : 2;
}
