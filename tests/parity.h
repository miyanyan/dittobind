// Shared semantic acceptance tests: compiled against dittobind and pybind11.
// Included after the common bindings/helpers in embed.cpp.
static PyConfig parity_config(bool isolated = true) {
    PyConfig config;
    if (isolated) PyConfig_InitIsolatedConfig(&config);
    else PyConfig_InitPythonConfig(&config);
    config.parse_argv = 0;
    config.install_signal_handlers = 0;
    auto status = PyConfig_SetBytesString(&config, &config.home, DITTOBIND_PYTHON_HOME);
    if (PyStatus_Exception(status)) {
        PyConfig_Clear(&config);
        throw std::runtime_error("parity config home");
    }
    return config;
}

static void parity_body(const std::string &scenario) {
    auto main = binding::cast<binding::dict>(import_module("__main__").attr("__dict__"));
    main["host"] = import_module("test_host");
    if (scenario == "parity_restart" || scenario == "parity_manual" || scenario == "parity_move" || scenario == "parity_unwind") {
        require(!main.contains("old_generation"), "globals must be fresh after restart");
        execution::exec("old_generation = True");
        run_scenario("eval");
        run_scenario("calls");
        run_scenario("classes");
        run_scenario("exceptions");
        run_scenario("threads");
        run_scenario("ownership");
        run_scenario("module_failure");
        // Shutdown invokes bound C++ code while the backend is still alive.
        execution::exec("import atexit\natexit.register(lambda: host.add(20, 22))");
    } else if (scenario == "parity_frame_globals") {
        binding::dict other;
        other["host"] = main["host"];
        execution::exec("frame_marker = 41\nassert host.frame_eval() == 41\nhost.frame_exec()\nassert frame_written == 42", other);
        require(!main.contains("frame_written"), "callback eval must use the Python caller's module");
    } else if (scenario == "parity_eval") {
        binding::dict global, local;
        execution::exec("answer = len([1, 2, 3])", global, local);
        require(global.contains("__builtins__"), "inject builtins into explicit globals");
        require(!global.contains("answer") && binding::cast<int>(local["answer"]) == 3, "separate locals");
        global["__builtins__"] = binding::dict();
        bool caught = false;
        try { execution::eval("len([])", global); }
        catch (const python_error &e) { caught = std::string(e.what()).find("NameError") != std::string::npos; }
        require(caught, "honor custom builtins");
        require(execution::eval<execution::eval_statements>("x = 1", main).is_none(), "statement result");
        execution::exec("import sys\nseen = []\nsys.displayhook = seen.append");
        execution::eval<execution::eval_single_statement>("6 * 7");
        execution::exec("assert seen == [42]\nsys.displayhook = sys.__displayhook__");
        caught = false;
        try { execution::eval<static_cast<execution::eval_mode>(-1)>("1"); }
        catch (const std::runtime_error &) { caught = true; }
        require(caught, "reject invalid evaluation mode");
        caught = false;
        try { execution::exec("raise ValueError('line check')"); }
        catch (const python_error &e) {
            caught = std::string(e.what()).find("line check") != std::string::npos;
        }
        require(caught, "traceback retained");
    } else if (scenario == "parity_file") {
        const auto script = std::string(DITTOBIND_FIXTURES) + "/script.py";
        binding::dict global, local;
        global["__file__"] = binding::str("preserve-me.py");
        require(execution::eval_file(script, global, local).is_none(), "file statements return None");
        require(binding::cast<std::string>(global["__file__"]) == "preserve-me.py", "preserve __file__");
        require(!global.contains("file_answer") && binding::cast<int>(local["file_answer"]) == 42, "file locals");
        bool caught = false;
        try { execution::eval_file(script + ".missing", global); }
        catch (const python_error &) { throw std::runtime_error("missing file must be a C++ runtime_error"); }
        catch (const std::runtime_error &e) { caught = std::string(e.what()).find("could not be opened") != std::string::npos; }
        require(caught, "file open exception category");
        binding::dict expression;
        auto answer = execution::eval_file<execution::eval_expr>(std::string(DITTOBIND_FIXTURES) + "/expression.txt", expression);
        require(binding::cast<int>(answer) == 42, "file expression mode");
        // Use a real non-UTF-8 source and a Unicode filename; clean up on failure.
        execution::exec("import tempfile, pathlib\ntemp = tempfile.TemporaryDirectory()\nsource_path = pathlib.Path(temp.name) / '编码.py'\nsource_path.write_bytes(b'# coding: latin-1\\nword = \\'caf\\xe9\\'\\n')");
        auto path = binding::cast<std::string>(import_module("builtins").attr("str")(main["source_path"]));
        try {
            execution::eval_file(path, expression);
            require(binding::cast<std::string>(expression["word"]) == "café", "source encoding cookie");
        } catch (...) { main["temp"].attr("cleanup")(); throw; }
        main["temp"].attr("cleanup")();
    } else if (scenario == "parity_imports") {
        execution::exec("import math, json, fixture_module, importlib\nassert math.isqrt(1764) == 42\nassert json.loads('{\"answer\":42}')['answer'] == 42\nfixture_module.answer = 9\nimportlib.reload(fixture_module)\nassert fixture_module.answer == 42\nassert importlib.import_module('test_host') is host");
    } else if (scenario == "parity_raw_config") {
        execution::exec("import sys\nassert sys.flags.isolated == 1\nassert sys.flags.optimize == 2\nassert sys.flags.no_site == 1\nassert not __debug__\nassert 'site' not in sys.modules\nassert sys.argv == ['embedded', '--argument']");
    } else if (scenario == "parity_program_path") {
        execution::exec("import sys, os\nassert sys.path[0] == os.path.abspath(os.path.dirname(sys.argv[0]))");
    } else if (scenario == "parity_no_program_path") {
        execution::exec("import sys, os\nassert '' not in sys.path\nassert os.path.abspath(os.path.dirname(sys.argv[0])) not in sys.path");
    } else if (scenario == "parity_empty_argv") {
        execution::exec("import sys\nassert sys.argv == ['']\nassert sys.path[0] == ''");
    } else if (scenario == "parity_shutdown") {
        execution::exec("import atexit\natexit.register(lambda: host.add(20, 22))\nretained = host.make_tracked()");
    } else if (scenario == "parity_extensions") {
#ifdef DITTOBIND_REFERENCE
        auto extension = import_module("pybind11_ext");
#else
        auto extension = import_module("dittobind_ext");
#endif
        auto dictionary = binding::cast<binding::dict>(extension.attr("__dict__"));
        require(!dictionary.contains("old_generation"), "extension globals reset after restart");
        extension.attr("old_generation") = binding::int_(1);
        require(binding::cast<int>(extension.attr("add")(20, 22)) == 42, "extension function");
        auto value = extension.attr("Value")(21);
        require(binding::cast<int>(value.attr("twice")()) == 42, "extension class");
        require(binding::cast<int>(main["host"].attr("callback")(value.attr("add"), 21)) == 42,
                "extension callback");
    } else {
        throw std::runtime_error("Unknown parity scenario: " + scenario);
    }
}

static int run_parity(const std::string &scenario) {
    if (scenario == "parity_defaults") {
        execution::scoped_interpreter guard;
        try {
            execution::exec("import sys, signal\nassert sys.flags.isolated == 0\nassert sys.flags.ignore_environment == 0\nassert sys.argv == ['']\nassert sys.path[0] == ''\nassert signal.getsignal(signal.SIGINT) == signal.default_int_handler");
        } catch (const std::exception &e) { throw std::runtime_error(e.what()); }
        return 0;
    }
    const int cycles = (scenario == "parity_restart" || scenario == "parity_extensions") ? 20 :
        ((scenario == "parity_manual" || scenario == "parity_unwind") ? 3 : 1);
    for (int cycle = 0; cycle < cycles; ++cycle) {
        PyConfig config = parity_config();
        if (scenario == "parity_raw_config") { config.optimization_level = 2; config.site_import = 0; }
        const std::string script = std::string(DITTOBIND_FIXTURES) + "/script.py";
        const char *args[] = {"embedded", "--argument"};
        const char *script_args[] = {script.c_str()};
        const bool path_case = scenario == "parity_program_path" || scenario == "parity_no_program_path";
        const bool empty = scenario == "parity_empty_argv";
        const int count = empty ? 0 : (path_case ? 1 : 2);
        const char *const *arguments = empty ? nullptr : (path_case ? script_args : args);
        const bool add_path = scenario == "parity_program_path" || empty;
        auto exercise = [&] {
            if (scenario == "parity_imports")
                import_module("sys").attr("path").attr("insert")(0, DITTOBIND_FIXTURES);
            if (scenario == "parity_extensions")
                import_module("sys").attr("path").attr("insert")(0, DITTOBIND_EXTENSION_DIR);
            try { parity_body(scenario); }
            catch (const std::exception &e) { throw std::runtime_error(e.what()); }
        };
        if (scenario == "parity_manual") {
            execution::initialize_interpreter(&config, count, arguments, add_path);
            try { exercise(); }
            catch (...) { execution::finalize_interpreter(); throw; }
            execution::finalize_interpreter();
        } else if (scenario == "parity_unwind") {
            bool caught = false;
            try {
                execution::scoped_interpreter guard(&config, count, arguments, add_path);
                exercise();
                throw std::runtime_error("host unwind");
            } catch (const std::runtime_error &e) { caught = std::string(e.what()) == "host unwind"; }
            require(caught, "C++ stack unwinding closes interpreter");
        } else {
            execution::scoped_interpreter guard(&config, count, arguments, add_path);
            if (scenario == "parity_move") {
                { execution::scoped_interpreter moved(std::move(guard)); exercise(); }
                require(!Py_IsInitialized(), "moved guard owns shutdown");
            } else { exercise(); }
        }
        require(!Py_IsInitialized(), "shutdown after every generation");
        require(Tracked::live == 0, "release Python-owned C++ instances at shutdown");
    }
    std::cout << "PASS " << scenario << " (" << cycles << " generations)\n";
    return 0;
}
