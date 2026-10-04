**参考 Cordis 的 C++／Python 插件方案：可行性与设计草案**

2026-09-20。本文是源码调研与拟议设计，不代表 dittobind 已实现这些接口。

分析基于 Cordis 提交 `f8ea3cd50f1a5724e8e715995bcde131c9c12b2c`，其中 core 的 package.json 为 `4.0.0-rc.10`。README 明确说明 API 尚未稳定。阅读了 core 的 context、fiber、registry、reflect、service、events、utils，实现相关测试，以及 timer 与 HMR 的部分源码；未运行 Cordis 测试。

**结论与定位**

可以借鉴，而且这能把此前“更完善的嵌入式 Python 基础设施”落实为明确产品：由 C++ 宿主驱动、C++ 和 Python 插件共同参与的动态服务与资源生命周期系统。

借鉴重点是 Context、服务依赖、可撤销注册、实例作用域及依赖变化驱动的启停。C++ 的引用、线程、动态库和 Python 的解释器生命周期需要另外设计；不能逐行翻译 TypeScript 后期待等价行为。

Cordis 论文将关注点分为组件移除时的副作用撤销，以及组件依赖变化时的动态组合。本文只借鉴其设计思路，没有验证其形式化证明，也不将其结论直接套用到 C++／Python。[论文摘要](https://arxiv.org/abs/2608.25512)

**源码中实际存在的机制**

| 机制 | Cordis 实现 | C++／Python 对应设计 |
| --- | --- | --- |
| 上下文 | Context.extend、isolate、intercept 派生服务视图和配置 | ContextView，保存作用域、服务命名空间及配置；不等于另建解释器 |
| 插件定义与实例 | Registry 保存插件 runtime；每次挂载创建 Fiber | PluginDefinition 与 PluginInstance 分离；一份代码可以有多组配置与多个实例 |
| 动态依赖 | inject 声明需求；Reflect.notify 通知 Fiber 重新检查依赖 | 统一服务注册表、依赖图和激活状态机；服务就绪才激活消费方 |
| 受管资源 | Fiber.effect 收集 disposer；事件、服务与 timer 通过它登记 | ResourceScope 统一持有订阅、任务、定时器、服务注册及显式清理动作 |
| 服务调用上下文 | traceable proxy 区分服务定义处和使用处 | 服务适配器显式携带 DefinitionContext 与调用方 ScopeToken |
| 动态重载 | core 负责实例生命周期；loader 和 HMR 分包处理代码与配置加载 | 生命周期核心和 Python／native loader 分开；各自声明能保证的重载范围 |

源码：[Context](https://github.com/cordiverse/cordis/blob/f8ea3cd50f1a5724e8e715995bcde131c9c12b2c/packages/core/src/context.ts)、[Registry](https://github.com/cordiverse/cordis/blob/f8ea3cd50f1a5724e8e715995bcde131c9c12b2c/packages/core/src/registry.ts)、[Fiber](https://github.com/cordiverse/cordis/blob/f8ea3cd50f1a5724e8e715995bcde131c9c12b2c/packages/core/src/fiber.ts)、[Reflect](https://github.com/cordiverse/cordis/blob/f8ea3cd50f1a5724e8e715995bcde131c9c12b2c/packages/core/src/reflect.ts)。

清理顺序需要准确理解：此版本在一个 effect 内对 disposer 反序串联；Fiber._unload 对不同顶层 effect 使用 Promise.all 并发等待。因此不能宣称 Cordis 的所有清理动作均严格按全局反序完成。C++ 版本应独立规定依赖资源的释放顺序。

**最值得保留：通过共享服务创建的资源属于调用插件**

例如 Python 插件 A 调用 C++ UI 服务创建菜单项。菜单项应在 A 停止时删除，而不能一直存活到整个 UI 服务销毁。类似情况还包括命令注册、定时器、事件监听与文件观察器。

Cordis 的 utils.ts 明确区分 def site 和 use site：服务自己的依赖解析来自定义处，effects 等行为跟随使用处。这比“从容器取一个服务指针”更有价值。[traceable 实现](https://github.com/cordiverse/cordis/blob/f8ea3cd50f1a5724e8e715995bcde131c9c12b2c/packages/core/src/utils.ts#L157)

拟议实现采用显式 CallContext，而不修改共享服务对象上的可变 current_context 字段，也不只依赖 thread_local。调用跨线程或协程挂起后，调用方作用域必须继续有效且仍指向正确实例。服务内部解析自己的依赖，使用服务定义上下文；新建调用方资源时使用调用方作用域。

**建议的核心结构**

```text
C++ 宿主
  └─ PluginRuntime（C++ 核心，不强制启动 Python）
       ├─ Registry / DependencyGraph / LifecycleCoordinator
       ├─ ContextView / ResourceScope / ServiceHandle
       ├─ Events / TaskGroup / Executor adapters
       ├─ C++ plugin adapter
       └─ CPython adapter（pybind11 或经过验证的 nanobind 后端）
            └─ Python plugin facade / service adapters
```

两种语言必须共享同一份注册表、插件实例状态和资源归属记录。Python 包只是核心的接口与加载适配器，不应再独立实现另一套生命周期管理。

服务契约至少需要稳定的服务 ID、版本、方法签名、参数所有权、错误形式和执行位置。C++ 模板接口可提高编译期检查，Python 使用绑定后的契约或 Protocol／stub 辅助检查；Python 类型标注本身不足以保证跨语言兼容。

建议首版采用显式注册的类型契约：C++ 服务经绑定供 Python 调用；Python 实现经适配器供 C++ 调用。纯 C++ 调用保持原生路径，跨语言时才经过 Python 适配器。大数组支持明确寿命的 buffer，不必统一转成 JSON；并非任意 Python 对象都能自动变成 C++ 接口。

Context 隔离只影响服务解析。例如文档 A 和 B 可以各自拥有同名 scene 服务。首版仍可共享一个 CPython 主解释器；该机制不隔离 sys.modules、第三方包版本或原生崩溃。

**拟议 API 体验**

以下代码表达使用方式，尚不可运行。ScanService 契约需事先注册，示例服务声明在宿主线程执行。

```python
from dittobind import plugin
from host_api import ScanService

@plugin(requires=[ScanService])
def apply(ctx, config):
    scanner = ctx.use(ScanService)

    # 注册被 ctx 持有；丢弃返回值不会立即取消。
    ctx.on("document.changed", lambda doc: scanner.scan(doc))
    ctx.interval(1.0, lambda: scanner.poll())
```

```cpp
// 拟议接口：PluginScope 由运行时持有，不是 apply() 的栈变量。
void apply(PluginScope& ctx, const Config& config) {
    ctx.provide<ScanService>(std::make_shared<NativeScanner>(),
                             Execution::HostThread);
}
```

预期行为：Python 插件先加载时保持 Waiting；C++ 服务就绪后启动它；服务撤回时停止其受管注册，依赖恢复后创建新的激活代次。同一插件挂载到另一个文档 Context，会创建独立实例。

ResourceScope 是资源的主要持有者。`ctx.on()` 返回用于提前撤销的 SubscriptionId／控制句柄，不能默认设计成“临时返回值析构就取消”，否则 Python 和 C++ 的示例都会出现刚注册就撤销的问题。普通局部 RAII 可另设 API，并清楚区分。

**卸载协议是主要实现难点**

建议至少区分插件代码定义、配置实例、每次激活代次、单次调用四个寿命。

拟议状态：Waiting → Starting → Active → Stopping → Waiting／Stopped；启动失败进入 Failed，由显式更新或重试恢复。重启使用新的 generation，防止旧定时器或迟到任务把结果写回新实例。首版对必需依赖环报错，缺失依赖给出诊断。

停止过程应协调以下步骤：

1. 在生命周期协调器中标记停止，禁止新普通调用和注册进入该代次；撤回对新消费者的可见性。
2. 通知依赖它的实例停止，撤销事件和定时器，向受管任务发出协作取消。
3. 等待已获准的调用完成；依赖者清理期间仍需保留必要的服务存储和有限清理权限。
4. 在规定的 executor 上执行释放，逐步撤销服务与插件对象，并聚合清理错误。
5. 满足 Python 线程状态要求时释放受管 Python 引用；最后才允许解释器终止。

以上不是一个全局锁中的同步函数。不要持有注册表锁执行任意插件代码，不要在插件自身回调里同步等自己退出，也不要让 UI 线程阻塞等待一个需要 UI 回调才能结束的任务。

ServiceHandle 的 generation 检查必须和“取得在途调用租约”同步完成；只检查 active 后再取裸指针会存在竞争窗口。旧句柄应返回明确的服务失效错误，而不是指向已释放对象。shared_ptr 可以延长对象寿命，但不能单独表达服务已经退出，也不能自动保证插件代码所在动态库仍被加载。

异步关闭应有显式 `close_async()`／等价接口；C++ 析构函数不能承担任意异步等待。关闭超时意味着尚未完成卸载，应保留必要对象并报告阻塞来源，不能直接宣称成功。

**重载与资源撤销的保证范围**

| 情况 | 首版建议 |
| --- | --- |
| 停止／重新激活实例 | 支持：释放框架追踪的注册、任务和对象 |
| 修改配置 | 支持：检查配置后停止旧代次，再激活新代次；不承诺外部操作的事务回滚 |
| 更新 Python 业务插件代码 | 后续受限支持：先停止和排空，再加载新实现；限定模块组织方式和依赖范围 |
| 任意 Python 模块及原生扩展热重载 | 不作为通用保证 |
| 停用 C++ 插件 | 支持逻辑停用；默认保留 DLL／so 映射至进程结束 |
| C++ DLL／so 真正卸载 | 后续需同时排空线程、回调、对象、虚表、删除器及代码引用 |
| 卡死或崩溃后的强制恢复 | 独立 worker 方案；不由进程内 disposer 保证 |

Python 官方明确说明 reload 不更新外部旧引用，现有实例继续使用旧类，扩展模块重载也存在限制。因此先保证插件实例的启停，再定义代码更新范围。[importlib.reload](https://docs.python.org/3/library/importlib.html#importlib.reload)

effects 只代表框架能追踪、且资源实现提供撤销动作的注册。已经写出的业务数据、发送的请求、插件私自创建的线程，不会因为 dispose 自动回滚。Context 也不是运行不可信 Python 或 C++ 代码的沙箱。

**相邻方案和差异验证**

此前的嵌入库调研还不够覆盖这个新定位。CppMicroServices 已有基于 OSGi 思想的 C++ 服务注册、Bundle、跟踪和 Declarative Services；Python 的 pluggy 提供规范化 hook 机制。后续比较应加入它们，不能把动态服务或插件启停本身作为新颖性主张。[CppMicroServices](https://docs.cppmicroservices.org/en/stable/)、[pluggy](https://pluggy.readthedocs.io/en/stable/)

候选差异是把 Cordis 式资源归属与依赖联动，落到 C++／Python 双向服务、宿主线程和可诊断关闭上。是否值得独立建库，取决于相对于已有框架组合能减少多少集成负担。

**首个验证范围**

先使用一个 CPython 主解释器；生命周期变更通过串行协调器管理；提供宿主线程 executor。必需依赖、服务提供与消费、Context 子作用域、事件订阅、可取消定时器和资源撤销是核心。第一版不必包含包管理器、通用 IDL、多解释器和二进制热卸载。

首个示例应由 C++ 插件提供服务，Python 插件消费并注册回调，再由 Python 提供另一个服务供 C++ 消费。两种语言都必须走相同的状态机。重点验证：

- 消费者先到、提供者后到，能正确激活。
- 撤回服务时，依赖插件撤销注册；旧句柄拒绝新调用。
- 提供者清理不会早于依赖者的必要清理及在途调用完成。
- 启动中途失败能释放已登记资源，重复停止不会重复释放。
- 任务晚到与回调重入不会污染下一代实例。
- Python callback 引用在解释器终止前释放。
- 一份插件代码可在两个 Context 中使用不同服务，停用其中一个不影响另一个。

通过这些测试，才能具体证明这套方案在生命周期方面提供了超过基础绑定库的能力。
