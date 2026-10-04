#pragma once
#include <dittobind/embed.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace dittobind {

// Metadata is copied per exported view. The exporting C++ object owns data
// and must keep its address and allocation valid until every view is released.
struct buffer_info {
    void *data;
    Py_ssize_t itemsize;
    std::string format;
    std::vector<Py_ssize_t> shape;
    std::vector<Py_ssize_t> strides; // bytes, including negative strides
    bool readonly = false;
};

namespace detail {
template <typename T, buffer_info (*Describe)(T &)>
struct buffer_export {
    static int get(PyObject *exporter, Py_buffer *view, int flags) noexcept {
        if (!view) {
            PyErr_SetString(PyExc_BufferError, "A buffer view is required");
            return -1;
        }
        view->obj = nullptr;
        try {
            auto info = std::make_unique<buffer_info>(Describe(nanobind::cast<T &>(nanobind::handle(exporter))));
            if (info->itemsize <= 0 || info->shape.size() != info->strides.size() ||
                info->shape.size() > PyBUF_MAX_NDIM || info->format.empty() ||
                info->format.find('\0') != std::string::npos)
                throw std::invalid_argument("Invalid buffer metadata");
            Py_ssize_t length = info->itemsize;
            for (auto dimension : info->shape) {
                if (dimension < 0 || (dimension && length > std::numeric_limits<Py_ssize_t>::max() / dimension))
                    throw std::invalid_argument("Invalid buffer shape or length overflow");
                length *= dimension;
            }
            if (length && !info->data) throw std::invalid_argument("Nonempty buffer has null data");
            if ((flags & PyBUF_WRITABLE) && info->readonly)
                throw std::invalid_argument("Buffer is read-only");
            Py_buffer full{};
            full.buf = info->data;
            full.len = length;
            full.itemsize = info->itemsize;
            full.readonly = info->readonly;
            full.ndim = static_cast<int>(info->shape.size());
            full.format = info->format.data();
            full.shape = info->shape.data();
            full.strides = info->strides.data();
            char order = (flags & PyBUF_C_CONTIGUOUS) == PyBUF_C_CONTIGUOUS ? 'C' :
                (flags & PyBUF_F_CONTIGUOUS) == PyBUF_F_CONTIGUOUS ? 'F' :
                (flags & PyBUF_ANY_CONTIGUOUS) == PyBUF_ANY_CONTIGUOUS ? 'A' :
                (flags & PyBUF_STRIDES) != PyBUF_STRIDES ? 'C' : 0;
            if (order && !PyBuffer_IsContiguous(&full, order))
                throw std::invalid_argument("Requested buffer layout is not contiguous");
            if (!(flags & PyBUF_FORMAT)) full.format = nullptr;
            if (!(flags & PyBUF_ND)) { full.shape = nullptr; full.ndim = 0; }
            if ((flags & PyBUF_STRIDES) != PyBUF_STRIDES) full.strides = nullptr;
            full.obj = exporter;
            full.internal = info.release();
            Py_INCREF(exporter);
            *view = full;
            return 0;
        } catch (nanobind::python_error &error) {
            error.restore();
            nanobind::chain_error(PyExc_BufferError, "Error getting buffer");
        } catch (const std::bad_alloc &) {
            PyErr_NoMemory();
            nanobind::chain_error(PyExc_BufferError, "Error getting buffer");
        } catch (const std::exception &error) {
            PyErr_SetString(PyExc_BufferError, error.what());
        } catch (...) {
            PyErr_SetString(PyExc_BufferError, "Buffer description failed");
        }
        return -1;
    }
    static void release(PyObject *, Py_buffer *view) noexcept {
        delete static_cast<buffer_info *>(view->internal);
        view->internal = nullptr;
    }
};
}

// Usage: nb::class_<T>(m, "T", dittobind::buffer_protocol<T, &describe>()).
// A function pointer avoids process-global Python callbacks or per-type state.
template <typename T, buffer_info (*Describe)(T &)>
nanobind::type_slots buffer_protocol() {
    static const PyType_Slot slots[] = {
        {Py_bf_getbuffer, reinterpret_cast<void *>(&detail::buffer_export<T, Describe>::get)},
        {Py_bf_releasebuffer, reinterpret_cast<void *>(&detail::buffer_export<T, Describe>::release)},
        {0, nullptr}
    };
    return nanobind::type_slots(slots);
}
}
