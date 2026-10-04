# dittobind

基于 nanobind 的 C++ 嵌入 CPython 库。当前优先级是**对齐 pybind11 的嵌入功能和可观察行为，完成兼容验收后再优化性能**。绑定语法沿用 nanobind，不承诺 pybind11 源码直接替换。

范围聚焦解释器生命周期、执行与导入、线程/GIL、子解释器及宿主部署。nanobind 明确不支持的绑定特性（例如多重 C++ 继承、pybind11 风格自定义 holder）不要求补齐；不将整个绑定库的兼容性作为 embedding 的完成条件。

对齐基准固定为 pybind11 3.0.4，后端固定为 nanobind 3.1.0 的可复现源码适配层。embedding 稳定性验收尚未完成；已验证的行为、缺口和后续顺序见 [兼容矩阵](docs/compatibility.md)。

## 当前能力

- `scoped_interpreter`，支持移动构造、正常关闭后的再次初始化。
- `initialize_interpreter` / `finalize_interpreter`，支持显式生命周期管理。
- pybind11 风格的 bool/argc/argv 初始化参数，以及原生 `PyConfig*` 配置入口。
- `interpreter_config` 便利配置：Python home、程序名、argv、路径、isolated/site/signal 开关。
- `DITTOBIND_EMBEDDED_MODULE`、模块导入、C++ 函数与类、Python callback、异常、STL 转换、共享所有权。
- `globals`、`exec`、`eval`、`eval_file`、`exec_file`；支持显式 globals/locals、三种执行模式、文件编码和 traceback。
- 显式 GIL 获取/释放和工作线程调用。
- Python 3.12+ 子解释器创建、激活、嵌套恢复、独立 GIL 并发和跨线程销毁；支持模块能力标记。
- free-threaded 构建、`mod_gil_not_used` 声明和并发调用；仍有后端对象永久保留的限制。
- 独立扩展 `DITTOBIND_MODULE`、NumPy 数组互操作，以及安装后的 `find_package(dittobind)` 接入。
- C++ 类型直接导出 buffer：`<dittobind/buffer.h>`，支持只读、步长、memoryview 保活和连续性请求。

## 构建和运行

需要 C++17、CMake 3.24+、带开发头文件和链接库的 CPython 3.10+；free-threaded 构建使用 CMake 3.30+。实测组合及基准失败记录见兼容矩阵。

```powershell
uv venv --python 3.12 .venv
uv pip install --python .venv/Scripts/python.exe -r requirements-dev.txt
cmake -S . -B build-msvc -G "Visual Studio 18 2026" -A x64 -DPython_EXECUTABLE="$PWD/.venv/Scripts/python.exe" -DDITTOBIND_COMPARE_PYBIND11=ON -DDITTOBIND_TEST_NUMPY=ON
cmake --build build-msvc --config Release -j 8
ctest --test-dir build-msvc -C Release --output-on-failure --output-junit compatibility-results.xml
./build-msvc/Release/dittobind_hello.exe
```

Visual Studio 2022 使用对应生成器。Linux/macOS 使用自己的生成器和 Python 路径，单配置生成器添加 `-DCMAKE_BUILD_TYPE=Release`。已添加三个操作系统、Python 3.10–3.14 的 [CI 配置](.github/workflows/compatibility.yml)，但尚未在远程运行；这些组合不能视为已验证。

示例输出 `Hello, embedded Python` 和 `Python answer: 42`。pybind11 只用于对照测试，dittobind 库和运行时不依赖它。版本升级需要重新做兼容验收。

## 接入

```cmake
add_subdirectory(path/to/dittobind)
add_executable(my_host main.cpp)
target_link_libraries(my_host PRIVATE dittobind::embed)
```

指定 `Python_EXECUTABLE` 并在该环境安装 nanobind 3.1.0。作为子项目时默认关闭测试和示例。构建会校验原始源码哈希，并将适配后的源码放在构建目录 `_deps/nanobind`；不会修改 site-packages。

也可执行 `cmake --install build-msvc --config Release --prefix <目录>`，消费端设置 `CMAKE_PREFIX_PATH` 后使用 `find_package(dittobind CONFIG REQUIRED)` 和 `dittobind::embed`。安装包自带适配后的 nanobind 头文件、后端库和第三方许可；消费端须使用相同 Python 主次版本、GIL ABI、架构及兼容的 C++ 运行库。

```cpp
#include <dittobind/embed.h>
#include <iostream>

namespace nb = nanobind;
DITTOBIND_EMBEDDED_MODULE(host, m) {
    m.def("add", [](int a, int b) { return a + b; });
}

int main() {
    dittobind::interpreter_config config;
    config.python_home = "C:/path/to/python";
    dittobind::scoped_interpreter python(config);
    try {
        dittobind::exec("import host\nanswer = host.add(20, 22)");
        std::cout << nb::cast<int>(dittobind::eval("answer")) << '\n';
    } catch (const nb::python_error &error) {
        // 异常也持有 Python 引用，必须在 guard 销毁前释放。
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

[完整示例](examples/hello.cpp) 还展示 C++ 类绑定和启动错误处理。

`python_home` 指向 Python 基础安装目录（`sys.base_prefix`），不是 venv 目录。示例由 CMake 注入该路径。显式 home 不会自动启用 venv 的第三方包路径，可通过 `search_paths` 添加；列表中的前项优先。

默认采用普通 Python 配置、安装信号处理器、将 argv[0] 对应的已有文件目录加入 `sys.path`，不存在或无 argv 时加入空字符串，行为对齐 pybind11。需要隔离环境时显式设置 `isolated = true`；如不希望额外插入程序目录，还要设置 `add_program_dir_to_path = false`。这两个开关互相独立。

原生 `PyConfig*` 入口保留调用者设定的其他配置，用传入 argc/argv 替换 config.argv，并在接受启动后自动 `PyConfig_Clear`。重复初始化的拒绝发生在接管配置前，调用者仍需清理自己的配置。

要使用已有虚拟环境，显式设置 `executable`，不要把 `python_home` 指向虚拟环境目录：

```cpp
dittobind::interpreter_config config;
config.executable = R"(D:\app\.venv\Scripts\python.exe)"; // Linux/macOS: .venv/bin/python
config.program_name = config.executable;
config.isolated = true;
config.add_program_dir_to_path = false;
// python_home 留空，让 CPython 读取该环境的 pyvenv.cfg；import_site 保持 true。
dittobind::scoped_interpreter python(config);
dittobind::exec("import sys; print(sys.prefix)");
```

`program_name` 是程序名称输入，不能跨版本替代 `executable`。简化配置的 `executable` 对应原生 `PyConfig.executable`，采用同样的字节解码规则。`isolated` 会忽略 `PYTHON*` 环境变量，但不禁止虚拟环境的 site-packages；`import_site = false` 才会禁用 site 初始化。`search_paths` 在启动完成后按给定顺序插到 `sys.path` 前端，不替换标准库路径；如需完全指定搜索路径，应使用原生 `module_search_paths` / `module_search_paths_set`。

Windows 示例复制匹配的 Python DLL，并关闭示例目标的全局 vcpkg 自动部署。自己的宿主仍需部署匹配 DLL、标准库及第三方扩展。模块宏应写在直接链接进 exe 的 `.cpp` 或 OBJECT library 中，避免仅含静态注册副作用的对象文件被链接器丢弃。

## 生命周期约定

- 同一时刻只允许一个主解释器。正常关闭后可再次初始化；不得在解释器间保留 Python 对象、异常或 callback。
- 创建和关闭必须在同一线程，关闭时持有 GIL；可移动 guard 不改变其线程归属。
- 所有 `nb::object`、`nb::python_error` 和持有 Python callback 的 `std::function` 都必须在关闭前释放。禁止静态 Python 对象。
- 工作线程先取得 GIL；主线程等待 worker 前释放 GIL；关闭前 join 全部 worker。库不自动调度或停止线程。
- 以 `rv_policy::reference` 暴露的宿主对象必须比其 Python 包装活得更久。GIL 不替代宿主对象的线程同步。
- 使用手动生命周期接口时，不要同时保留 owning guard，也不要调用外部的 `Py_FinalizeEx`。
- `finalize_interpreter()` 返回 CPython 关闭状态。guard 析构在非零状态时打印诊断；生命周期约定被违反时终止进程。
- 部分 CPython 初始化失败后当前进程不能重试；前置参数校验失败不影响后续启动。CPython 已完整初始化、但 dittobind bootstrap 失败时会先关闭再允许重试。任意第三方扩展的重启兼容性需要单独验证。
- 正常关闭及完整初始化后的回滚会清理 CPython 的进程级路径缓存，下一次启动重新使用配置和环境变量计算路径，避免继承上一个虚拟环境。每次启动应重新传入所需配置；不保证已弃用的全局路径 setter 跨关闭保持有效。

`globals()` 在 Python 回调中返回调用方的全局字典，没有 Python 栈帧时使用 `__main__`。不同 globals 仍共享解释器和 `sys.modules`。

`eval_file` 与 pybind11 一样保留已存在的 `__file__`，只在缺失时设置；打开失败抛 C++ `std::runtime_error`，脚本执行错误抛 `nb::python_error`。`exec_file` 是 statement 模式的便利入口。这修正了 0.1 原型覆盖 `__file__` 和抛 `FileNotFoundError` 的行为。

## 实现边界

启动先通过 CPython C API 导入 `_dittobind_runtime`，由正常的 `NB_MODULE` 路径初始化后端。模块注册只添加一次，名称存储在稳定地址；每次启动重新导入模块，关闭交给 CPython 和 nanobind 的清理逻辑。没有修改已安装的 nanobind 源码。

本项目承担修改后 nanobind 后端的维护和测试责任。适配层使用独立内部 ABI 2202，避免和原版 nanobind 的内部状态混用；从 2201 升级时需重新编译宿主及适配层扩展。需要在宿主与扩展之间交换绑定类型时，双方都必须使用此适配层；普通第三方扩展仍受其自身的子解释器、重启及 GIL 限制。

子解释器接口位于 `<dittobind/subinterpreter.h>`。先创建主解释器，再用 `subinterpreter::create()` 创建子解释器，用 `subinterpreter_scoped_activate` 在作用域内切换。Python 对象和异常必须在所属解释器激活期间销毁；先结束 worker，再销毁子解释器，最后关闭主解释器。内建模块和外部扩展分别使用 `DITTOBIND_EMBEDDED_MODULE` 和 `DITTOBIND_MODULE`，可传入 `dittobind::multiple_interpreters::per_interpreter_gil()`。该标记要求绑定代码自身没有跨解释器共享的 Python 对象。

子解释器清理会汇集同一适配 ABI 的各个 backend DLL/domain，等 `Py_EndInterpreter` 完成 Python 析构后再检查并回收 backend 记录，也包括关闭回调临时导入的 backend。不能提前卸载这些 DLL。该机制不接管未经适配的第三方 backend。

free-threaded 模块可增加 `dittobind::mod_gil_not_used()`；C++ 可变数据仍需使用原子或锁。当前 nanobind 后端会永久保留部分类型和函数对象，并在该构建下关闭其原有泄漏检测，因此无 GIL 测试通过不代表无泄漏重启。测试构建对普通 GIL 的宿主和独立扩展后端均启用 `NB_ABORT_ON_LEAK`。

多个宿主 DLL 各自管理运行时的场景尚未验证；第三方扩展的重启与子解释器能力取决于扩展自身。任意自定义 holder、多重 C++ 继承不在本项目对齐范围内。当前保留了 pybind11 自身会失败的边界测试，完整对照执行可能返回非零；具体失败及复现命令见兼容矩阵。

子解释器、线程状态和解释器退出时的对象清理仍在 embedding 目标内。已有绑定/数组/buffer 测试保留为回归样本，不继续扩展为整个 pybind11 的替代实现。先完成 embedding 稳定性验收，再开展性能比较与优化。

导出 C++ buffer 时，在类型构造参数中添加 `dittobind::buffer_protocol<T, &describe>()`。`describe(T&)` 返回 `dittobind::buffer_info`，描述数据指针、元素字节数、PEP 3118 format、shape、字节 strides 和 readonly。每个 Python view 独立持有元数据并保活宿主对象；宿主必须保证 view 存活期间底层存储不被释放或重新分配。示例与验收见 `tests/buffers.cpp`。
