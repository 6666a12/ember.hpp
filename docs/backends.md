# 后端边界与接入路线

当前提供两个可运行后端：OpenGL 4.3（默认）与 Vulkan 1.1。公共层已独立，后续 DX、Metal 可以复用配置、发射器和帧请求准备逻辑，通过 `ParticleBackend` 分阶段实现功能。这里的接口是粒子系统操作接口，不是通用图形 API 包装。

## 模块与源码

| 模块 | 位置 | 职责 |
| --- | --- | --- |
| 数据协议 | `include/ember/core.hpp`、`emitters.hpp`、`particle_types.hpp` | 粒子/出生请求布局，力场、仿真、视觉、相机、统计参数 |
| 后端契约 | `include/ember/backend.hpp` | 初始化、上传、更新、绘制、读回、能力查询 |
| 公共系统 | `include/ember/system.hpp`、`src/particle_system.cpp` | 配置、发射器、颜色表变更检测、出生预算、前缀、时间和种子 |
| GL 入口 | `include/ember/opengl.hpp`、`src/backends/opengl/compat.cpp` | 工厂、原生纹理/程序适配、旧 API 兼容 |
| GL 对象 | `include/ember/backends/opengl/`、`src/backends/opengl/shader.cpp` | glad、Buffer/Texture/VAO RAII、着色器编译和 uniform 缓存 |
| GL 资源 | `src/backends/opengl/resources.cpp` | 分配、扩容、清空、上传、贴图和程序加载 |
| GL 仿真 | `src/backends/opengl/simulation.cpp` | 积分、批量出生、回收、存活列表、间接绘制参数和屏障 |
| GL 排序 | `src/backends/opengl/sort.cpp` | 深度键、分块 bitonic、旧排序协议兼容 |
| GL 绘制 | `src/backends/opengl/render.cpp` | billboard、宿主状态恢复、Bloom、软粒子、折射 |
| GL 调度和统计 | `src/backends/opengl/statistics.cpp` | GPU 调度切换、间接命令、异步统计环、精确读回 |
| GL 着色器 | `shaders/`、`src/backends/opengl/shaders.cpp`、`params.hpp` | 可编辑 GLSL 与生成的内嵌副本；std140 参数块的 CPU 镜像 |
| Vulkan 入口 | `include/ember/vulkan.hpp`、`src/backends/vulkan/context.cpp` | 工厂、自建设备 helper、宿主上下文注入 |
| Vulkan 模拟 | `src/backends/vulkan/resources.cpp`、`simulation.cpp`、`statistics.cpp` | 缓冲/描述符/管线、phase 链、GPU 调度与异步统计环 |
| Vulkan 排序 | `src/backends/vulkan/sort.cpp` | 分块 bitonic 深度键排序（同步与 GPU 调度两模式） |
| Vulkan 渲染 | `src/backends/vulkan/render.cpp`、`sprite.cpp`、`bloom.cpp` | 帧目标注入、render pass/管线缓存、billboard 间接绘制、贴图、软粒子、折射、bloom |
| Vulkan 宿主集成 | `src/backends/vulkan/context.cpp`、`options.cpp` | 上下文/帧目标/宿主纹理注入、能力选项、宿主 command buffer 录入 |
| SPIR-V 构建 | `tools/embed_spirv.py`、CMake `ember_spirv` | 构建期 GLSL→SPIR-V 与内嵌 |
| 可选窗口 | `include/ember/glfw_window.hpp`、`src/glfw_window.cpp` | GLFW 上下文与事件便利模块 |

`ParticleSystem` 不再持有 GL 资源或发出 GL 调用。它独占一个后端实例；移动系统仅转移所有权，后端地址及内部缓冲区引用保持稳定。移动后的源对象只能析构或重新赋值。

`shaders/` 保留原路径以兼容已有程序的加载路径。修改 GLSL 后运行 `python tools/sync_embedded_shaders.py`，再运行 `python tools/amalgamate.py`。Bloom 仍从外部文件加载；其加载失败时关闭 Bloom，保留普通粒子绘制。

## 构建和选择

| CMake 目标 | 依赖 | 用途 |
| --- | --- | --- |
| `ember_core` / `ember::core` | glm | 公共逻辑与自定义后端 |
| `ember` / `ember::ember` / `ember::opengl` | core + glad | 默认 GL 后端，保留旧目标名 |
| `ember_glfw` / `ember::ember_glfw` | OpenGL + GLFW | 示例和可选窗口模块 |
| `ember_vulkan` / `ember::vulkan` | core + Vulkan 加载器 | Vulkan 后端（`-DEMBER_BUILD_VULKAN=ON`，默认开） |

`ember_vulkan` 只在 `find_package(Vulkan)` 成功时构建；找不到 Vulkan 时配置打印一条 STATUS 并静默跳过整个模块（含测试），带 Vulkan 的消费方照常构建。当前功能与 GL 对齐：设备环境注入、缓冲与描述符、`simulate.comp` 的 phase 0/3/1/2 仿真链、`setGpuDriven` 的 schedule.comp 间接 dispatch 与 4 槽异步统计环、分块 bitonic 深度排序（同步/GPU 调度两模式）、`setVulkanFrameTarget` 注入宿主帧目标上的 billboard 间接绘制（render pass/管线变体缓存、负 viewport 翻 Y、GL 风格投影的深度重映射）、PNG 贴图与内置渐变、软粒子、折射、Bloom（RGBA16F 链 + 宿主深度遮挡）、事件子发射（死亡/反弹触发的 GPU 子发射）、生命周期曲线（颜色/尺寸 LUT）以及宿主 command buffer 录入。`capabilities()` 全开 true。`behavior` 语义套件、渲染像素用例、GPU 调度/排序/effects 用例在 Vulkan 后端全过。

描述符采用**单个 set**：共享 SPIR-V 的 GLSL 没有显式 `set` 限定，仿真/排序/调度着色器的 binding 0–19 都落在 set 0，因此绑定 0–13 的 SSBO、14–19 的 UBO 以及事件系统的 20–22 都落在 set 0，每帧按帧槽更新乒乓缓冲绑定。渲染着色器的绑定点在 Vulkan 下重排到同一 set 的非冲突槽位（顶点 0/1/2 + 曲线 8，片元 3/4/5/7），GL 编译仍走原有每阶段绑定（0/8/11/23 与 0/1/2/18）——两套编号由 `EMBER_CLIP_VULKAN` 宏切换，GL 行为不变。

只编译公共层（不会下载、生成或链接 glad/GLFW）：

```sh
cmake -S . -B build-core -DEMBER_BUILD_OPENGL=OFF
cmake --build build-core
ctest --test-dir build-core --output-on-failure
```

GL 后端使用宿主上下文时可设置 `-DEMBER_BUILD_GLFW=OFF`，此时不会构建需要窗口模块的示例、GL 测试；基准程序仍要求开启 GL 和 GLFW。两项选项默认 ON，原有构建命令继续有效。

构建期 SPIR-V：内置 GLSL 已满足 Vulkan 约束（无散 uniform、显式 binding/location、std140 参数块），默认 `-DEMBER_BUILD_SPIRV=ON` 时若找到 glslangValidator，`ember_spirv` 目标把 `shaders/` 全部编译到 `build/generated/spirv/`（`--target-env vulkan1.0`）。同一 GLSL 同时服务 GL（文本）与 Vulkan（二进制），SPIR-V 随源码构建始终同步；Vulkan 后端直接加载这些二进制，无运行时编译器依赖。后端仍向散 uniform 写相同数值，旧契约自定义 shader 不变。

安装包接受 `find_package(ember REQUIRED COMPONENTS core)`、`COMPONENTS opengl` 或 `COMPONENTS glfw`。不写组件时加载该安装中构建的全部模块。依赖由消费方提供已有 CMake 目标，或由包配置寻找已安装依赖；只选 core 不要求 GL 依赖。静态库手工链接时，GL 路径需要 `ember` **以及** `ember_core`，再链接 glad 和宿主窗口库。

现有代码继续使用：

```cpp
#include "ember/particle_system.hpp"
ember::ParticleSystem sys({100000, 30000}); // 默认 GL，需要已加载的当前 GL 上下文
```

显式选择并保留原生适配入口：

```cpp
#include "ember/system.hpp"
#include "ember/opengl.hpp"

auto backend = ember::makeOpenGLBackend(); // 此时即需要当前 GL 上下文
auto* glBackend = backend.get();           // 借用，不能比 sys 活得更久
ember::ParticleSystem sys({100000, 30000}, std::move(backend));
ember::setOpenGLSoftDepth(*glBackend, sceneDepthTexture);
sys.setSoftParticleParameters(true, 0.5f);
```

后端无关业务只 include `ember/system.hpp`，将自定义 `std::unique_ptr<ParticleBackend>` 注入构造函数，只链接 `ember::core`。公共头文件不包含 glad 或 GLFW。`particle_system.hpp`、`gl.hpp`、`gpu.hpp`、`shader.hpp` 保留兼容入口；GL 专属适配走 free function：`setOpenGLPrograms(backend, ...)`、`setOpenGLRefraction(sys, ...)`、`setOpenGLSoftParticles(sys, ...)`（原 `ParticleSystem::setPrograms/setRefraction/setSoftParticles` 成员已移除，公共 facade 不再出现 GL 类型）。此次保持源码使用兼容，类布局和静态库组成已变化，消费方需要重新编译。单头文件目前仍包含完整 GL 后端，纯公共层使用模块化构建。

## 后端必须保持的行为

- **生命周期与执行顺序**：系统独占后端，在构造时调用一次 `initialize`。同一个系统的调用由宿主串行化；后一次调用必须看到前一次的逻辑结果。执行可异步，后端负责上传数据、资源版本、可见性屏障和资源安全退役。
- **参数生命周期**：`UpdateBatch`、`SimulationParameters`、`RenderParameters`、颜色和力场数组都是调用期间借用。返回前必须复制到后端拥有的存储，或完成上传。不能把这些 CPU 引用留在尚未执行的命令中。
- **出生请求**：公共层将 `dt` 限制在 `[0, 0.1]`，拒绝负数和非有限值；先处理 burst，再按发射器顺序消耗每帧预算。每帧最多 4096 条请求，`base` 是出生数量前缀，`total` 是总请求量。GPU 容量不足的截断和空槽分配由后端完成。种子每次更新递增。
- **数据布局**：`Particle`、`SpawnRequest`、`Vortex`、`Spring` 的尺寸/偏移断言保留。新后端可以转换成自身 GPU 布局，但读回须返回同样的 `Particle` 语义。GL 的 binding 编号、计数器、间接命令布局仅属于 GL，见 `common.hpp` 和 GLSL。
- **配置和可选能力**：能力分别声明 GPU 调度、排序、Bloom、折射、软粒子、纹理加载、事件子发射、生命周期曲线。未实现功能默认关闭；开启时应明确拒绝。`validateConfiguration` 在修改配置前检查设备/容量/排序限制，不应清空粒子或改变选项。能力声明表示后端实现支持，具体着色器与设备限制仍可能导致开启失败。资源耗尽、贴图上传等执行错误不保证配置事务回滚。
- **清空/扩容**：清空粒子和旧统计样本，保留配置及更新序号；扩容额外改变容量。公共层 `clear()` 同时丢弃待处理 burst；不重置发射器累积量、时间或随机种子。
- **统计**：成功 `update()` 后序号增加一次，包括空帧和 `dt=0`。`pollStatistics()` 只返回已完成样本，不能等待未完成 GPU 工作；样本可以滞后，满环允许丢样并累计计数。`synchronizeStatistics()` / `aliveCount()` 是精确值，可等待。`readParticles(0)` 读全部存活粒子，其他值限制数量，按槽位顺序返回，用于验证和工具。宿主 command buffer 模式（WO-10）下，录进宿主 cmd 的样本只能被精确查询 drain，`pollStatistics()` 永不判定它们（不等待、不撒谎）。
- **事件子发射**：死亡（寿命到期、边界 kill）/ 反弹接触可在同一帧内于 GPU 触发子模板的批量出生；事件模板是"不连续发射的 Emitter"，位置取父粒子死亡点，子粒子速度 = 模板采样速度 + 父速度 × `inheritVelocity`，种子为 `f(父粒子出生谱系 id, 子序号)`（出生谱系 id：CPU 出生取 `frameSeed ^ 工作序号`，事件子代取 `父 id ^ 子序号哈希`，与槽位无关——槽位身份在回收后是排列等价的，谱系 id 保证任意代数的自由随机模板仍满足跨后端多重集确定性）。每帧上限 1024 实例 / 4096 子粒子 / 64 模板，溢出静默丢弃；`clearEventEmitters()` 清空链接后不再触发。
- **生命周期曲线**：系统级 color-over-life / size-over-life，facade 把用户 keys 烘焙为 64 项 LUT，shader 恒乘（关闭 = 全 1，逐位等价于未启用）；曲线只影响渲染，不进入模拟状态与对拍。
- **跨后端一致性**：同一指令流（相同设置、种子、发射器、burst、力场、dt 序列）驱动不同后端，粒子数量与统计必须逐帧精确相等，粒子内容按多重集在浮点容差内相等。槽位顺序只在无死亡时属于契约（纯追加出生是确定性整数逻辑）；一旦有粒子死亡，空槽栈的 `atomicAdd` 压栈顺序取决于 GPU 线程调度，槽位→粒子映射即使在同一后端的两次运行间也只是排列等价。数值上两后端编译同一 GLSL（GL 驱动文本编译 vs glslang SPIR-V），fma 收缩差异会经湍流噪声反馈放大，实测 30 帧全力量级约 5e-4，非混沌路径在 1e-6 量级。`cross_backend_test`（GL + Vulkan 同步 + Vulkan GPU 调度三条腿，回收相位另加一条 GL 腿佐证排列是同后端属性）逐帧执行该对拍。
- **原生资源**：GL 宿主颜色/深度纹理只借用、不删除；渲染完成前必须有效，不能与当前写入的目标形成反馈。非 GL 后端传入 GL 适配函数会被拒绝。所有 GL 操作及析构要求所属上下文 current。
- **坐标与深度约定**：传给 `render()` 的投影矩阵、以及软粒子/折射采样的宿主深度纹理遵循 **GL 约定**——NDC z ∈ [-1, 1]、帧缓冲原点左下、屏幕 UV 原点左下（片元着色器用 `uInvProj` 按此约定重建观察空间位置）。使用 [0,1] 深度或左上原点的宿主（Vulkan 常见，含 reversed-Z）必须自行在投影矩阵与深度导出中适配；内置 shader 已带 `EMBER_CLIP_VULKAN` 重映射，宿主可以继续传 GL 风格投影矩阵。

## Vulkan 的选择性接入顺序

内置 shader 已经是双 API 形态：GL 直接编译文本，SPIR-V 由构建产物提供（见上节），接入时不需要再改 GLSL。

1. ✅ 独立 Vulkan 工厂与原生上下文适配已就位（WO-01）：`makeVulkanBackend()` / `setVulkanContext()` 注入借用的 instance/device/queue，`makeVulkanDevice()` 提供自建设备 helper，构建期 SPIR-V 内嵌兜底；管线、帧资源、目标图像留待后续工单。
2. ✅ 基础 billboard 绘制已在 Vulkan 落地（WO-03）：`setVulkanFrameTarget` 注入借用目标，`render()` 录制自己的 command buffer，render pass/管线按混合与深度状态懒缓存，`vkCmdDrawIndirect` 消费 GPU 写出的绘制参数；负 viewport 翻 Y、GL 风格投影经 `EMBER_CLIP_VULKAN` 做深度重映射。出生、积分、回收、读回见 WO-02。
3. ✅ GPU 调度、间接 dispatch 与异步统计已落地（WO-04）：`setGpuDriven(true)` 后 `schedule.comp` 生成积分命令、`vkCmdDispatchIndirect` 消费，统计改 4 槽 fence 环异步采样；`pollStatistics()` 非阻塞，`synchronizeStatistics()`/`aliveCount()` 精确。
4. ✅ 排序（WO-05）与可选效果（WO-06）已落地：分块 bitonic 深度排序在同步与 GPU 调度两模式均可用（精确远→近），PNG 贴图、软粒子、折射、Bloom 全部接入，`capabilities()` 全开 true。
5. ✅ 事件子发射（WO-07/WO-08）与生命周期曲线（WO-09）已落地：`addEventEmitter`/`on_death`/`on_bounce` 在 GPU 上同帧触发子粒子（binding 20–22，phase 4 压缩），`cross_backend_test` 增加 `events` 相位；`setColorOverLife`/`setSizeOverLife` 由 facade 烘焙 64 项 LUT（乘法语义，空曲线为全 1）。
6. ✅ 宿主 command buffer 录入（WO-10）：`beginVulkanFrame`/`endVulkanFrame` 窗口内 `update()`/`render()` 录进宿主 cmd，后端不自提交、不 signal fence；窗口内精确统计/读回入口抛 `std::logic_error`，宿主提交并等待后再查询。

选择性接入是复用公共逻辑、选择后端、逐项实现能力。Vulkan 与 GL 是两个并列的可运行后端；目前没有同一粒子实例在 GL 仿真与 Vulkan 渲染之间共享资源的跨 API 互操作协议。

## Vulkan 渲染环境注入（设计定型）

GL 的 `render()` 依赖调用时刻的隐式环境（当前绑定的 FBO、viewport、深度/混合全局状态）。Vulkan 没有这些概念，因此渲染环境必须显式注入。在动手实现前约定如下，避免落地时回头改公共契约：

1. **适配入口形态**：与 `setOpenGLSoftDepth` 等 free function 同模式，不改动 `ParticleBackend` 契约。设备环境一次性注入：`setVulkanContext(backend, {instance, physicalDevice, device, queueFamilyIndex, queue})`；逐帧目标注入：`setVulkanFrameTarget(backend, {colorView, depthView, extent, colorFormat, frameIndex})`。宿主纹理/图像只借用不接管，与 GL 的所有权规则一致。
2. **第一版后端拥有提交**：`render()` 内部录制并提交自己的 command buffer（后端持有 command pool 与 `framesInFlight` 套帧资源，按 `frameIndex` 轮换），用 fence 等待帧完成，与 GL 的同步语义对齐。`pollStatistics()` 用 fence 非阻塞查询实现，契约不变。
3. **图像布局约定**：进入 `render()` 时颜色目标为 `COLOR_ATTACHMENT_OPTIMAL`、宿主深度（软粒子/折射用）为 `SHADER_READ_ONLY_OPTIMAL`；返回时布局不变。第一版由宿主负责布局迁移，后端不发 barrier——这条写进适配函数的文档注释，属于公开契约。
4. **录入宿主 command buffer 已落地（WO-10）**：`beginVulkanFrame(backend, cmd, frameIndex)` / `endVulkanFrame(backend)` 开启/关闭录入窗口，窗口内后端不自提交、不 signal fence；宿主遵守 `kFramesInFlight=2` 帧环与提交完成纪律，精确统计/读回前先提交并等待。详见 `include/ember/vulkan.hpp` 注释与 INTEGRATION。
5. **描述符布局**：SSBO binding 0–13 与 UBO binding 14–19 已在 GL 端错开命名空间，Vulkan 端单一 descriptor set 直接沿用相同编号；着色器直接用构建期 SPIR-V（`build/generated/spirv/`），无运行时编译器。

## 验证入口

`backend_contract_test` 只链接 core，使用记录型测试后端检查配置、发射预算、顺序、种子、颜色表、能力拒绝和移动行为。它不模拟粒子物理。`tests/behavior.hpp` 是后端无关语义套件——只通过公共 API 驱动（生命周期、容量/预算截断、统计序号、清空、dt 校验、发射累积），任何后端注入工厂即可运行；GL 在 `system_test` 与 `single_header_test` 中执行，Vulkan 后端落地时直接复用作为首批验收。`system_test` 与 `single_header_test` 共用 GL 行为回归，覆盖生命周期、回收、排序、旧着色器协议、GPU 调度、异步统计、Bloom、软粒子和折射。`benchmark_smoke` / `benchmark_gpu_smoke` 验证原有分阶段计时入口仍可运行；本次结构调整不宣称性能提升。`cross_backend_test` 是 GL 与 Vulkan 的逐帧对拍：同一指令流驱动三条腿（GL 同步、Vulkan 同步、Vulkan GPU 调度），每帧比较数量、统计与粒子内容（回收相位与 `events` 相位按多重集比较，见"跨后端一致性"），需要 GL 上下文与 Vulkan 设备同时可用，缺一跳过（77）。事件子发射由 `tests/behavior.hpp` 的双端门控用例覆盖；曲线烘焙契约在 `backend_contract_test`、INI 在 `config_test`、像素在 `effects.hpp`/`vulkan_test`；WO-10 的宿主 update/render 对拍、非法时序、混合模式与宿主 GPU 统计用例全部位于 `vulkan_test`。
