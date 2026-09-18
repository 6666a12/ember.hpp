# WO-07 — 事件系统（子发射）：公共契约与 facade

前置：无（不依赖 WO-01~06 之后的任何未落地工单）。本单只动公共层与契约，
两端后端在本单**不实现** GPU 行为——默认实现拒绝，能力声明保持 false。
GPU 落地在 WO-08。

## 目标

粒子死亡/反弹时在 GPU 上触发子发射（烟花二次炸开、落地溅射）。本单交付：

1. `Emitter` 的事件字段与 facade 的事件模板注册 API。
2. `SpawnRequest` 填充位（pad2/pad3）的事件语义定型。
3. `ParticleBackend` 新增事件模板上传虚函数 + `BackendCapabilities::events`。
4. INI 配置的 `[event]` 段与 `on_death=`/`on_bounce=` 引用。
5. `backend_contract_test` 记录型后端覆盖编码与拒绝语义。

## 必读

- `include/ember/emitters.hpp`（Emitter::makeRequest 编码、SpawnRequest 布局与
  static_assert）
- `include/ember/backend.hpp`（虚函数默认拒绝模式：`unsupported(...)`）
- `src/particle_system.cpp`（`registerEmitterPalette`/`synchronizePalettes`/
  `rebuildPaletteBuffer` 的脏标记+懒上传模式——事件模板照抄这个模式）
- `src/particle_system.cpp` 的 `apply(const Config&)`（paletteName 两阶段解析：
  先校验名字存在、再绑定——事件引用同名模式）
- `include/ember/config.hpp` 与 config 解析实现（EmitterConfig 的 name 字段）
- `tests/backend_contract_test.cpp`（记录型后端的既有断言风格）
- `docs/backends.md` "后端必须保持的行为"

## 设计定型（不要偏离）

### 语义

- 事件模板是"不连续发射的 Emitter"：注册进独立列表，只作为事件子发射的
  采样参数来源。模板自身的 `rate`/`position` 被忽略（位置取父粒子死亡点）。
- `onDeath` 触发时机：寿命到期、边界 kill。**不**触发：NaN 守卫回收、
  `clear()`、`resize()`（批量抹除不产生事件）。
- `onBounce` 触发时机：边界 bounce 接触（每次接触都触发）。
- 子粒子在**同一帧**出生（GPU 闭环，CPU 不参与），不占用 CPU 每帧出生预算；
  每帧事件实例上限 1024、子粒子上限 4096，溢出静默丢弃。
- 子粒子 RNG 种子 = f(frameSeed, 父槽位号, 子序号)——与事件入队的原子顺序
  无关，保证跨后端**多重集确定性**（对拍契约，见 backends.md 跨后端一致性节）。
- 模板可以带自己的事件链接 → 链式（烟花三级），按帧天然串行（子粒子最早
  下一帧才死亡），无需递归保护。
- 子粒子速度 = 模板采样速度 + 父粒子速度 × `inheritVelocity`。

### Emitter 新字段（`include/ember/emitters.hpp`）

```cpp
int onDeath = -1;              // 事件模板下标（addEventEmitter 返回值），-1 = 无
int onBounce = -1;             // 同上
// 以下两个字段只对"作为事件模板注册的 Emitter"有意义：
std::uint32_t eventCount = 8;  // 每次事件出生的子粒子数（[1, 64]）
float inheritVelocity = 0.f;   // 子粒子继承父粒子速度的比例（建议 [0,1]，只校验有限）
```

### facade API（`include/ember/system.hpp` + `src/particle_system.cpp`）

```cpp
// 注册事件模板，返回模板下标。模板按值拷入，之后修改不影响已注册内容。
std::size_t addEventEmitter(const Emitter& e);
std::size_t eventEmitterCount() const;
const Emitter* eventEmitter(std::size_t index) const; // 越界返回 nullptr
// 清空全部模板，同时把现存发射器的 onDeath/onBounce 置 -1（防止悬空引用）。
void clearEventEmitters();
```

校验：`eventCount` 裁剪到 [1,64]；`inheritVelocity` 非有限 →
`std::invalid_argument`；`addEmitter`/`apply` 时发射器的 onDeath/onBounce 越界
（>= eventEmitterCount 且 != -1）→ `std::invalid_argument`。

### 常量（`include/ember/particle_types.hpp`，`maxSpawnRequests` 旁边）

```cpp
inline constexpr std::uint32_t maxEventTemplates = 64;
inline constexpr std::uint32_t maxEventInstancesPerFrame = 1024;
inline constexpr std::uint32_t maxEventChildrenPerFrame = 4096;
```

### SpawnRequest 填充位语义（`emitters.hpp`，布局不变）

- `pad2` → **事件标签**：bits 0-15 = onDeath+1（0=无），bits 16-31 = onBounce+1。
  `makeRequest` 负责打包。出生 shader 把它写入逐槽位标签缓冲（WO-08）。
- `pad3` → **inheritVelocity 的 float 位**（仅事件模板请求使用；
  普通请求写 0）。用 `memcpy`/`bit_cast` 风格位转换，注释写明。
- static_assert(176B) 不变；字段注释更新，"Layout must match GLSL" 注释同步。

### 后端契约（`include/ember/backend.hpp`）

```cpp
struct BackendCapabilities {
    ...
    bool events = false; // GPU 事件（死亡/反弹触发子发射）
};
// 模板按 SpawnRequest 编码一次性上传（base/count 已填好，position 忽略）。
// 默认实现：非空列表抛 unsupported("event emission")。
virtual void uploadEventTemplates(const std::vector<SpawnRequest>& templates);
```

上传时机照调色板模式：facade 持 `eventTemplatesDirty_`，`update()` 开头
（`synchronizePalettes()` 旁边）调用 `synchronizeEventTemplates()`。
`uploadEventTemplates` 也在 `clearEventEmitters()` 后立即以空列表调用
（让后端释放资源）；默认实现对空列表不抛。

### INI 配置（`include/ember/config.hpp` + 解析实现 + `config/example.ini`）

- 新段 `[event <name>]`：键与 `[emitter]` 相同（形状/速度/寿命/尺寸/颜色/
  调色板引用/锥角等），外加 `count=`（eventCount）、`inherit=`（inheritVelocity）。
- `[emitter]` 新增 `on_death = <name>`、`on_bounce = <name>`。
- 解析两阶段（照 paletteName 模式）：先收集全部 `[event]` 名字，再解析引用，
  未知名字报 "unknown event '<name>'"。事件模板里的 on_death/on_bounce 允许
  引用其他事件（链式），同样两阶段。
- `config/example.ini` 加一个带注释的烟花示例段。

### 测试（`tests/backend_contract_test.cpp` 扩展）

记录型后端补 `uploadEventTemplates` 记录，断言：
- 注册模板 + 挂接发射器后，首次 `update()` 触发一次上传，内容编码正确
  （pad2 打包、模板 count、pad3 位值）。
- 无链接时不上传（脏标记只在有模板时置位——注册了模板就算脏，即使没人引用，
  上传无害；断言内容即可）。
- 默认后端（不覆写该虚函数）收到非空列表抛 `std::invalid_argument`，
  空列表不抛。
- onDeath 越界拒绝；`clearEventEmitters()` 清链接并触发空上传。
- INI：`[event]` 解析、`on_death` 引用、未知名字报错、链式引用。

## 验收

```bash
cmake --build build -j && cmake --build build-core -j
export PATH=/e/msys2/ucrt64/bin:$PATH
ctest --test-dir build --output-on-failure     # 11/11（含 cross_backend_test）
ctest --test-dir build-core --output-on-failure
python tools/amalgamate.py   # 改了 include/src 后必须
ctest --test-dir build --output-on-failure     # 单头同步后复跑
```

## 边界

- 不实现任何 GPU 行为（WO-08）；两端后端 `capabilities().events` 保持 false。
- 单头文件同步包含新 API（facade 是公共层，单头本来就有）。
- 事件不进入统计结构的语义变化：`aliveCount` 自然包含本帧事件子粒子。
- 不改 `Particle` 布局、不改既有 binding 编号、不动渲染路径。

---

## 完成记录

- 日期：2026-09-18
- 环境：Windows + MSYS2 ucrt64（Vulkan 1.4 驱动，NVIDIA RTX 4060 Laptop）。

### 交付物

- `include/ember/emitters.hpp`：`Emitter` 新增 `onDeath`/`onBounce`/`eventCount`/`inheritVelocity`；
  `makeRequest` 把链接打包进 `pad2`（低 16 位 = onDeath+1，高 16 位 = onBounce+1），
  把 `inheritVelocity` 的 float 位写进 `pad3`（布局不变，仍 176 B）。
- `include/ember/particle_types.hpp`：`maxEventTemplates=64`、
  `maxEventInstancesPerFrame=1024`、`maxEventChildrenPerFrame=4096`。
- `include/ember/backend.hpp`：`BackendCapabilities::events` 与
  `uploadEventTemplates`（默认非空列表抛 `unsupported("event emission")`，空列表不抛）。
- `include/ember/system.hpp` / `src/particle_system.cpp`：
  `addEventEmitter`/`eventEmitterCount`/`eventEmitter`/`clearEventEmitters`；
  `eventTemplatesDirty_` 懒上传（`update()` 开头 `synchronizeEventTemplates()`）；
  `addEmitter`/`apply` 校验 onDeath/onBounce 越界；
  `clearEventEmitters` 立即以空列表上传并解除全部链接；事件模板调色板并入共享调色板缓冲。
- `include/ember/config.hpp`：`[event "name"]` 段（键同 `[emitter]`，外加 `count`/`inherit`），
  `[emitter]`/`[event]` 的 `on_death`/`on_bounce`，两阶段名字解析（未知名字报
  `unknown event '<name>'`，支持链式）。
- `tests/backend_contract_test.cpp`：记录型后端 `uploadEventTemplates` + 编码/懒上传/
  越界/默认拒绝/INI 链式用例；`tests/config_test.cpp` 解析用例；`config/example.ini`
  增加带注释的烟花示例。

### 验收输出摘要

```text
ctest --test-dir build --output-on-failure        # 11/11
ctest --test-dir build-core --output-on-failure    # 6/6
```

### 遗留

- 本单两端后端 `capabilities().events` 保持 false（GPU 行为在 WO-08）；
  默认后端收到非空模板列表按契约抛 `std::invalid_argument`。

> 修正（验收，主模型）：第 41 行的种子设计"父槽位号"在验收中被证伪——回收
> 开始后槽位身份本身是排列等价的，第二代起子粒子值不再确定。已改为出生谱系
> id（CPU 出生 `frameSeed ^ (j*phi)`，事件子代 `父 id ^ (k*phi)`，逐槽位随
> 标签一并存储于 slotMeta uvec2 的 y 分量）。多重集确定性在自由随机模板下
> 成立，对拍相位已用自由随机模板验证。

---

## 验收修复记录（2026-09-18，审查人：主模型）

交付时 ctest 11/11 + build-core 6/6 + validation 零输出。四路并行深审
（契约/双后端接线/曲线/宿主 cmd）后修复：

1. **[event] 调色板引用未绑定未校验**：apply 的事件循环补
   `paletteName → palette` 绑定，phase 1 补 unknown-palette 校验
   （example.ini 烟花示例的 `palette = fire` 原本被静默丢弃）。
2. **模板 paletteIdx 过期**：`rebuildPaletteBuffer()` 重排调色板布局后未置
   `eventTemplatesDirty_`，GPU 端已编码模板按旧索引采样。已在
   `rebuildPaletteBuffer()` 内置 dirty，下次 update 重传。
3. **apply 原子性**：模板数上限（64）校验移入 phase 1，溢出在状态变更前抛。

补测试：onBounce 高 16 位打包断言、[event] 调色板绑定/未知名拒绝/65 模板
原子性、基类空上传不抛（原先走 recording 覆盖，测不到默认实现）。

验收结论：通过（ctest 11/11、build-core 6/6）。
