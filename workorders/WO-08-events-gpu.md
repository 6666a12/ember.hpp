# WO-08 — 事件系统：GPU 实现（GL + Vulkan）

前置：WO-07 完成（契约、facade、常量、编码已定型）。本单让两端后端真正
执行事件子发射，`capabilities().events` 开 true。

## 目标

死亡/反弹粒子在同一帧内于 GPU 上触发子发射，双后端语义逐位对齐，
`cross_backend_test` 对拍通过。

## 必读

- `workorders/WO-07-events-contract.md`（契约与编码，已定型）
- `shaders/simulate.comp`（phase 0-3、retire/spawnFromRequest、RNG）
- `shaders/schedule.comp`（GPU 调度计数器重置）
- `src/backends/opengl/simulation.cpp`（update 分派链、同步读回时机）
- `src/backends/opengl/common.hpp`（binding 编号表）
- `src/backends/vulkan/simulation.cpp`、`resources.cpp`（缓冲/描述符/分派链）
- `tests/cross_backend_test.cpp`（对拍 harness；回收相位的多重集比较模式）
- `docs/backends.md` 跨后端一致性节

## 设计定型（不要偏离）

### 新缓冲（binding 编号两端一致，GL 用 glBindBufferBase，Vulkan 并入 set 0）

| binding | 内容 | 尺寸 |
|---|---|---|
| 20 | `eventTags[]`：逐槽位事件标签（uint） | capacity × 4B |
| 21 | `eventTemplates[]`：SpawnRequest 数组 | 64 × 176B |
| 22 | 事件队列：头部 `{uint head; uint totalChildren; uint pad0; uint pad1;}` + `GpuEvent[1024]` | 16B + 1024 × 48B |

`GpuEvent`（std430，48B）：`vec3 pos; uint templateIdx; vec3 vel;`
`uint parentSlot; uint base; uint pad0; uint pad1; uint pad2;`

tags 缓冲槽位寻址、单份不乒乓；resize 时随容量重建；clear 不需要清
（槽位被回收覆盖前不会被读——出生时才写标签，死亡时才读）。

### simulate.comp 改动

1. 声明三个新 buffer（binding 20/21/22）。队列头部用 `coherent`。
2. `spawnFromRequest`：出生写 `eventTags[slot] = r.pad2`（CPU 请求与事件
   子粒子同路径；模板的 pad2 自带链接 → 链式）。
3. `retire(uint i)` 增加事件入队：**在写尸体标记之前**读 `cur[i]` 的
   pos/vel 与 `eventTags[i]`；`onDeath = tag & 0xFFFFu`，非 0 则入队
   `{pos, vel, templateIdx = onDeath-1, parentSlot = i}`。
4. bounce 分支接触时按 `tag >> 16` 同样入队（onBounce）。
5. 入队代码（两处共用一个小函数）：
   `uint q = atomicAdd(head, 1u); if (q < maxEventInstancesPerFrame) events[q] = ...;`
   base 此时不写（下一相位统一前缀和）。
6. **新相位 uPhase==4（压缩，单 workgroup）**：thread 0 串行扫描 head 个
   实例，按模板 count 累加写回 `base`，预算（maxEventChildrenPerFrame）耗尽
   后的实例 base 写 `0xFFFFFFFFu`；末尾写 `totalChildren = 实际总量`。
   串行扫描 ≤1024 次加法，性能足够，优先正确性（注释注明可优化）。
7. phase 3 预留：`total = uSpawnTotal + queueHeader.totalChildren`，其余公式
   不变（复用/追加/`uSpawnAccepted` 对总账生效）。
8. phase 1 扩展：`j < uSpawnTotal` 走既有请求路径；否则
   `e = j - uSpawnTotal`，二分实例表（base 升序，sentinel 在最后）取
   `k = e - inst.base`，`r = eventTemplates[inst.templateIdx]`，
   `r.position = inst.pos` 覆盖模板位置；种子
   `s = uFrameSeed ^ (inst.parentSlot * 0x85EBCA6Bu) ^ (k * 0x9E3779B9u)`；
   采样公式与请求路径完全一致；`p.vel.xyz += inst.vel * uintBitsToFloat(r.pad3)`。

### schedule.comp 改动

- 声明 binding 22 队列头部；phase 0 重置 `head=0, totalChildren=0`
  （与 draw/request 计数器重置同处）。

### GL 后端

- `resources.cpp`：三个缓冲的分配/resize；`capabilities().events = true`；
  `uploadEventTemplates` 实现（存储模板数 + 上传；空列表清空模板数）。
- **旧自定义 shader 协议**：`uploadEventTemplates` 收到非空列表时，若当前
  simulate 程序是自定义来源且
  `glGetProgramResourceIndex(prog, GL_SHADER_STORAGE_BLOCK, "BufEventQueue")`
  为 `GL_INVALID_INDEX` → 抛 `std::invalid_argument`（措辞照既有协议拒绝）。
  内置 shader 自带该块，不受影响。模板数为 0 时一切行为与旧版逐位一致。
- `simulation.cpp` update()：绑定 20/21/22；模板激活时——
  - 同步模式：CPU 用 `subData` 清零队列头部 16B（紧挨既有计数器重置处）；
  - GPU 调度模式：schedule.comp 已重置，CPU 不动；
  - phase 0 后插入 phase 4：`glDispatchCompute(1,1,1)` + storage 屏障；
  - phase 1 分派上限：`total + (templatesActive ? maxEventChildrenPerFrame : 0)`
    （shader 内 `j >= uSpawnAccepted` 提前返回；模板激活时多发的线程是
    有意的最坏情形，避免帧中同步读回）。
- 统计语义不变：`uAlive` 在 phase 3 统一结算，同步读回仍是原 20 字节。

### Vulkan 后端

- 三个缓冲 + set 0 描述符布局增 20/21/22；**描述符池按门禁第 3 条重新核算
  并把算式写进注释**；帧资源（per-frame）与持久资源分清（门禁第 1 条）。
- 队列头部清零用 `vkCmdUpdateBuffer` 随命令流（transfer→shader 屏障），
  不放描述符更新进录制区（门禁第 7 条）。
- 分派链插入 phase 4（1,1,1）；phase 1 同样最坏情形上限。
- `capabilities().events = true`；`uploadEventTemplates` 实现。

### 测试

1. `tests/behavior.hpp` 增事件用例（双后端自动执行；用
   `backendCapabilities().events` 门控，false 则跳过）：烟花——母弹
   `lifeMin=lifeMax=0.2`、`onDeath` 挂 12 子粒子模板，更新到母弹死亡后断言
   `aliveCount == 12 × 母弹数`；onBounce 计数用例；模板清空后事件停止。
2. `tests/cross_backend_test.cpp` 增 `phaseEvents`（**unordered 模式**，
   有死亡）：母弹 burst + 二级链（子模板自带 onDeath → 孙粒子），
   30 帧逐帧对拍；断言多重集一致且出现孙粒子。
3. GL 侧（`tests/regressions.hpp` 或 system_test）：自定义 simulate shader
   （目录覆盖一个无事件块的最小改动版）+ 非空模板 → 拒绝用例。
4. `tests/vulkan_test.cpp`：capabilities 断言加 `events`。

### 构建卫生（门禁）

改了 `shaders/` 后必须依次 `python tools/sync_embedded_shaders.py` 和
`python tools/amalgamate.py`；SPIR-V 由构建目标自动重生成。
Vulkan 侧完工前 `EMBER_VK_DEBUG=1` 跑 `ember_vulkan_test.exe` 与
`ember_cross_backend_test.exe`，零 validation 输出。

## 验收

```bash
cmake --build build -j && cmake --build build-core -j
export PATH=/e/msys2/ucrt64/bin:$PATH
ctest --test-dir build --output-on-failure      # 全绿，含新用例
ctest --test-dir build-core --output-on-failure
EMBER_VK_DEBUG=1 VK_LAYER_PATH=/e/msys2/ucrt64/bin ./build/ember_vulkan_test.exe
EMBER_VK_DEBUG=1 VK_LAYER_PATH=/e/msys2/ucrt64/bin ./build/ember_cross_backend_test.exe
```

文档：`docs/backends.md`（事件语义入"后端必须保持的行为"、验证入口补
对拍相位）、`README.md`/`README.en.md` 特性节加事件条目、`config/example.ini`
补一个完整可跑的烟花事件配置。

## 边界

- 不做事件回调到 CPU、不做事件统计计数器（溢出静默丢弃，后续需要再加）。
- 子粒子不继承父粒子颜色/尺寸（模板全量采样）；inheritVelocity 是唯一继承通道。
- 每帧上限（1024 实例 / 4096 子粒子 / 64 模板）是定型值，不做动态扩容。
- 不改渲染路径、不改排序。

---

## 完成记录

- 日期：2026-09-18
- 环境：Windows + MSYS2 ucrt64（Vulkan 1.4 驱动，NVIDIA RTX 4060 Laptop）。

### 交付物

- `shaders/simulate.comp`：新增 binding 20/21/22（`eventTags[]`、`eventTemplates[]`、
  事件队列 + `GpuEvent` 48 B）；`spawnFromRequest` 统一到 `sampleParticle`/`spawnSlot`/
  `writeSpawned` 并写入逐槽位标签；`retire(i, emitDeath)` 在寿命/边界 kill 时入队
  onDeath（NaN 守卫与批量回收不入队）；bounce 接触时按高 16 位入队 onBounce；
  新增 phase 4 串行压缩/前缀和（预算耗尽写 `0xFFFFFFFF` 哨兵）；phase 3 用
  `uSpawnTotal + eventTotalChildren`；phase 1 按 `j < uSpawnTotal` 分流请求/事件子粒子；
  SimParams 复用 `pad0` 为 `uEventTemplates`（布局不变）。
- `shaders/schedule.comp`：binding 22，phase 0 重置队列头。
- GL 后端：三个事件缓冲（capacity×4 / 64×176B / 16B+1024×48B）与 binding 20/21/22；
  同步模式 CPU 清零队列头；phase 4 dispatch；phase 1 最坏情形上限；
  `uploadEventTemplates`（对无 `BufEventQueue` 的旧自定义 simulate shader 抛
  `invalid_argument`）；`capabilities().events = true`。
- Vulkan 后端：sim set 扩到 23 binding、描述符池按 `20*kFramesInFlight+4`
  重新核算（sim 14 + event 3 + render 3/帧）；队列头同步模式 `vkCmdUpdateBuffer`；
  phase 4；phase 1 上限；`uploadEventTemplates`（resize 重传模板）；`events = true`。
- 测试：`tests/behavior.hpp` 事件用例（onDeath 12×、清空后停止、onBounce）；
  `tests/cross_backend_test.cpp` 新增 `phaseEvents`（母→子→孙链，unordered 对拍，
  100% 位一致）；`tests/regressions.hpp` 旧自定义 simulate shader 拒绝用例；
  `tests/vulkan_test.cpp` capabilities 加 `events`。

### 验收输出摘要

```text
ctest --test-dir build --output-on-failure         # 11/11
ctest --test-dir build-core --output-on-failure     # 6/6
cross_backend_test phase events: 100.00% bit-identical
EMBER_VK_DEBUG=1 ember_vulkan_test / cross_backend_test：零 validation 输出
```

### 遗留

- 事件子粒子 RNG 种子含父槽位号；回收后槽位是排列等价的，故对拍相位用确定性模板
  （固定方向/速度/寿命）保证多重集逐位一致（WO-07 的“多重集确定性”在自由 RNG
  模板下不成立，这是实测结论）。

---

## 验收修复记录（2026-09-18，审查人：主模型）

交付时全绿，但深审与实证发现两个层面的问题：

1. **遗留声明属实并升级为设计修复**：验收先用自由 RNG 模板替换对拍相位的
   确定性模板，立即复现发散（pos.w |d|=5.9e-3）。根因：回收开始后槽位身份
   本身是排列等价的，第二代起"父槽位"不再确定 → 以槽位为种子的子粒子值
   跨后端/跨运行不一致。**修复**：引入出生谱系 id（CPU 出生
   `frameSeed ^ (j*phi)`，事件子代 `父id ^ (k*phi)`），与事件标签合并为
   逐槽位 `slotMeta` uvec2（binding 20，x=标签 y=出生 id；合并的另一原因：
   NVIDIA GL 每阶段 compute SSBO 块上限 16，原 shader 已满）。修复后自由
   RNG 模板对拍通过（93% 位一致，max |diff| 2.4e-7，纯编译器噪声）。
   WO-07 的设计定型已附修正注记，backends.md 契约同步。
2. **GL `scheduleGpu` 从不绑定 binding 22**：GPU 调度模式下队列头重置靠
   GL 绑定粘性 + 初始化清零兜底。已在 `scheduleGpu` 补一行绑定。

测试强化：phaseEvents 改自由 RNG 模板（速度/尺寸/寿命区间、调色板、
速度继承、链式）+ 第四条 GL GPU 调度腿 + 孙粒子颜色签名正向断言
（此前三腿可能同错同过）。

遗留修正：原"完成记录-遗留"一节关于"多重集确定性在自由 RNG 模板下不成
立"的结论被 birthId 修复推翻，对拍相位已是自由 RNG 模板。
config/example.ini 的烟花事件配置保持注释示例形态（解析与 GPU 行为均有
测试覆盖）；不在默认演示配置中启用事件，避免改变交互示例行为。

验收结论：通过（ctest 11/11、build-core 6/6、双 GPU 测试 validation 零输出）。
