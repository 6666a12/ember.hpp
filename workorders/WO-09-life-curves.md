# WO-09 — 生命周期曲线：color-over-life / size-over-life（GL + Vulkan）

前置：无硬依赖。这是纯渲染路径增强，不碰模拟/调度/排序链路；假定 WO-06
的效果管线（bloom/软粒子/折射）已合入。

## 目标

系统级生命周期曲线：粒子颜色与尺寸随寿命比例 ageFrac 变化。facade 在 CPU
侧把用户 keys 重采样烘焙成 64 项 LUT，双后端经一个 SSBO 消费，语义为乘法；
空曲线（关闭）时渲染结果与现状逐位一致。

## 必读（已核实的代码事实，动手前通读）

### shaders

- `shaders/particle.vert:18-30` — `EMBER_CLIP_VULKAN`/GL 两分支的 binding 宏
  块。VULKAN 分支 `set = 0`：VS 用 binding 0/1/2（BufCur/BufSorted/BufLive，
  `:38-41`）+ 6（DrawParams UBO，std140，`:45`）。GL 分支（per-stage 命名空间）：
  VS 用 0/8/11 + 17。
- `shaders/particle.frag:25-40` — FS 与 VS 共用 VULKAN 的 set 0：binding 3/4/5
  是 sprite/depth/color 采样器，7 是 FragParams。**render set 0 的 binding 3
  已被 sprite 采样器占用，不是空闲号**；空闲号从 8 开始。GL 分支 FS 用
  0/1/2 + 18（采样器占纹理单元，不与 SSBO 冲突）。
- `shaders/particle.vert:88-97` — ageFrac 已有现成量：
  `vFade = 1.0 - clamp(p.vel.w / max(p.life.x, 1e-4), 0.0, 1.0)`（`:94`），
  故 `ageFrac = clamp(p.vel.w / max(p.life.x, 1e-4), 0.0, 1.0)`。
  `vColor = p.color`（`:93`）、`vFadeRGB = p.life.yzw`（`:95`）。
- `shaders/particle.vert:101-109` — 折射剔除读 `p.pos.w` 符号（`:101`，不可
  乘曲线）；`const float size = abs(p.pos.w)`（`:109`）——尺寸展开起点；
  `:122` `const float hs = 0.5 * size * uSizeScale` —— quad half-extent。
- `shaders/particle.frag:187-188` — 既有 fade 颜色：
  `vec3 rgb = mix(vFadeRGB, vColor.rgb, vFade)`。mix 对两个参数都是线性的，
  所以 `mix(a,b,f)*c == mix(a*c,b*c,f)` —— 在 VS 预乘 `vColor`/`vFadeRGB`
  与在 FS 乘插值结果**逐位等价**。这保证了"VS 采样"定型可以严格实现
  "最终颜色 = fade 插值结果 × colorLut"。
- `shaders/particle.vert:79-87` — `EMBER_DEBUG_FULLSCREEN` 早退路径输出字面
  量，不采样曲线；保持不动即可。
- `particle.frag` 两个分支都**不需要**加 BufCurves binding（FS 不引用，
  VS-only 采样定型）；只需改 `particle.vert`，但 VULKAN/GL 两个分支都要加
  同名宏 `EMBER_BIND_CURVES`（否则其中一分支编译不过）。

### 参数镜像与 binding 表

- `src/backends/opengl/params.hpp:16-82` — CPU 镜像模式：std140 UBO 结构 +
  `static_assert` 钉 `sizeof` 与关键 `offsetof`。注意此文件**全是 UBO
  （std140）镜像；BufCurves 是第一个 SSBO（std430）镜像**，照同样模式新增
  struct + static_assert。Vulkan render 复用此文件（`src/backends/vulkan/render.cpp:2`），
  镜像两端共用。
- `src/backends/opengl/common.hpp:16-40` — GL binding 编号表：SSBO 0-13 模拟、
  UBO 14-19（`:30-32` 注释说明 UBO 区避开 Vulkan 模拟 set 的 SSBO 区）。渲染
  VS SSBO 已用 0/8/11。23 在模拟区之外、无冲突。注意 GL 4.3 规范只保证
  `GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS >= 8`，索引 23 需要实现支持 ≥24
  （桌面驱动普遍满足，NVIDIA 报 96）；项目渲染路径已用 binding 11，已超出
  最小保证，23 与既有风险一致。
- `src/backends/opengl/render.cpp:249-259` — drawParticles 绑 UBO 与 SSBO
  （kBindingCur/Live/Sorted）的位置；新绑定加在 `:259` 旁。
- `src/backends/opengl/resources.cpp:12-33` — Buffer 构造初始化列表（ctor 里
  以 `GL_SHADER_STORAGE_BUFFER` 构造）；`:68-74` UBO `data()` 分配先例；
  `:190-210` upload* 系列实现风格。
- `src/backends/opengl/particle_backend.hpp:14` — capabilities 是**位置聚合初
  始化** `{true,true,true,true,true,true}`，加第 7 个字段后此处必须补一个
  `true`，否则 GL 端静默返回 `lifeCurves=false`。

### Vulkan 描述符路径

- `src/backends/vulkan/resources.cpp:155-196` — simSetLayout_（模拟 set 0，
  binding 0-19）与 renderSetLayout_。命名提醒：host 注释/成员把渲染集合称
  "set 1"（`vulkan_backend.hpp:14`、`:175`），但 shader 里 `EMBER_RENDER_SET`
  是 `set = 0`——因为 render pipeline layout 只含这一个 set
  （`:219-224`），绑定时 firstSet=0（`src/backends/vulkan/render.cpp:287`）。
  两套 pipeline layout 各自编号独立，shader 的 set=0 指 render layout。
- `src/backends/vulkan/resources.cpp:177-191` — `renderBindings[8]`：
  0/1/2 = SSBO(VERTEX)，3/4/5 = COMBINED_IMAGE_SAMPLER(FRAGMENT)，6/7 =
  UBO。**新增 binding 8（SSBO, VERTEX）后数组扩到 9、bindingCount 改 9**。
- `src/backends/vulkan/resources.cpp:199-211` — 描述符池现算式：
  SSBO `17 * kFramesInFlight + 4`（注释 "sim 14 + render 3 per frame"）、
  UBO `8 * kFramesInFlight + 4`、sampler `8 * kFramesInFlight + 4`、
  maxSets `4 * kFramesInFlight + 4`。改后按门禁第 3 条把新算式
  `((14 + 4) * kFramesInFlight + 4)` 与推导写进注释；maxSets 不变
  （set 个数没变）。
- `src/backends/vulkan/resources.cpp:417-475` — updateRenderSet：`buffers[4]` +
  8 条 write；加 binding 8 后 `buffers[5]`、writes 数组 8→9、末尾
  `vkUpdateDescriptorSets(..., 9, ...)`。bloom 走独立 pool/layout
  （`bloom.cpp`），不受影响。
- `src/backends/vulkan/resources.cpp:107-132,321-366` — BufCurves 容量固定
  1280B、与粒子容量无关 → **持久缓冲**（进 destroyBuffers 清单，
  `:111-113`；destroyPerFrameBuffers 不动，门禁第 1 条"帧资源与持久资源分
  清"）。分配照 paletteBuf_ 先例（`:338`，定长 host-visible）。注意
  `resize()`（`:556-567`）走 destroyBuffers+allocateBuffers，会重建 BufCurves
  → 后端须持 CPU 副本、重建后重传（见实现要求）。

### 公共层 / facade / INI

- `include/ember/backend.hpp:10-19` — BackendCapabilities 6 个 bool 全默认
  false；**新字段必须加在尾部**保持源兼容。`:59-61` `unsupported()` 措辞
  `"ember: backend does not support <feature>"`。`:39` 借用契约注释：
  "Borrowed inputs are valid only during the call. Copy/upload before return."
- `include/ember/particle_types.hpp:72-85` — RenderParameters 现状；本工单
  **不改它**（理由见设计定型第 4 条）。
- `src/particle_system.cpp:112-239` — apply() 两段式（Phase 1 全量校验、
  Phase 2 才改状态）；`:381-392` setRefractionParameters 的
  capability 拒绝模式（enabled && !caps → throw invalid_argument）；
  `:327-330` facade 存状态 + 立即调 backend upload 的模式。
- `include/ember/config.hpp:121-328` — 自包含 INI parser：Section enum
  （`:127`）、段分发（`:145-186`，未知段 fail）、key 分发（`:207-325`）、
  `fail` 带行号（`:132-134`）；palette 多值用 `|` 分隔 vec4（`:256-263`）；
  空 value 一律拒绝、仅 `system.texture` 例外（`:193-195`）。
- `include/ember/vulkan.hpp` / `include/ember/opengl.hpp` — Vulkan 无自定义
  render shader 入口（模块固定），渲染宽容路径只与 GL 自定义 program 有关。
- `tests/regressions.hpp:18,35,43,80` — GL 像素测试工具：`require`、`burst()`、
  `Target`、`sum(pixels, channel)`；逐位相等断言惯用例
  `tests/effects.hpp:138`（`target.pixels()==reference`）。
- `tests/vulkan_test.cpp:34-274` — OffscreenTarget/sumChannel/centeredBurst；
  capabilities 断言在 `:896-898`。
- `tests/backend_contract_test.cpp:18-61` — RecordingBackend（capabilities
  `return {}`）+ `rejects(...)` 惯用法：验证不支持特性的拒绝路径。
- `tests/config_test.cpp` — 无 GL 的 parser 测试（CHECK/CHECK_THROWS）。

## 设计定型（不要偏离）

1. **系统级（v1），facade API**（`include/ember/system.hpp`）：
   `void setColorOverLife(std::vector<glm::vec4> keys)`、
   `void setSizeOverLife(std::vector<float> keys)`，及对应 getter
   （返回当前存储的 keys）。空 vector = 关闭该通道（现状行为）。
2. **keys 均匀分布**：程序化 API 的 keys 不带 t，key i 位于
   t_i = i/(n-1)（n ≥ 2）；n == 1 为常数曲线。烘焙：LUT[i] = eval(i/63)，
   eval 为 keys 分段线性、端点外钳制到首/末 key 值。INI 的显式 t 键在
   apply 时先重采样成 64 项均匀 keys（eval(j/63)，j = 0..63），再走同一路径。
3. **facade 烘焙 64 项 LUT**，后端只收 LUT，shader 不做变长 keys 插值。
4. **乘法语义，shader 恒乘，不改任何参数块**：
   - 最终尺寸 = `pos.w × sizeLut[idx]`；最终颜色 = 既有 fade 插值结果 ×
     `colorLut[idx]`（VS 中对 `vColor` 与 `vFadeRGB` 同时预乘，与 FS 乘插值
     结果逐位等价——见必读 mix 线性说明）。
   - **关闭 = 上传全 1 LUT**（不是 shader 分支、不是 DrawParams 标志位）。
     IEEE 754 下 `x * 1.0f == x` 逐位恒等，因此关闭时与现状逐位一致。
   - 结论：`RenderParameters`、`DrawParams`、`FragParams` 一律不加字段；
     `cross_backend_test` 不改（曲线不进模拟状态）。
   - LUT 采样：`idx = min(uint(ageFrac * 63.0 + 0.5), 63u)`（就近取整，
     两端各自实现必须用同一公式保证像素对拍）。
5. **GPU 数据**：一个 SSBO `BufCurves { vec4 colorLut[64]; float sizeLut[64]; }`
   （std430，1280B：colorLut 占 `[0,1024)`，sizeLut 偏移 1024，sizeof 对齐
   16 → 1280）。binding：**GL SSBO binding 23**（`common.hpp` 加
   `kBindingCurves = 23`）；**Vulkan render set 0 的 binding 8**（3 已被
   sprite 占用，已核实）。GL 端 `particle.vert` 的 VULKAN 与 GL 两个分支都
   加 `#define EMBER_BIND_CURVES`（8 / 23）；`particle.frag` 不加。
6. **后端契约**（`include/ember/backend.hpp` + `particle_types.hpp`）：
   - `BackendCapabilities` 尾部加 `bool lifeCurves = false;`。
   - `particle_types.hpp` 加
     `inline constexpr std::uint32_t kLifeCurveLutSize = 64;` 和
     ```cpp
     struct LifeCurvesLut {                 // 前 1280B 即 BufCurves 的 std430 镜像
         glm::vec4 color[kLifeCurveLutSize];
         float     size[kLifeCurveLutSize];
         std::uint32_t mask = 0;            // bit0 = color 启用, bit1 = size 启用
     };                                     // mask 只走 CPU，不进 SSBO
     ```
   - `ParticleBackend` 加：
     ```cpp
     virtual void uploadLifeCurves(const LifeCurvesLut& lut) {
         if (lut.mask != 0) unsupported("life curves");
     }
     ```
     （纯虚会破坏第三方后端；带默认实现的后端不实现即声明不支持。）
   - `src/backends/opengl/params.hpp` 加 std430 镜像
     `struct CurvesParams { glm::vec4 colorLut[64]; float sizeLut[64]; };` +
     `static_assert(sizeof(CurvesParams) == 1280)` 与
     `static_assert(offsetof(CurvesParams, sizeLut) == 1024)`。GL `subData` 与
     Vulkan `writeBuffer` 都以此 struct 为字节源（上传时丢弃 mask）。
7. **INI**：`[curves]` 段，键 `size` / `color`：
   ```
   [curves]
   size  = 0:1, 0.5:1.2, 1:0            # t:值（size 每键 1 个分量）
   color = 0:1,1,1,1, 1:1,0,0,0         # t:r,g,b,a（color 每键 4 个分量）
   ```
   解析规则（照抄 `config.hpp` 的段/key 分发模式）：按 `,` split；**含 `:`
   的 token 开启新键**（`:` 前为 t，其后为本键第一个分量）；其后不含 `:`
   的 token 追加为分量，直到下一个含 `:` 的 token（size 每键恰 1 个分量、
   color 恰 4 个，多了少了都 `fail`）。校验：t 必须在 `[0,1]` 且严格递增、
   分量必须有限。存储为显式 t 对：`colorKeys: std::vector<glm::vec4>`（x=t）、
   `sizeKeys: std::vector<glm::vec4>`（x=t，y=size）。另外允许**显式空值**
   `size =` / `color =`（仿 `system.texture` 的 `:194` 例外）= 关闭该通道。
   `Config` 记录 `specified` 键 `curves.size` / `curves.color`；apply() 只动
   配置里出现的键（不出现的通道保留程序化值）。
8. **旧自定义 render shader 协议（宽容路径）**：GL 端 drawParticles **无条
   件** `glBindBufferBase(..., kBindingCurves, ...)`；自定义 render program
   未声明 BufCurves 时曲线静默不生效——**不抛错、不做
   `glGetProgramResourceIndex` 检查**（那是 simulate 严格协议的做法，渲染
   路径保持宽容）。文档注明。Vulkan 无自定义 render shader，天然不涉及。
9. **facade 的 capability 拒绝**：setColorOverLife/setSizeOverLife 收到非空
   keys 且 `!capabilities().lifeCurves` → 抛 `std::invalid_argument`
   （措辞与既有 setter 一致："ember: backend does not support life curves"）；
   空 keys 任何时候都允许（关曲线不需要能力）。INI 同理在 apply() 的
   Phase 1 检查（保持原子性）。

## 实现要求

### shaders/particle.vert（particle.frag 不动）

两个分支的宏块都加 `#define EMBER_BIND_CURVES`（VULKAN=8，GL=23），声明：

```glsl
layout(EMBER_RENDER_SET binding = EMBER_BIND_CURVES, std430) readonly buffer BufCurves {
    vec4 colorLut[64];
    float sizeLut[64];
};
```

main() 内（紧接 `:94` 的 vFade 计算后）：

```glsl
const float ageFrac = clamp(p.vel.w / max(p.life.x, 1e-4), 0.0, 1.0);
const uint  lutIdx  = min(uint(ageFrac * 63.0 + 0.5), 63u);
const vec4  colorMul = colorLut[lutIdx];   // 曲线关闭时恒为 vec4(1.0)
const float sizeMul  = sizeLut[lutIdx];    // 曲线关闭时恒为 1.0
```

随后：`:93` `vColor = p.color * colorMul;`；`:95`
`vFadeRGB = p.life.yzw * colorMul.rgb;`；`:109`
`const float size = abs(p.pos.w) * sizeMul;`（`:101` 的折射符号判断仍读原始
`p.pos.w`，不可乘）。早退的 debug/fullscreen/折射剔除路径保持字面行为。

### 公共层

- `particle_types.hpp`：`kLifeCurveLutSize` + `LifeCurvesLut`（见定型第 6 条）。
- `backend.hpp`：`BackendCapabilities` 尾部 `bool lifeCurves = false;`；
  `ParticleBackend::uploadLifeCurves` 默认实现（见定型第 6 条）。
- `system.hpp` / `particle_system.cpp`：
  - facade 新增私有存储：`std::vector<glm::vec4> colorOverLifeKeys_,
    sizeOverLifeKeys_;`（均匀 keys；getter 返回它们）。
  - setter：非空 + `!backend_->capabilities().lifeCurves` → 抛
    invalid_argument（Phase 校验式，见定型第 9 条）；校验每个分量有限；换算
    均匀 t 后存储，并烘焙 `LifeCurvesLut` 调 `backend_->uploadLifeCurves(...)`。
    空 vector → mask 对应位清 0、LUT 对应半区填 1.0f，同样上传（保证关闭后
    GPU 态为全 1）。
  - 烘焙（两端共用同一函数，像素对拍的前提）：
    ```cpp
    // uniform keys（n≥2，t_i = i/(n-1)）或 n==1 常数；eval 分段线性、端点钳制
    for (int i = 0; i < 64; ++i) lut.color[i] = evalColor(i / 63.0f);
    ```
  - apply()：`if (cfg.has("curves.size")) ...`、`if (cfg.has("curves.color")) ...`
    （INI 显式 t 先重采样为 64 项均匀 keys，见定型第 2 条）；Phase 1 增加
    capability 校验（保持 loadConfig 原子性）。

### GL 后端

- `common.hpp`：`inline constexpr GLuint kBindingCurves = 23;`（SSBO 区，
  避开模拟 0-13 与 UBO 14-19）。
- `params.hpp`：`CurvesParams` + static_asserts（见定型第 6 条）。
- `particle_backend.hpp`：capabilities 补第 7 个 `true`（位置聚合初始化，
  见必读）；声明 `void uploadLifeCurves(const LifeCurvesLut&) override;`；
  成员 `Buffer curvesBuf_;`。
- `resources.cpp`：ctor 初始化列表加 `curvesBuf_(GL_SHADER_STORAGE_BUFFER)`；
  `initialize()` 分配 `sizeof(CurvesParams)` 并预填全 1（默认态 = 关闭，
  GL 的 `resize()`/`clear()` 不重建 UBO/定长缓冲，无需重传——对照
  `resize()` 不碰 ubo* 的既有行为）。
- `render.cpp` drawParticles：`:259` 旁加
  `glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingCurves, curvesBuf_.id());`
  **无条件**（宽容路径，见定型第 8 条）。
- `uploadLifeCurves` 实现：`CurvesParams cp; memcpy(cp.color, lut.color,
  sizeof cp.color); memcpy(cp.size, lut.size, sizeof cp.size);`
  `curvesBuf_.subData(0, sizeof(cp), &cp);`（mask 不上传）。

### Vulkan 后端

- `vulkan_backend.hpp`：声明 override；持久成员 `Buffer curvesBuf_;` + CPU
  副本 `LifeCurvesLut curvesLut_{};`。
- `options.cpp` capabilities()：`caps.lifeCurves = true;`。
- `resources.cpp`：
  - allocateBuffers 里建 `curvesBuf_ = makeBuffer(sizeof(opengl::CurvesParams),
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, host)`
    （照 paletteBuf_ 先例）；CPU 副本 `curvesLut_` 写入缓冲（默认全 1、
    mask 0）。
  - destroyBuffers 清单加 `&curvesBuf_`。
  - **resize 保活**：allocateBuffers 末尾（或调用点）若 `curvesLut_.mask`
    非 0（其实无条件更简单：副本总在）用 `writeBuffer(curvesBuf_, 0,
    sizeof(opengl::CurvesParams), &curvesLut_)` 重传。
  - renderSetLayout_：`renderBindings[8]` 扩 `[9]`，`addRender(8,
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT)`，
    bindingCount 8→9。
  - 描述符池（门禁第 3 条）：SSBO 算式改
    `(14 + 4) * kFramesInFlight + 4` 并把推导写进注释：
    `// sim set 14 SSBO + render set 4 SSBO（0/1/2 + curves 8）per frame`。
    maxSets、UBO、sampler 算式不变。
  - updateRenderSet：`buffers[4]`→`[5]`（新项 `buffers[4].buffer =
    curvesBuf_.buffer; range = VK_WHOLE_SIZE;`），writes 8→9 条，加
    `bufferWrite(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &buffers[4]);`，
    末尾 count 改 9。
- `uploadLifeCurves` 实现：存 `curvesLut_ = lut;`，随后
  `writeBuffer(curvesBuf_, 0, sizeof(opengl::CurvesParams), &curvesLut_);`
  （`writeBuffer` 已处理映射回写，见 `resources.cpp:48-60`）。
- 新 render pipeline layout 与 SPIR-V 必须严格匹配（set 0 binding 8、SSBO、
  VERTEX），validation 门禁会抓到任何漂移。

### INI / 示例 / 文档

- `config.hpp`：Section enum 加 `Curves`；段分发加 `sec == "curves"`；key
  分发加 `size`/`color`（解析算法与校验见定型第 7 条）；`:193-195` 的空值
  例外加 curves 两键；secName 表（`:198-204`）加 `"curves"` 前缀。
- `config/example.ini`：加注释示例段（两键各一行 + 说明显式空值关闭）。
- 文档：`docs/backends.md`（"后端必须保持的行为"补乘法曲线语义与宽容路径
  说明、"验证入口"补新用例）、`README.md`/`README.en.md` 特性节加条目。

## 测试

1. **烘焙/契约（`tests/backend_contract_test.cpp`，无 GL）**：RecordingBackend
   增加记录 `LifeCurvesLut` 的 `uploadLifeCurves` override + 开关
   `lifeCurves` 的 capabilities。断言：caps 关闭时 `rejects([&]{
   sys.setColorOverLife({{1,1,1,1},{1,0,0,0}}); })` 与
   `setSizeOverLife({1.f, 0.f})`；空 vector 不抛且 getter 为空；caps 开启
   时收到烘焙 LUT——如 size keys {1, 0}：mask bit1 置位、`size[0]==1`、
   `size[63]==0`、`size[31]` 约 0.5；color 同理；再 set 空验证 LUT 回到 1。
   再补 apply 路径：`rejects([&]{ sys.apply(Config::fromString(
   "[curves]\nsize=0:1, 1:0\n")); })`（caps 关闭，原子性）＋
   `[curves]` 空值关闭用例。
2. **INI parser（`tests/config_test.cpp`，无 GL）**：`[curves]` 正常解析
   （size 单调 t 列表、color 四分量）、显式空值 = 空 keys、坏 t（越界/非递
   增/分量数错/非数字）按行号抛 runtime_error。
3. **GL 像素（`tests/effects.hpp`，仿 "streak and color fade"
   `:118-129`）**，64×64 Target、`useSprite(false)`、静止单粒子
   （speed 0、pos (0,0,-2)）：
   - **size 单调缩小**：size=1、life=1；基线 draw；`setSizeOverLife({1, 0})`
     后按 0.1s 步进多帧 draw，断言红通道和（覆盖度代理）逐帧不增、且明显
     小于基线；粒子死亡后为零。
   - **color 曲线**：`setColorOverLife({{1,1,1,1},{1,0,0,0}})`（末端红通
     道以外的通道归零）或等价构造使某通道末端归零；断言末帧该通道和显著
     下降/归零，而基线帧非零。
   - **空曲线逐位退回**：设置曲线再 `setSizeOverLife({})` +
     `setColorOverLife({})`，draw 与从未设置的基线 `==`（vector 逐位相等，
     惯用例 `:138`）。
4. **Vulkan 像素（`tests/vulkan_test.cpp`）**：仿 softParticleFade
   （`:558-594`）用 OffscreenTarget + centeredBurst + sumChannel 复刻上述
   三条；`:896-898` capabilities 断言加 `caps.lifeCurves`。
5. `cross_backend_test` **不改**（渲染时效果，不进模拟状态）。

## 构建卫生（门禁）

改了 `shaders/` 后必须依次 `python tools/sync_embedded_shaders.py` 和
`python tools/amalgamate.py`（公共头变了，单头文件也要重生成）；SPIR-V 由
构建目标 ember_spirv 自动重生成。Vulkan 侧完工前 `EMBER_VK_DEBUG=1` 跑
`ember_vulkan_test.exe`，零 validation 输出。

## 验收

```bash
cmake --build build -j && cmake --build build-core -j
export PATH=/e/msys2/ucrt64/bin:$PATH
ctest --test-dir build --output-on-failure      # 全绿，含新用例
ctest --test-dir build-core --output-on-failure
EMBER_VK_DEBUG=1 VK_LAYER_PATH=/e/msys2/ucrt64/bin ./build/ember_vulkan_test.exe
python tools/sync_embedded_shaders.py
python tools/amalgamate.py
cmake --build build -j && cmake --build build-core -j
ctest --test-dir build --output-on-failure      # 复跑，仍全绿
ctest --test-dir build-core --output-on-failure
```

文档：`docs/backends.md`（行为节 + 验证入口）、`README.md`/`README.en.md`
特性节、`config/example.ini` 注释示例。

## 边界

- 不做逐发射器曲线（v2）、不做曲线编辑器 UI、不做速度曲线（那是模拟侧）、
  不做 GPU 端 keys 插值（facade 烘焙后 shader 只查表）。
- 曲线不影响模拟状态/统计/排序；`cross_backend_test` 与 `RenderParameters`
  保持不变。
- 不支持曲线的后端：程序化非空 setter 与含非空 `[curves]` 的 apply 都拒绝；
  空曲线永远允许（现状行为）。
- GL binding 23 超出 4.3 规范最小保证（8 个 SSBO binding point），与项目
  已用 binding 11 的既有假设一致；不做运行时探测回退。
- 曲线对折射 pass 同样生效（vColor/vFadeRGB 预乘影响 coverage 的 alpha），
  这是乘法语义的统一结果，不做按 pass 开关。

---

## 完成记录

- 日期：2026-09-18
- 环境：Windows + MSYS2 ucrt64（Vulkan 1.4 驱动，NVIDIA RTX 4060 Laptop）。

### 交付物

- `include/ember/particle_types.hpp`：`kLifeCurveLutSize=64` + `LifeCurvesLut`
  （`vec4 color[64]`、`float size[64]`、CPU-only `mask`）。
- `include/ember/backend.hpp`：`BackendCapabilities::lifeCurves` + `uploadLifeCurves`
  默认实现（mask 非零抛 `unsupported("life curves")`）。
- `include/ember/system.hpp` / `src/particle_system.cpp`：`setColorOverLife`/
  `setSizeOverLife` + getter；facade 烘焙 64 项均匀 LUT（分段线性、端点钳制），
  关闭时上传全 1；非空 keys 无能力时抛 `invalid_argument`，分量非有限拒绝。
- `include/ember/config.hpp` + `config/example.ini`：`[curves]` 段（`size`/`color`
  显式 t 键、严格递增、空值关闭），apply 先重采样为均匀 LUT 再走 setter；
  `curves.size`/`curves.color` 记录 specified，apply 保持原子性。
- `src/backends/opengl/params.hpp`：`CurvesParams`（std430，1280 B，offset 断言）。
- `shaders/particle.vert`：VS 两个分支都加 `EMBER_BIND_CURVES`（Vulkan 8 / GL 23）与
  `BufCurves`；`colorLut`/`sizeLut` 按 `ageFrac` 采样，颜色对 `vColor`/`vFadeRGB`
  预乘、尺寸乘进 `abs(p.pos.w)`。
- GL：`kBindingCurves=23`，`curvesBuf_` 初始化全 1，`uploadLifeCurves`，
  `drawParticles` 无条件 `glBindBufferBase`（宽容路径）；能力第 8 位 true。
- Vulkan：render set binding 8（STORAGE/VERTEX）、描述符池
  `21*kFramesInFlight+4`、`curvesBuf_` 持久资源 + CPU 副本（resize 重传）、
  `uploadLifeCurves`；`lifeCurves=true`。
- 测试：`backend_contract_test`（能力门控 + 烘焙端点/中点 + apply 原子性）、
  `config_test`（`[curves]` 解析/空值/坏 t）、`effects.hpp` 与 `vulkan_test.cpp`
  各 3 条像素用例（尺寸单调缩小、颜色衰减、清空后逐位回到基线）。

### 验收输出摘要

```text
ctest --test-dir build --output-on-failure         # 11/11
ctest --test-dir build-core --output-on-failure     # 6/6
EMBER_VK_DEBUG=1 ember_vulkan_test：零 validation 输出
```

### 遗留

- INI `color` 键需要 t + 4 分量（rgba），无法塞进单个 vec4，故 Config 用
  `CurveKey{float t; glm::vec4 value;}`（对 WO-09 原稿的 vec4 表述做了必要偏离）。
- GL binding 23 超出 4.3 规范最小 SSBO binding 保证（8），与项目已用 binding 11
  的既有假设一致，不做运行时探测回退。

---

## 验收修复记录（2026-09-18，审查人：主模型）

深审结论：烘焙算法、shader 公式、双后端接线、capability 拒绝、INI 解析、
像素用例全部符合定型，无功能 bug。补了三条验收用例缺口：color LUT 中点
断言（发现并修正审查新增断言自身的期望值错误——alpha 同样随曲线插值）、
apply 级 `[curves]` 显式空值关闭（caps 开/关两种状态）、粒子死亡后覆盖
为零（GL effects.hpp 与 Vulkan 侧各一条）。

验收结论：通过（ctest 11/11、build-core 6/6、validation 零输出）。
