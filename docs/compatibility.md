# pybind11 embedding 功能与行为对齐

基准日期：2026-10-02。目标是嵌入场景的功能与语义一致，不要求绑定声明语法、类型名、内部实现或异常文本完全相同。基准固定为 **pybind11 3.0.4**，后端为 **nanobind 3.1.0 + dittobind 源码适配层**。任何版本升级都需要重跑相同场景，不以移动的 latest 文档替代版本基准。

**当前整体状态：embedding 稳定性验收尚未完成。** 表中“通过”只覆盖所列行为及已验证平台。没有运行的测试、计划中的接口、后端宣称支持的特性都不计为通过。

## 范围与下一步

按 2026-10-02 更新的需求，目标聚焦 C++ 宿主中的 Python embedding，以 nanobind 已支持的绑定能力为基础。nanobind 明确不支持的 pybind11 绑定特性不再要求补齐，例如多重 C++ 继承和 pybind11 风格的任意自定义 holder。不要求 nanobind 的整个绑定 API、类型转换规则或异常文本复制 pybind11。

解释器生命周期、模块加载、线程状态/GIL、子解释器隔离以及这些操作涉及的对象清理仍是本项目责任，不能以 nanobind 原生不支持 embedding 为由排除。已有绑定、NumPy 和 buffer 测试保留为嵌入环境的回归样本，不继续扩展为整个绑定库的兼容认证。

后续按以下顺序推进：

1. **关闭与失败路径**：验证初始化失败后能否安全重试、主/子解释器销毁顺序、多 worker join、atexit/析构中的回调和异常；将违反生命周期约定的行为明确诊断。先保证正常 GIL 构建可靠。
2. **后端状态隔离与清理**：审计不同 nanobind domain、独立扩展 DLL 的状态归属、缓存、TLS 和释放时机，覆盖解释器重建和嵌套线程切换；对照嵌入 API 清单逐项收尾，包括 disarm 的语义。
3. **宿主配置与交付**：补充 Unicode argv/path、venv/site-packages、环境变量优先级及依赖部署；验证 Linux/macOS 和尚未实测的 Python 组合。多宿主 DLL/静态 CPython 单独注明支持状态，不作无限组合承诺。
4. **free-threaded 与性能**：free-threaded 的并发/关闭安全仍需专门验收，区分上游常驻对象与本项目引入的额外泄漏。常规 embedding 验收收敛后，再在相同配置和工作负载下比较启动、调用、线程切换、内存和关闭成本。

pybind11 基准失败应继续保留复现记录，查清测试前提和上游行为；基准崩溃不作为需要复刻的目标。完成标准是已声明支持范围内的 embedding 行为通过验收，且已知限制有明确说明，而不是补齐整个 pybind11 绑定库。

## 已有验收

`tests/embed.cpp` 和 `tests/parity.h` 分别编译成 dittobind 与 pybind11 两个可执行文件，执行相同输入、检查相同预期。每个测试为独立进程；重启测试特意在同一进程循环。独立扩展由 `tests/extension.cpp` 分别编译为两种后端模块。

| ID | 功能 / 可观察行为 | 验收场景 | 状态 |
| --- | --- | --- | --- |
| E01 | 普通初始化、默认 argv、环境配置、默认 SIGINT 处理器 | parity_defaults | 通过 |
| E02 | 原生 PyConfig、isolated、optimize、禁用 site、保留 argv 选项 | parity_raw_config、config | 通过所列字段 |
| E03 | 程序目录插入、关闭目录插入、空 argv | parity_program_path、parity_no_program_path、parity_empty_argv | 通过 |
| E04 | scoped 生命周期及正常重启 | parity_restart：每轮函数/类/异常/worker/所有权/atexit，连续 20 轮 | 通过 |
| E05 | 手动 initialize/finalize、移动 guard、C++ 异常退栈 | parity_manual、parity_move、parity_unwind | 通过 |
| E06 | 内建模块、多模块、初始化失败不破坏其他模块 | calls、module_failure、parity_restart | 通过 |
| E07 | 字符串执行、表达式/statement/single 三种模式、dedent | eval、parity_eval | 通过 |
| E08 | Python callback 的当前帧 globals，无帧时 __main__ | parity_frame_globals | 通过 |
| E09 | 显式 globals/locals、补充内建字典、尊重自定义 builtins | parity_eval | 通过 |
| E10 | 文件模式、独立 locals、保留 __file__、打开失败分类、编码 cookie、Unicode 文件名 | parity_file、file | 通过 |
| E11 | 纯 Python 与标准库扩展导入、纯 Python 模块 reload | parity_imports | 通过 |
| E12 | 独立绑定扩展的函数、类、回调及重启 | parity_extensions，连续 20 轮 | 通过 |
| B01 | C++→Python、Python→C++、回调往返、vector 转换 | calls | 通过这些类型 |
| B02 | C++ 类实例、宿主引用、shared_ptr 双向共享、关闭释放 | classes、ownership | 通过这些所有权策略 |
| B03 | Python/C++/callback 异常传播及恢复后继续调用 | exceptions | 通过所列异常 |
| T01 | GIL acquire/release、worker 调用、主线程 join | threads、parity_restart、workers_join：8 个 worker 各调用 100 次，10 代重启 | 通过所列并发场景 |
| L01 | 重复初始化、外部解释器接管、运行中注册、非法配置拒绝 | lifecycle、external、invalid_config | dittobind 约束测试通过，异常文本不作相等保证 |
| S01 | 子解释器、嵌套激活、globals/sys.modules/state_dict 隔离 | sub_isolation、sub_lifecycle，分别循环 3 代 | 通过所列行为 |
| S02 | 独立 GIL 并发、释放后重入同一解释器、回调 | sub_concurrent，双 worker 屏障 + 各 200 轮对象操作 | 通过 |
| S03 | 内建及独立扩展的 multiple_interpreters 标签 | sub_isolation、共享 GIL 模块与拒绝路径 | 通过 |
| S04a | 独立 DLL/domain 随子解释器及时清理 | sub_domains、sub_domains_late：每代 40 个子解释器，3 代；含只在 atexit 中导入的 backend | 通过；检查 backend 记录数和对象构造/析构计数，FT 内存限制另列 |
| L03 | 主/子解释器 atexit、析构回调、异常报告与后续清理 | shutdown_callbacks、sub_shutdown_callbacks：检查 2 次回调、1 次析构、2 次 unraisable 报告，10 代 | 两后端通过 |
| L04 | 非 owner、未附着、有存活子解释器、关闭重入 | shutdown_guards、shutdown_reentry；包括其他 worker 持有 GIL 时的 owner 拒绝 | dittobind 约束测试通过 |
| E13a | 部分 CPython 初始化失败、主/子 bootstrap 导入失败 | startup_partial_failure、startup_bootstrap_failure、sub_bootstrap_failure | 部分初始化后拒绝重试；已完成 CPython 初始化时回滚后可重试；子失败恢复主解释器 |
| E14a | 中文/emoji argv、脚本目录与模块名；迁移到 Unicode 目录的宿主 exe | config_unicode、config_relocated 及简化配置版本；验证标准库和 ssl 扩展加载 | Windows 通过，每项连续启动两代 |
| E14b | 环境读取、isolated、单独 use_environment=0、显式字段覆盖环境 | config_environment、config_isolated、config_no_environment、config_precedence | 两后端通过；含无效 PYTHONHOME 被隔离或显式覆盖 |
| E14c | Python 参数解析、追加路径顺序、完全显式 module_search_paths | config_parse_argv、config_search_paths、config_raw_paths | 两后端通过，检查 argv/flags、同名模块优先级及标准库 |
| E14d | venv 的 site-packages、禁用 site、解释器重启与环境切换 | config_venv、config_venv_no_site、config_venv_switch，含简化配置版本 | dittobind 通过同进程 A→B→基础环境；3.14 基准缓存失败另列 |
| B04a | 位置/关键字参数、默认值、重载、None、noconvert、args/kwargs | binding_arguments、binding_containers、binding_inheritance | 通过所列输入 |
| B04b | map/vector/optional/variant/tuple、虚函数 override、异常映射 | binding_containers、binding_inheritance、binding_exceptions | 通过所列类型 |
| B04c | reference_internal、keep_alive、unique_ptr 双向所有权、转移后 ValueError | binding_lifetimes、binding_unique | 通过所列生命周期 |
| B06a | NumPy float64 二维数组、C/F 布局、负步长、零长度、只读读取、禁止隐式 dtype 转换 | arrays，NumPy 2.2.6（3.10）/2.3.5（其他） | 通过 |
| B06b | 胶囊持有 C++ 数组、切片和 memoryview 共享及释放 | arrays，检查 C++ live counter 与双向修改 | 通过 |
| B06c | C++ 类型直接导出 buffer、正负 byte strides、只读写入拒绝、连续性请求、view 保活 | buffers，SIMPLE/ND/STRIDES/C/F/ANY 标志与 C++ live counter | 通过所列二维 double 布局 |
| T02a | free-threaded 构建、模块声明、无 GIL 双线程并发 | free_threaded，检查 sys._is_gil_enabled()，各 1000 轮操作 | 3.13.7t 通过；退出内存问题另列 |
| P02a | CMake 安装、独立消费端、部署匹配 Python DLL | install_consumer | 通过已运行组合 |

## 本地验证记录

2026-10-02，Windows x64、MSVC 19.51。下列为完整执行结果，未跳过失败用例：

| CPython / 配置 | dittobind | pybind11 3.0.4 | 完整结果 |
| --- | --- | --- | --- |
| 3.10.18 / Release | 63/63 | 45/45 | 108/108 |
| 3.11.13 / Release | 62/62 | 44/44 | 106/106；本组未启用 NumPy 测试 |
| 3.12.11 / Release | 72/72 | 49/51 | 121/123，两个基准失败 |
| 3.12.11 / Debug | 72/72 | 49/51 | 121/123，同上；CPython 本身为 Release |
| 3.14.7 / Release | 72/72 | 46/51 | 118/123，五个基准失败 |
| 3.13.7t / Release | 73/73 | 50/52 | 123/125，两个基准失败 |

每种后端的主解释器重启和独立扩展重启测试各执行 20 轮。普通 GIL 测试构建的 nanobind 宿主及独立扩展后端均开启 `NB_ABORT_ON_LEAK`；free-threaded 上游后端因永久保留类型/函数对象而禁用其内部泄漏检查，本项目没有将这类运行通过当作无泄漏保证。原始结果位于各构建目录 `compatibility-results.xml`，Debug 为 `compatibility-debug-results.xml`；汇总见 [验证记录](validation-results.json)。CI 配置尚未在远程执行。

六组配置的 dittobind 共 414/414 项通过。3.11 使用本地缓存安装固定版本 nanobind/pybind11；由于 PyPI 请求超时且缺少该解释器的 NumPy wheel 缓存，配置 `DITTOBIND_TEST_NUMPY=OFF`，未把未运行的数组用例计为通过。其余五组启用 NumPy。

宿主配置新增 12 个共享场景以及 8 个 dittobind 简化配置场景，每项在同一进程初始化两次；环境切换项执行三个环境。`configuration_runner.py` 使用独立临时目录创建 Unicode 路径和无 pip 的真实 venv，仅修改子进程环境；迁移测试将宿主 exe 和匹配 CPython DLL 复制到 Unicode 目录，并验证标准库/ssl 导入。原生配置和简化配置的 venv 均显式设置 `executable`、不设置 `home`。

本轮先复现 `sub_domains` 的“dead interpreter backend retained”，再验证修复后的跨 DLL 清理。`sub_domains_late` 每代主解释器连续创建 40 个仅在退出时导入独立 domain 的子解释器，检查每次退出后该 DLL 的 backend 记录归零以及 C++ 对象析构完成。

新增 `workers_join` 在 CPython 3.10 首次运行中复现崩溃：适配层错误地把其他 worker 的进程级当前 thread state 当成本线程已取得 GIL。修复后 `workers_join`、`shutdown_guards` 各连续执行 30 次通过，再执行上表完整回归；关闭保护也覆盖 owner 未附着、但另一个 worker 正持有 GIL 的情况。版本分支依据 [CPython 3.10.18 thread state 实现](https://github.com/python/cpython/blob/v3.10.18/Python/pystate.c) 与 [3.12.11 线程局部状态实现](https://github.com/python/cpython/blob/v3.12.11/Include/internal/pycore_pystate.h)。

free-threaded 屏障在等待期间 detach thread state，允许另一解释器触发 stop-the-world GC；普通构建仍在持有各自 GIL 时等待。两种后端均断言 GIL 未被启用。修正该测试同步方式后，`sub_concurrent` 和 `free_threaded` 四个用例各连续执行 20 次通过。

### 基准失败的处理

- `pybind11.sub_failure` 在 CPython 3.12.11 出现 `drop_gil: GIL is not locked`，对应私有 allocator 未开启扩展兼容性检查的非法配置。dittobind 在调用 CPython 前预检查该组合；有效创建和失败后恢复测试通过。内存耗尽等非确定性启动失败仍需单独测试。
- `pybind11.sub_worker` 在以上 3.12、3.14、3.13t 构建中崩溃，即使只执行一代也可复现。该用例包括 detached worker 创建解释器和跨线程销毁；dittobind 通过。尚未把该现象定位到具体上游缺陷，不能据此宣称任意线程操作已完全兼容。
- 3.13t 多轮完整回归记录到 `pybind11.sub_lifecycle`、`pybind11.sub_failure` 的 internals 重入 ImportError，两者存在间歇性；本轮最终报告为 `sub_lifecycle` 与 `sub_worker` 失败。3.14 本轮 `sub_isolation` 也记录到同类 ImportError。历史失败仍保留为未解决问题，不因其他运行通过而认定消失。
- CPython 3.14.7 上 `pybind11.config_venv`、`config_venv_no_site`、`config_venv_switch` 在第二代启动中把 `sys.base_prefix` 识别为第一代虚拟环境目录。相同用例在修复前的 dittobind 也失败；正常关闭后清理 CPython 路径缓存后，dittobind 通过。未给基准额外清理或跳过该断言。
- 原基准 3.0.1 在新增的普通子解释器关闭测试中崩溃；升级至 3.0.4 并完全重编译后，隔离和并发用例通过。旧的 51 项结果不能替代此次扩展验收。

复现：`ctest --test-dir build-msvc -C Release --output-on-failure -R "pybind11.sub_(failure|worker)"`。这些用例继续保留且默认执行；未设置跳过或将崩溃标为通过。仅运行 dittobind 自身测试可使用 `-R "^dittobind[.]"`。

## 尚未对齐 / 尚未覆盖

下列项目按 embedding 范围重新归类。范围外的绑定特性不阻塞 embedding 验收；尚未完成的 embedding 能力仍需跟进。

| ID | 差距 | 下一项验收 / 实现要求 |
| --- | --- | --- |
| S04 | 子解释器尚未完成后端全审计与 API 全覆盖 | 已补独立 domain/DLL 及时清理和析构回调；仍需跨解释器对象误用诊断、disarm、分配失败、嵌套 worker 与 FT pool/TLS 审计 |
| T02 | free-threaded 退出内存管理未对齐 | 上游永久保留类型/函数对象；需要处理生命周期、对象锁压力、pool/TLS 清理和竞争检测 |
| B04 | 整个绑定功能集的对齐移出本阶段范围 | caster、pickle、enum/operator 等沿用 nanobind；仅在其与解释器创建/关闭、回调或线程状态相互作用时补充回归 |
| B05 | 明确不支持的绑定特性排除 | 不补多重 C++ 继承及 pybind11 风格的任意自定义 holder；纯绑定语义沿用 nanobind，嵌入生命周期安全仍需保证 |
| B06 | 数组完整 API 对齐移出本阶段范围 | 不穷举 dtype/DLPack/buffer 功能；保留已有互操作用例，关注导入、Python/C++ 所有权和关闭，记录 NumPy 自身的重启/子解释器限制 |
| E13 | 初始化失败的极端路径未覆盖 | 已区分并实测前置校验、部分初始化、主/子 bootstrap 失败；CPython 内部任意位置的内存分配失败仍未注入验证，部分初始化失败维持禁止重试 |
| E14 | 宿主配置代表性场景已补充，跨平台仍待验证 | Windows 已覆盖 Unicode argv/path、venv/site-packages、环境变量优先级、参数解析和显式搜索路径；Linux/macOS 需实际执行相同用例，不要求穷举 CPython 所有字段组合 |
| L02 | 扩展样本与复杂析构图需继续覆盖 | 已补多 worker、关闭重入及 atexit/析构异常；仍需悬挂回调和更复杂引用图验证；第三方扩展沿用其支持边界，不承诺任意扩展可重启 |
| P01 | Linux/macOS、常规 Python 3.13 等组合未实测 | Windows 3.11 已补基础 embedding 回归但 NumPy 未运行；其他组合已编写 CI 配置，实际执行后才更新支持状态 |
| P02 | 多宿主 DLL、静态 CPython 未覆盖 | 运行库部署及符号/状态归属验证；安装消费端已单独通过 |

## 已实施的后端适配

`tools/prepare_nanobind.py` 校验固定版本源码哈希，将适配结果写入构建目录 `_deps/nanobind`，从不修改 site-packages。内部 ABI 改为 2202，使原版与修改版状态隔离。2201 消费端需重新编译。主要改动：

1. `NB_CTX` 按当前解释器查询后端状态；import cache 移入每个后端状态。
2. 模块定义构造同步、能力标签配置；避免独立 GIL 并发写入全局元类 slot。
3. backend registry 同步；解释器字典保存跨 DLL/domain 的清理注册表，以 C++ 所有权跨过 `Py_EndInterpreter`，随后调用各 backend 回收记录。注册表保留到关闭完成，覆盖 atexit 中新导入的 backend。每个 backend 每代主解释器只注册一次 `Py_AtExit`，不因子解释器记录清空而重复占用槽位。
4. GIL 释放记录当前 thread state，随后 acquire 和 callback 重入同一子解释器。CPython 3.10/3.11 用 `PyGILState_Check` 判断当前线程是否持有 GIL，避免进程级 unchecked 状态造成跨线程误判；3.12+ 子解释器使用线程局部状态查询。
5. 转移所有权后的实例方法错误类别对齐为 ValueError。

子解释器创建阶段串行处理 CPython 初始化，用户执行阶段不共用全局执行锁。上述改造有针对性验收，但不等于已完成任意 domain、任意第三方扩展及 free-threaded 内存管理的完整审计。

宿主关闭和已完整初始化后的回滚通过 `Py_SetPath(nullptr)` 清理进程级路径缓存，再允许下一次启动；不在第一次启动前清除用户输入。[CPython 3.14.7 pathconfig.c](https://github.com/python/cpython/blob/v3.14.7/Python/pathconfig.c) 中该入口保留为 Stable ABI 符号，虽然 3.13+ 头文件已移除声明。项目为该版本分支声明兼容入口，并在全部本地 CPython 组合中验证；未来升级仍需重验。每代需重新提交 PyConfig/简化配置，不承诺旧式全局路径 setter 跨关闭持续有效。

## 验收规则

- 可观察结果、错误类别和对象释放行为均需检查；运行不崩溃不等于通过。
- 所有 Python 异常必须在当前解释器还活着时转换或销毁。线程测试有超时，nanobind 宿主后端启用 `NB_ABORT_ON_LEAK`。
- 一份共享源码同时编译、运行两套实现；因命名不同允许薄适配，语义断言不得按后端分支降低标准。
- 范围内暂时无法覆盖的能力明确标记缺口；范围外明确标记排除，不添加“跳过=通过”的伪验收。
- 先收敛已声明支持范围的 embedding 验收，再进入性能优化。后续性能变化必须继续通过同一组行为测试。

参考：[pybind11 3.0.4 embed.h](https://github.com/pybind/pybind11/blob/v3.0.4/include/pybind11/embed.h)、[eval.h](https://github.com/pybind/pybind11/blob/v3.0.4/include/pybind11/eval.h)、[subinterpreter.h](https://github.com/pybind/pybind11/blob/v3.0.4/include/pybind11/subinterpreter.h)、[nanobind 官方差异说明](https://nanobind.readthedocs.io/en/latest/porting.html)、[free-threaded 内存限制](https://nanobind.readthedocs.io/en/latest/free_threaded.html)、[CPython 3.12.11 初始化源码](https://github.com/python/cpython/blob/v3.12.11/Python/pylifecycle.c)。版本化源码是验收基准，文档仅作背景说明。
