#ifdef DITTOBIND_REFERENCE
#include <pybind11/embed.h>
namespace binding = pybind11;
namespace execution = pybind11;
#else
#include <dittobind/embed.h>
namespace binding = nanobind;
namespace execution = dittobind;
#endif
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool result, const char *message) {
    if (!result) throw std::runtime_error(message);
}
static std::string environment(const char *key) {
#ifdef _WIN32
    const std::string name(key);
    const std::wstring wide(name.begin(), name.end());
    auto *value = _wgetenv(wide.c_str());
    return value ? std::filesystem::path(value).u8string() : "";
#else
    auto *value = std::getenv(key);
    return value ? value : "";
#endif
}
static void check(PyStatus status) {
    if (PyStatus_Exception(status))
        throw std::runtime_error(status.err_msg ? status.err_msg : "configuration failed");
}
static void append_path(PyConfig &config, const std::string &path) {
    wchar_t *wide = nullptr;
    check(PyConfig_SetBytesString(&config, &wide, path.c_str()));
    auto status = PyWideStringList_Append(&config.module_search_paths, wide);
    PyMem_RawFree(wide);
    check(status);
}
static void verify(const std::string &scenario, const std::string &root, int cycle) {
    auto globals = execution::globals();
    globals["root_text"] = binding::str(root.c_str());
    globals["base_home"] = binding::str(DITTOBIND_PYTHON_HOME);
    globals["venv_text"] = binding::str(environment("DITTO_CONFIG_VENV").c_str());
    if (scenario == "venv_switch" && cycle == 1)
        globals["venv_text"] = binding::str(environment("DITTO_CONFIG_VENV_B").c_str());
    globals["expected_answer"] = binding::int_(scenario == "venv_switch" && cycle == 1 ? 84 : 42);
    execution::exec("import sys, pathlib, importlib.util\nroot = pathlib.Path(root_text)");
    if (scenario == "unicode") {
        execution::exec(R"(
            assert sys.argv == [str(root / '启动.py'), '--名称=中文🐍', '带 空格'], sys.argv
            assert pathlib.Path(sys.path[0]) == root, sys.path
            import 路径模块
            assert 路径模块.answer == 42
        )");
    } else if (scenario == "relocated") {
        globals["host_executable"] = binding::str(environment("DITTO_CONFIG_EXECUTABLE").c_str());
        execution::exec(R"(
            import json, math, ssl
            assert pathlib.Path(sys.executable) == pathlib.Path(host_executable)
            assert pathlib.Path(sys.base_prefix) == pathlib.Path(base_home)
            assert math.isqrt(json.loads('1764')) == 42
            assert ssl.OPENSSL_VERSION
            assert '' not in sys.path
        )");
    } else if (scenario == "parse_argv") {
        execution::exec("assert sys.argv == ['-c', 'tail'], sys.argv\nassert sys.flags.optimize == 1\nassert sys.dont_write_bytecode");
    } else if (scenario == "environment") {
        execution::exec(R"(
            import priority_marker
            assert priority_marker.value == 'environment'
            assert sys.flags.optimize == 2 and sys.dont_write_bytecode
            assert sys.flags.ignore_environment == 0 and sys.flags.isolated == 0
            assert pathlib.Path(sys.base_prefix) == pathlib.Path(base_home)
        )");
    } else if (scenario == "isolated" || scenario == "no_environment") {
        execution::exec(R"(
            assert importlib.util.find_spec('priority_marker') is None
            assert sys.flags.ignore_environment == 1
            assert sys.flags.optimize == 0 and not sys.dont_write_bytecode
            assert pathlib.Path(sys.base_prefix) == pathlib.Path(base_home)
            assert '' not in sys.path
        )");
        require(binding::cast<int>(execution::eval("sys.flags.isolated")) == int(scenario == "isolated"), "isolated and ignore_environment are independent");
    } else if (scenario == "precedence") {
        execution::exec(R"(
            import priority_marker
            assert priority_marker.value == 'environment'
            assert sys.flags.optimize == 1 and not sys.dont_write_bytecode
            assert pathlib.Path(sys.base_prefix) == pathlib.Path(base_home)
        )");
    } else if (scenario == "search_paths" || scenario == "raw_paths") {
        execution::exec(R"(
            import priority_marker, json, math
            assert priority_marker.value == 'first'
            assert [pathlib.Path(p) for p in sys.path[:2]] == [root / 'first', root / 'second']
            assert math.isqrt(json.loads('1764')) == 42
        )");
        if (scenario == "raw_paths")
            execution::exec("assert len(sys.path) == 4, sys.path\nassert str(root / 'environment') not in sys.path");
        else execution::exec("assert str(root / 'environment') in sys.path");
    } else if (scenario == "venv_switch" && cycle == 2) {
        execution::exec("assert sys.prefix == sys.base_prefix\nassert pathlib.Path(sys.prefix) == pathlib.Path(base_home)\nassert importlib.util.find_spec('venv_marker') is None");
    } else if (scenario == "venv" || scenario == "venv_no_site" || scenario == "venv_switch") {
        execution::exec("assert pathlib.Path(sys.base_prefix) == pathlib.Path(base_home), (sys.base_prefix, base_home)\nassert pathlib.Path(sys.executable).is_relative_to(pathlib.Path(venv_text)), (sys.executable, venv_text)");
        if (scenario != "venv_no_site") {
            execution::exec(R"(
                import venv_marker
                assert venv_marker.answer == expected_answer
                assert pathlib.Path(venv_marker.__file__).is_relative_to(pathlib.Path(venv_text))
                assert pathlib.Path(sys.prefix) == pathlib.Path(venv_text)
                assert sys.prefix != sys.base_prefix
            )");
        } else {
            execution::exec("assert 'site' not in sys.modules\nassert importlib.util.find_spec('venv_marker') is None");
#if PY_VERSION_HEX >= 0x030E0000
            execution::exec("assert pathlib.Path(sys.prefix) == pathlib.Path(venv_text)");
#else
            execution::exec("assert sys.prefix == sys.base_prefix");
#endif
        }
    } else throw std::runtime_error("unknown configuration scenario");
}
int main(int argc, char **argv) {
    try {
        require(argc == 2, "scenario required");
        std::string scenario(argv[1]);
        const std::string suffix = "_options";
        const bool options = scenario.size() > suffix.size() &&
            scenario.compare(scenario.size() - suffix.size(), suffix.size(), suffix) == 0;
        if (options) scenario.resize(scenario.size() - suffix.size());
        const auto root = environment("DITTO_CONFIG_ROOT");
        require(!root.empty(), "fixture root required");
        const auto script = (std::filesystem::u8path(root) / std::filesystem::u8path("启动.py")).u8string();
        const char *arguments[] = {script.c_str(), "--名称=中文🐍", "带 空格"};
        const bool venv_case = scenario.rfind("venv", 0) == 0;
        const bool isolated = scenario == "isolated" || scenario == "unicode" || venv_case ||
            scenario == "raw_paths" || scenario == "relocated" || scenario == "parse_argv";
        const bool add_program_dir = scenario == "unicode";
        auto exercise = [&](int cycle) {
            try { verify(scenario, root, cycle); }
            catch (const std::exception &e) { throw std::runtime_error("cycle " + std::to_string(cycle) + ": " + e.what()); }
        };
        for (int cycle = 0; cycle < (scenario == "venv_switch" ? 3 : 2); ++cycle) {
            const bool virtualenv = venv_case && !(scenario == "venv_switch" && cycle == 2);
            const bool custom_executable = virtualenv || scenario == "relocated";
            const auto executable_key = scenario == "venv_switch" && cycle == 1 ?
                "DITTO_CONFIG_EXECUTABLE_B" : "DITTO_CONFIG_EXECUTABLE";
            const auto program = custom_executable ? environment(executable_key) :
                environment("DITTO_CONFIG_BASE_EXECUTABLE");
#ifndef DITTOBIND_REFERENCE
            if (options) {
                execution::interpreter_config config;
                config.isolated = isolated;
                config.add_program_dir_to_path = add_program_dir;
                config.program_name = program;
                if (venv_case) config.executable = config.program_name;
                // For venv discovery, home must remain unset.
                if (!virtualenv && scenario != "environment") config.python_home = DITTOBIND_PYTHON_HOME;
                config.argv = {arguments[0], arguments[1], arguments[2]};
                config.import_site = scenario != "venv_no_site";
                if (scenario == "search_paths") config.search_paths = {root + "/first", root + "/second"};
                execution::scoped_interpreter guard(config);
                exercise(cycle);
                continue;
            }
#endif
            PyConfig config;
            if (isolated) PyConfig_InitIsolatedConfig(&config);
            else PyConfig_InitPythonConfig(&config);
            config.parse_argv = scenario == "parse_argv" ? 1 : 0;
            config.install_signal_handlers = 0;
            check(PyConfig_SetBytesString(&config, &config.program_name, program.c_str()));
            if (venv_case) check(PyConfig_SetBytesString(&config, &config.executable, program.c_str()));
            if (!virtualenv && scenario != "environment")
                check(PyConfig_SetBytesString(&config, &config.home, DITTOBIND_PYTHON_HOME));
            if (scenario == "no_environment") config.use_environment = 0;
            if (scenario == "precedence") { config.optimization_level = 1; config.write_bytecode = 1; }
            if (scenario == "venv_no_site" || scenario == "raw_paths") config.site_import = 0;
            if (scenario == "raw_paths") {
                config.module_search_paths_set = 1;
                for (const auto &path : {root + "/first", root + "/second",
                        environment("DITTO_CONFIG_STDLIB"), environment("DITTO_CONFIG_DYNLOAD")}) append_path(config, path);
            }
            const char *parsed[] = {script.c_str(), "-O", "-B", "-c", "pass", "tail"};
            execution::scoped_interpreter guard(&config, scenario == "parse_argv" ? 6 : 3,
                scenario == "parse_argv" ? parsed : arguments, add_program_dir);
            if (scenario == "search_paths") {
                // pybind11 has no search_paths convenience field. Configure the
                // equivalent insertion order explicitly for this shared case.
                try {
                    auto globals = execution::globals();
                    globals["root_text"] = binding::str(root.c_str());
                    execution::exec("import sys\nsys.path[:0] = [root_text + '/first', root_text + '/second']");
                } catch (const std::exception &e) { throw std::runtime_error(e.what()); }
            }
            exercise(cycle);
        }
        std::cout << "PASS " << argv[1] << '\n';
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
