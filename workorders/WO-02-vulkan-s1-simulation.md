# WO-02 — S1：仿真核心（缓冲 / 描述符 / phase 链 / 同步读回 / clear/resize）

前置：WO-01 完成（ember_vulkan 可构建，空后端存在）。

## 目标

实现 Vulkan 后端的 GPU 仿真完整路径：`update()` 的出生请求上传、phase 0/3/1/2
dispatch 链、缓冲乒乓、同步统计读回、`clear()`/`resize()`、`readParticles()`。
完成后 **behavior 套件在 Vulkan 后端全部通过**（该套件不含渲染）。

## 必读

- `src/backends/opengl/simulation.cpp`（逐行对照：上传、屏障、dispatch 结构）
- `src/backends/opengl/resources.cpp`（缓冲清单与初始值、resize/clear 语义）
- `src/backends/opengl/params.hpp`（SimParams 等 CPU 镜像，**直接复用**：
  vulkan 侧 `#include "../opengl/params.hpp"`，这些结构是纯数据无 GL 类型）
- `src/backends/opengl/common.hpp`（binding 编号、kMaxSpawnRequests、
  kSimGroupSize、nextPow2）
- `shaders/simulate.comp`（协议：binding 布局、counters 8×uint、phase 语义）
- `include/ember/backend.hpp`（参数借用语义、统计语义）
- `docs/backends.md`"后端必须保持的行为"全节
- `tests/behavior.hpp`（验收标准本身）

## 实现要求

### 1. 内存与缓冲（`resources.cpp`）

- 30 行级分配助手：`findMemoryType(typeBits, props)` + `createBuffer(size, usage, props)`
  返回 {buffer, memory} RAII 对。**不引 VMA**。
- 缓冲清单与 GL 完全一致（`particle_backend.hpp` 的 Buffer 列表对照）：
  粒子乒乓 A/B（capacity×64B，DEVICE_LOCAL）、spawn 请求（4096×176B，
  DEVICE_LOCAL + 每帧 staging）、palette/attractor/vortex/spring
  （HOST_VISIBLE|COHERENT，延迟上传见下）、dead 栈（capacity×4）、counters
  （8×uint=32B，DEVICE_LOCAL，初值 `{0,0,0,capacity,0,0,0,0}`）、live/nextLive
  （capacity×4 各一）、sorted（nextPow2(capacity)×4）、indirect（16B，初值
  `{4,0,0,0}`）、schedule（kScheduleBytes，GPU 调度时分配）、统计读回
  （HOST_VISIBLE|CACHED，5×uint）。
- UBO ×6（Sim/Sort/Schedule/Draw/Frag/Bloom，尺寸见 params.hpp），
  HOST_VISIBLE|COHERENT，**每帧 2 份**（frames-in-flight 轮换）。
- 每帧 staging：spawn 请求上传缓冲（704KB，HOST_VISIBLE|COHERENT，2 份轮换）。

### 2. 描述符（`resources.cpp`）

- set 0（持久 SSBO）：binding 0–13 全 STORAGE_BUFFER，一个 descriptor set，
  缓冲交换（乒乓 swap、resize、clear）后 `vkUpdateDescriptorSets` 更新
  （更新前已完成帧 fence 等待，见第 4 节同步模型）。
- set 1（每帧资源）：binding 14–19 UNIFORM_BUFFER，每 frame-in-flight 一份。
- pool 按上界预分配；layout 创建一次全后端共享。

### 3. update()（`simulation.cpp`）

逐条对照 GL 版翻译：

```
等待当前 frameIndex 的帧 fence 并重置（首次除外）
memcpy 当前帧 UBO(Sim)（phase 字段按 GL 版逐阶段改写，重录时重 memcpy）
dirty 的 palette/attractor/vortex/spring 经 staging 刷入
  （setter 只写 CPU 副本 + dirty 位——契约允许"返回前复制到后端拥有的存储"）
有出生请求：memcpy staging → vkCmdCopyBuffer → barrier(TRANSFER_WRITE→SHADER_READ)
非 GPU 模式：counters[2]=nr、indirect={4,0,0,0}（vkCmdUpdateBuffer 即可）
bind sim pipeline（simulate.comp.spv）+ set0/set1
dispatch phase 0（(alive+63)/64）→ barrier(COMPUTE SSBO 写→读)
有出生：dispatch phase 3（1×1×1）→ barrier
dispatch phase 1（(total+63)/64）→ barrier
dispatch phase 2（1×1×1）→ barrier(SSBO + INDIRECT_COMMAND_READ + TRANSFER_READ)
counters → 读回缓冲 vkCmdCopyBuffer
提交 + 帧 fence；随后 vkWaitForFences（同步模式默认）→ memcpy 读回 5 计数
交换粒子乒乓与 live 乒乓；frameCount_+1
```

- 屏障与 GL 的 `glMemoryBarrier` 一一对应，**位置照抄**（审查修复后的正确性
  已经验证，不要"优化"）。
- `alive_`/`allocated_` CPU 缓存语义照 GL（`countsFresh_` 机制）。
- debug 诊断（每 6 帧 stderr 输出计数）按 GL 版移植，可选。

### 4. 帧同步与破坏性操作

- frames-in-flight=2：每帧 {command buffer, fence, staging, UBO 描述}，
  录制前 `vkWaitForFences` 自己的槽位。
- `resize/clear/析构`：先 `vkQueueWaitIdle` 再操作（v1 有意从简，写注释说明）。
- 缓冲交换后更新 set 0 描述符前确保旧绑定无在飞使用（上面的 fence 已覆盖）。

### 5. validateConfiguration

查询并缓存 `VkPhysicalDeviceLimits`：`maxStorageBufferRange/64` 折算粒子上限、
`maxComputeWorkGroupCount[0]`（对照 GL 的 `validateGpuCapacity`）、
`maxDescriptorSetStorageBuffers ≥ 14`。超出抛 `invalid_argument`。

### 6. readParticles / 统计

- `readParticles`：帧 fence 已等（同步模式）→ 从 host-visible 读回复本按
  GL 版逻辑（扫 allocated，life.x≥0 过滤，槽位序）返回。需要 cur 粒子缓冲的
  读回：做一次 staging 拷贝 + waitIdle（此函数本就标注"工具/测试用途"）。
- `synchronizeStatistics/aliveCount`：精确（同步模式下即缓存值）。
- `pollStatistics`：同步模式直接返回当前统计；GPU 模式留 WO-04，先按
  同步路径实现，语义合法（契约允许样本滞后为零增长）。

### 7. 测试接入

`tests/vulkan_test.cpp`：device 可用时，在原骨架基础上加：

```cpp
behavior::run([](const ember::ParticleSettings& s) {
    auto b = ember::makeVulkanBackend();
    ember::setVulkanContext(*b, ctx);   // ctx 来自测试持有的 makeVulkanDevice
    return ember::ParticleSystem(s, std::move(b));
});
```

`render()` 仍是空操作——behavior 套件不渲染，合法。

## 验收

```bash
cmake --build build -j
export PATH=/e/msys2/ucrt64/bin:$PATH
./build/ember_vulkan_test.exe   # 含 "behavior suite (backend-agnostic): ALL PASSED"
ctest --test-dir build --output-on-failure   # 10/10，GL 九项不回归
```

## 边界

- 不做渲染、不做 GPU 调度（setGpuDriven(true) 继续抛不支持）、不做排序。
- 不引入新公共头改动；binding 常量重复定义在 vulkan/common.hpp（同 WO-01）。
- 屏障/计数器/乒乓语义只许照抄 GL，不许重新设计。

## 完成记录

（执行者填写）

## 完成记录

- 日期：2026-09-17
- 环境：Windows + MSYS2 ucrt64（Vulkan 1.4 驱动，NVIDIA RTX 4060 Laptop）。

### 交付物

- `src/backends/vulkan/resources.cpp`：`findMemoryType` / `makeBuffer`（单资源单
  VkDeviceMemory，无 VMA）、`writeBuffer`（host-visible 直写，device-local 走
  staging+queueWaitIdle）、缓冲清单与 GL 对齐（粒子乒乓、spawn、palette/attractor/
  vortex/spring、dead、counters 8×uint、live/nextLive、sorted、indirect、readback）、
  单个 descriptor set layout（binding 0–13 STORAGE、14–19 UNIFORM）、每帧 UBO ×6
  （2 槽轮换）、每帧 spawn staging、compute 管线（`simulate.comp.spv` 优先文件、
  内嵌兜底）、`initialize/resize/clear/validateConfiguration`。
- `src/backends/vulkan/simulation.cpp`：`update()` 的 phase 0/3/1/2 链、GL 对应的
  屏障位置、`vkCmdUpdateBuffer` 逐 phase 改写 SimParams.phase、非 GPU 模式
  counters[2]/indirect 初始化、计数读回拷贝、提交+fence 同步等待、乒乓交换与
  序号推进、debug 每 6 帧诊断。
- `src/backends/vulkan/statistics.cpp`：`refreshCounts`（同步模式缓存）、
  `pollStatistics`/`synchronizeStatistics`、`readParticles`（staging 拷贝+
  queueWaitIdle，按槽位序过滤 life.x≥0）。
- `tests/vulkan_test.cpp`：骨架断言 + `behavior::run` 全量语义套件。
- `tools/embed_spirv.py`：blob 数组加 `alignas(4)`（`pCode` 对齐要求）。

### 关键实现决策 / 偏差

- **描述符是单个 set**，不是工单描述的 set0(SSBO)/set1(UBO) 两个。原因：共享
  SPIR-V 的 GLSL 无显式 `set`，`spirv-dis` 实测 binding 0–19 全部
  `DescriptorSet 0`。拆两个 set 会与着色器不符。
- GL 的“每 phase 用 `glBufferSubData` 重写 UBO”在 Vulkan 单命令缓冲里无对应；
  改用 `vkCmdUpdateBuffer` 在命令流中改写 `phase` 字段，并配 TRANSFER↔COMPUTE
  屏障。
- `loadShader` 只接受 `shaderDir/<name>.spv`：早先还回退到 `shaderDir/<name>`
  （会误读同目录的 GLSL 源文本喂给 `vkCreateShaderModule`），已移除该回退。

### 验收输出摘要

```text
cmake --build build -j                                  # 无 error/warning
./build/ember_vulkan_test.exe
  behavior suite (backend-agnostic): ALL PASSED
  vulkan simulation: PASSED
ctest --test-dir build --output-on-failure              # 10/10，GL 九项不回归
ctest --test-dir build-core --output-on-failure         # 6/6
```

### 遗留

- `render()` 仍为空操作（WO-03）；`setGpuDriven`/`setSortEnabled` 继续抛不支持。
- `readbackBuf_` 为所有帧槽共用：当前同步模式每次 update 都等完 fence，无并发
  写入；WO-04 引入异步统计环时需改为每槽独立。
- 设备本地缓冲的 `writeBuffer` 走一次 staging + `vkQueueWaitIdle`，仅在
  initialize/resize/clear 等非热路径使用，未做批处理优化。
- 本机无 `VK_LAYER_KHRONOS_validation`，屏障正确性靠行为测试（年龄推进、回收、
  容量截断）间接验证，未做同步验证层端到端检查。



---

## 验收修复记录（2026-09-17，审查人：主模型）

验收时构建+测试基线即绿（ctest 10/10），但代码审查与 validation layer 发现以下
问题并已直接修复：

1. **`resize()` 后崩溃**：`destroyBuffers()` 连带销毁了 per-frame UBO/staging 且
   不重建。已拆分为 `destroyBuffers()`（持久缓冲）+ `destroyPerFrameBuffers()`
   （仅析构调用）。
2. **力场/调色板缓冲溢出**：palette/attractor/vortex/spring 固定分配 1 个元素，
   上传更大数组即越界写。已加 `ensureHostBuffer()` 按需扩容（在 `updateSimSet`
   之前完成，避免描述符悬挂）。
3. **描述符池欠配**：storage 描述符实际需要 (14+3)×2=34，只配了 32（规范允许
   驱动报错，NVIDIA 宽容未暴露）。已改为 `17*kFramesInFlight+4`。
4. **深度比较语义**：`LESS_OR_EQUAL` 改为 `LESS`，与 GL 默认 depthFunc 对齐。
5. **缺 WO-03 验收用例**：补上深度策略渲染用例（far 可见 / near 遮挡）。
6. **validation layer 规范违规**：`VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL`
   需要 separateDepthStencilLayouts 特性，1.1 基线下改用
   `VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL`。修复后 validation 全静默。
7. `setContext` 支持 initialize 之后的延迟初始化；`setVulkanSoftDepth`/
   `setVulkanRefractionInputs` 补上定义（存储成员，WO-06 接入描述符）；
   `tests/behavior.hpp` 新增 resize 用例（GL/Vulkan 双端覆盖第 1 条）；
   `EMBER_VK_DEBUG=1` 可开启测试端 validation。

验收结论：通过（修复后 ctest 10/10、build-core 6/6、validation 干净）。
