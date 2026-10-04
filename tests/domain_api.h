#pragma once
#include <cstddef>
struct domain_diagnostics {
    size_t (*records)() noexcept;
    int (*constructed)() noexcept;
    int (*live)() noexcept;
};
