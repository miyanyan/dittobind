#ifdef DITTOBIND_REFERENCE
#include <pybind11/embed.h>
namespace binding = pybind11;
namespace execution = pybind11;
#define MODULE PYBIND11_EMBEDDED_MODULE
#else
#include <dittobind/buffer.h>
namespace binding = nanobind;
namespace execution = dittobind;
#define MODULE DITTOBIND_EMBEDDED_MODULE
#endif
#include <array>
#include <atomic>
#include <iostream>

struct Matrix {
    static std::atomic<int> alive;
    std::array<double, 6> data{0, 1, 2, 3, 4, 5};
    bool readonly, reverse;
    Matrix(bool readonly, bool reverse) : readonly(readonly), reverse(reverse) { ++alive; }
    ~Matrix() { --alive; }
    double get(int i) const { return data.at(i); }
};
std::atomic<int> Matrix::alive{0};
#ifndef DITTOBIND_REFERENCE
static dittobind::buffer_info describe(Matrix &matrix) {
    return {matrix.data.data() + (matrix.reverse ? 5 : 0), sizeof(double), "d", {2, 3},
        {matrix.reverse ? -24 : 24, matrix.reverse ? -8 : 8}, matrix.readonly};
}
#endif
MODULE(buffer_exports, m, execution::mod_gil_not_used()) {
#ifdef DITTOBIND_REFERENCE
    auto type = binding::class_<Matrix>(m, "Matrix", binding::buffer_protocol());
    type.def_buffer([](Matrix &matrix) {
        return binding::buffer_info(matrix.data.data() + (matrix.reverse ? 5 : 0), sizeof(double), "d", 2,
            {2, 3}, {matrix.reverse ? -24 : 24, matrix.reverse ? -8 : 8}, matrix.readonly);
    });
#else
    auto type = binding::class_<Matrix>(m, "Matrix", dittobind::buffer_protocol<Matrix, &describe>());
#endif
    type.def(binding::init<bool, bool>()).def("get", &Matrix::get);
    m.def("alive", [] { return Matrix::alive.load(); });
    m.def("layout", [](binding::handle obj, int mode) {
        const int flags[] = {PyBUF_SIMPLE, PyBUF_ND, PyBUF_STRIDES | PyBUF_FORMAT,
            PyBUF_F_CONTIGUOUS, PyBUF_C_CONTIGUOUS, PyBUF_ANY_CONTIGUOUS};
        Py_buffer buffer{};
        if (mode < 0 || mode >= 6) throw std::invalid_argument("mode");
        if (PyObject_GetBuffer(obj.ptr(), &buffer, flags[mode]) < 0) {
#ifdef DITTOBIND_REFERENCE
            throw binding::error_already_set();
#else
            throw binding::python_error();
#endif
        }
        auto result = binding::make_tuple(buffer.ndim, buffer.len, buffer.itemsize,
            buffer.shape != nullptr, buffer.strides != nullptr, buffer.format != nullptr);
        PyBuffer_Release(&buffer);
        return result;
    });
    m.def("writable", [](binding::handle obj) {
        Py_buffer buffer{};
        if (PyObject_GetBuffer(obj.ptr(), &buffer, PyBUF_WRITABLE | PyBUF_STRIDES) < 0) {
#ifdef DITTOBIND_REFERENCE
            throw binding::error_already_set();
#else
            throw binding::python_error();
#endif
        }
        PyBuffer_Release(&buffer);
    });
    m.def("contiguous", [](binding::handle obj) {
        Py_buffer buffer{};
        if (PyObject_GetBuffer(obj.ptr(), &buffer, PyBUF_SIMPLE) < 0) {
#ifdef DITTOBIND_REFERENCE
            throw binding::error_already_set();
#else
            throw binding::python_error();
#endif
        }
        PyBuffer_Release(&buffer);
    });
}
int main() {
    try {
        PyConfig config;
        PyConfig_InitIsolatedConfig(&config);
        auto status = PyConfig_SetBytesString(&config, &config.home, DITTOBIND_PYTHON_HOME);
        if (PyStatus_Exception(status)) throw std::runtime_error("Python home");
        execution::scoped_interpreter python(&config, 0, nullptr, false);
        try { execution::eval_file(DITTOBIND_FIXTURES "/buffers.py"); }
        catch (const std::exception &e) { throw std::runtime_error(e.what()); }
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
    return Matrix::alive == 0 ? 0 : 2;
}
