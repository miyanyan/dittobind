#include <dittobind/embed.h>
#include "domain_api.h"
#include <atomic>

// A separate DLL and domain must own its own backend records. Querying this
// module from main lets the test detect records left behind by dead children.
static std::atomic<int> created{0}, alive{0};
struct DomainValue {
    int value;
    explicit DomainValue(int v) : value(v) { ++created; ++alive; }
    ~DomainValue() { --alive; }
};
static domain_diagnostics diagnostics{
    &nanobind::detail::interpreter_record_count,
    []() noexcept { return created.load(); },
    []() noexcept { return alive.load(); }};
DITTOBIND_MODULE(dittobind_domain_ext, m,
                dittobind::multiple_interpreters::per_interpreter_gil(),
                dittobind::mod_gil_not_used()) {
    nanobind::class_<DomainValue>(m, "Value")
        .def(nanobind::init<int>()).def_ro("value", &DomainValue::value);
    m.def("records", &nanobind::detail::interpreter_record_count);
    m.attr("diagnostics") = nanobind::capsule(&diagnostics, "dittobind.test.domain");
}
