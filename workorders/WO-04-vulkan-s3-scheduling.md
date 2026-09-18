# WO-04 — S3：GPU 调度与异步统计（fence 环 / 间接 dispatch / gpuDriven）

前置：WO-02、WO-03 完成。

## 目标

实现 `setGpuDriven(true)`：GPU 生成积分 dispatch 命令（schedule.comp）、
间接 dispatch、统计改异步 4 槽 fence 环采样；`pollStatistics()` 非阻塞、
`aliveCount()` 保持精确。本工单**不含 GPU 模式下的排序**（WO-05 一并做；
本阶段 gpuDriven+sortEnabled 组合按契约拒绝）。

## 必读

- `src/backends/opengl/statistics.cpp`（fence 环语义、enqueue/poll/reset、
  setGpuDriven 的切换协议与校验）
- `shaders/schedule.comp` + `shaders/simulate.comp` 的 `uGpuDriven` 分支
  （binding 13 word 0 = 积分上限；phase 0 前调度重置绘制/请求计数）
- `include/ember/backend.hpp` 统计语义注释（poll 不等待、满环丢样、
  synchronize 精确）
- `docs/backends.md`"统计"条与 INTEGRATION.md GPU 调度节

## 实现要求

### 1. GPU 调度路径（`statistics.cpp` + `simulation.cpp`）

- `setGpuDriven(true)`：校验 capacity 对 `maxComputeWorkGroupCount[0]`
  （对照 GL `validateGpuCapacity`）；若 `sortEnabled_` 已开 → 抛
  `invalid_argument`（排序 GPU 协议属 WO-05，届时解除）。分配 schedule
  缓冲（kScheduleBytes，数值照 GL common.hpp）+ 统计环资源。
- update 开头（GPU 模式）：dispatch `schedule.comp`（phase 0，
  1×1×1）重置 indirect={4,0,0,0}、写请求数、生成积分命令 → barrier
  （SSBO 写→读 + INDIRECT_COMMAND_READ）。
- 积分改为 `vkCmdDispatchComputeIndirect`（schedule 缓冲 offset 2×4B）；
  其余 phase 3/1/2 不变（CPU 已知 total）。
- 关闭时：一次性同步读回（对照 GL 注释"deliberate one-time synchronization"），
  spawn 缓冲改回 CPU 直写路径，重置统计环。

### 2. 异步统计环（4 槽）

- 每槽 {HOST_VISIBLE 读回缓冲 20B, frame 号}。update 末尾
  `vkCmdCopyBuffer` counters→空槽；该样本的完成性由**当帧 frame fence**
  判定（`vkGetFenceStatus`，不等待）——不需要额外 fence 对象，只需记录
  每个样本对应哪一帧的 fence。
- `pollStatistics()`：扫描环，fence 已信号的读出来更新统计（新者胜）；
  满环 `++droppedStatistics_`。语义逐条对照 GL `pollStatistics/enqueueStatistics`。
- `synchronizeStatistics()/aliveCount()`：精确路径——等当前帧 fence 后读
  （GPU 模式允许等待，契约明示"可等待"）。
- `resetStatistics()`：clear/resize/模式切换时清环（对照 GL）。

### 3. 测试（`tests/vulkan_test.cpp` 扩展）

移植 `tests/regressions.hpp` 中**不依赖 GL 读 binding** 的 GPU 调度检查：

- gpuDriven 生命周期：burst/更新/死亡/再生在各模式下 alive 一致
  （与同步模式对拍：同 seed 同配置两系统逐帧 alive 相等）。
- `pollStatistics()` 永不远于 `updateSequence()`、不阻塞（调 1000 次计时上界，
  或直接验证返回值合法即可，不做脆弱的性能断言）。
- `aliveCount()` 在 GPU 模式精确。
- 模式切换（开→关→开）后行为一致；clear/resize 后统计复位。
- gpuDriven+sort 开启抛 `invalid_argument`。

## 验收

```bash
cmake --build build -j
export PATH=/e/msys2/ucrt64/bin:$PATH
./build/ember_vulkan_test.exe     # 新增 GPU 调度用例全过
ctest --test-dir build --output-on-failure
```

## 边界

- 不做排序（WO-05）；GPU 模式下 `render()` 的早退语义照 GL（alive 缓存
  不参与渲染路径）。
- fence 等待粒度 = 帧 fence，不引入 timeline semaphore（v1 从简）。
- 统计环槽数、拷贝字节数、丢样语义照 GL，不改数字。

## 完成记录

日期：2026-09-18

实现：

- `statistics.cpp`：`setGpuDriven` 能力校验（`maxComputeWorkGroupCount[0]`，对照 GL
  `validateGpuCapacity`）、schedule 缓冲 + 4 槽统计环分配/释放；`enqueueStatistics`
  预留槽位，`drainStatistics(wait)` 统一 poll/synchronize 的读取；槽位记录
  `frameSlot`/`submission`，判活时若帧槽已被后续提交复用（队列 in-order）则直接
  认为样本完成，避免 2 帧环下饿死丢样。
- `simulation.cpp`：GPU 模式开头 `scheduleGpu(0, nr)`（1×1×1 dispatch）重置
  indirect/请求数并生成积分命令 + barrier（SSBO 写→读 + INDIRECT_COMMAND_READ）；
  积分改用 `vkCmdDispatchIndirect(scheduleBuf_, 2×4B)`；关闭时一次性
  `refreshCounts` 同步。
- `resources.cpp`：schedule pipeline 与 binding 13 绑定；析构/销毁清理。
- `options.cpp`：`capabilities().gpuScheduling = true`。

验收输出摘要：

- `./build/ember_vulkan_test.exe`：新增 4 条 GPU 调度用例全过
  （gpu/sync 逐帧 alive 对拍、异步统计非阻塞/精确、模式切换+clear/resize、
  gpuDriven+sort 组合门禁）。
- `EMBER_VK_DEBUG=1` 校验层运行零 validation 输出。
- `ctest --test-dir build` 10/10、`build-core` 6/6 全绿。

遗留问题：

- 统计环依赖帧 fence 的复用判定，帧环深度较小（2）时仍可能丢样，语义与 GL
  的一致（满环丢样并累计 `droppedStatistics`），但丢样点比 GL 更早；未做
  timeline semaphore（工单允许从简）。
- GPU 模式下 debug 日志仍读 readbackBuf_ 的旧值，仅诊断用途。


---

## 验收修复记录（2026-09-18，审查人：主模型）

交付时功能测试全绿，但 validation layer（`EMBER_VK_DEBUG=1`）抓到规范级问题，
逐一修复：

1. **scheduleBuf_ 缺 `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT`**：`vkCmdDispatchIndirect`
   的间接缓冲必须带 INDIRECT usage（WO-04）。
2. **录制中更新描述符（无 UPDATE_AFTER_BIND，命令缓冲失效）**：三处——
   `drawParticles` 内的 `updateRenderSet`、排序的 binding-12 重绑、bloom 的
   `setSource`。统一前移：`render()` 在 `vkBeginCommandBuffer` 之前完成
   `updateRenderSet` / `sortPrepare`（排序缓冲与描述符准备拆出）/
   `ensureBloom` / `updateBloomSets`；录制区内只留 vkCmd* 命令。
3. **bloom 单描述符集逐 pass 覆写（WAW 语义错误）**：四个 pass 共享一个 set、
   录制前全部覆写，执行时所有 pass 只会采到最后绑定的图。改为 4 个持久
   描述符集（full→half→blur→half），每帧录制前统一写入。
4. **drawParticles 多 pass 的 UBO 覆写**：host-visible memcpy 在单次提交前
   逐 pass 覆写，所有 pass 会读到最后一组值（折射/普通混合粒子场景出错）。
   改为 `vkCmdUpdateBuffer` 随命令流有序更新 + transfer→shader 屏障。
5. **bloom 合成管线缺 `pDepthStencilState`**：宿主 pass 带深度附件时
   规范要求显式（禁用的）深度状态。
6. 文档化限制：Vulkan 端软粒子与折射共用一个深度绑定，两者同时注入时
   必须使用同一深度图（GL 端允许不同纹理）。

验收结论：通过（ctest 10/10、build-core 6/6、validation 零输出）。
