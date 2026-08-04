# ember

基于**现代 OpenGL（4.3 core）**的 GPU 粒子动画库。仿真跑在 compute shader 上，渲染用 instanced billboard quad（视图对齐四边形）。核心库**不依赖任何窗口系统**，可嵌入任意 C++ 技术栈（GLFW / SDL2 / Qt / Win32 / EGL / 自有引擎），也提供**单头文件版**（`single_header/ember.hpp`，一个文件注入整个库）。

> 🌏 English: [README.en.md](README.en.md) · 嵌入指南: [INTEGRATION.md](INTEGRATION.md) / [INTEGRATION.en.md](INTEGRATION.en.md)

## 特性

- **GPU 仿真**：compute shader 三阶段（积分 / 出生 / 间接绘制参数），SSBO 乒乓缓冲；粒子出生采样（形状/锥形/调色板/渐变/尺寸↔速度联动）全部在 GPU 完成——CPU 每帧只编码少量**出生请求**（~176B/条）并读回 16 字节
- **槽位自动回收**：free-stack（原子 CAS 弹栈 + LIFO，并发安全）复用死亡槽位，无压缩开销、alive 计数零漂移
- **力场**：重力 / 阻力（线性·二次）/ 均匀风 / 值噪声湍流 / 点吸引子 / 涡旋 / 弹簧 / 空间噪声风 / 行波 / 平面边界（kill·bounce），全部力可**独立开关**（`enableForce`/`disableForce` bitmask）
- **发射器**：Point / Box / Sphere / Cone 四种形状，速度 / 寿命 / 尺寸 / 颜色随机区间；锥形开口角、尺寸↔速度联动（大粒子飞得更快）
- **可编辑配置层**：INI 式配置文件（`config/example.ini`）暴露全部参数；示例按 `R` 键**热重载**；API 层 `setGravity`/`setDrag`/`setAttractors` 等可编程设置，两条路径并存
- **交互式发射器编辑器**（示例内，`EMBER_BUILD_EDITOR` 宏可整体关掉）：`1-9` 选源、`WASD` 移动、鼠标放置、`[ ]` 缩放形状、`- =` 调 rate、`P` 打印成配置片段
- **粒子贴图**：内置径向渐变光斑 + 可选 PNG（stb_image），sprite sheet 帧动画，颜色渐变熄灭（fade color）
- **Billboard quad 渲染**：每粒子一个视图对齐四边形（`TRIANGLE_STRIP`×4，无顶点属性，VS 里由 `gl_VertexID` 展开），替代点精灵——不受点尺寸/旋转限制
- **拖尾拉伸（streak）**：四边形沿粒子速度在视图平面的投影方向拉长，火花/彗星尾迹；`sys.setStreak(k)`
- **软粒子**：采样宿主提供的场景深度纹理，粒子接近遮挡面时淡出（`sys.setSoftParticles(on, depthTex, radius)`）
- **GPU 深度排序（OIT）**：compute bitonic 排序按视图深度远→近重排绘制顺序，正确 alpha 混合（`sys.setSortEnabled(true)`）；排序默认关闭
- **HDR bloom 后处理**：自带 FBO 链（提亮→2×2 降采样→两次高斯模糊→叠加回宿主帧缓冲），`sys.setBloom(true)` / `B` 键切换
- **间接绘制**：渲染实例数来自 GPU 写入的绘制参数缓冲（`glDrawArraysIndirect`），CPU 读回不参与渲染路径
- **嵌入友好**：CMake `add_subdirectory` / `FetchContent` / `find_package` 三种消费方式；非 CMake 项目可直接编译源码；或使用**单头文件版** `single_header/ember.hpp`

## 依赖

| 依赖 | 角色 | 必需性 | 说明 |
| --- | --- | --- | --- |
| OpenGL **4.3 core** | 运行时 | ✅ | compute shader + SSBO（macOS 最高 4.1 不支持，见 INTEGRATION 第 8 节） |
| glad（v2，`gl:core=4.3`） | GL 入口加载 | ✅ | 生成的头文件 + `glad.c` 编译进项目；`ember::gl::init(loader)` 注入 |
| glm（≥ 0.9.9） | 数学库（仅头文件） | ✅ | vec/mat 类型 |
| GLFW 3.4 | 窗口（仅示例/`ember_glfw` 模块/`EMBER_USE_GLFW`） | 可选 | 核心库不需要 |
| stb_image | PNG 贴图（`EMBER_USE_STB`） | 可选 | 未定义时 `setSpriteTexture` 回退内置渐变 |
| CMake ≥ 3.16 | 构建 | 可选 | 非 CMake 项目直接编译源码/单头文件 |
| Python 3 | 仅 FetchContent 拉 glad 时 | 可选 | glad 生成器（离线构建需 jinja2） |

> 单头文件版（`single_header/ember.hpp`）同样需要 glad/glm（文档化依赖，见 [INTEGRATION.md §3.4](INTEGRATION.md)）；GLFW/stb 按宏开启。

## 构建与运行

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 从项目根目录运行（配置文件路径也可作为第一个参数传入）
./build/example_basic
./build/example_fireworks
```

离线/受限网络构建：仓库自带依赖包（`deps/`），配置时用 `-DEMBER_DEPS_DIR=<绝对路径>` 即可全程无需联网（glad 生成仍需本机 Python 3 + jinja2）；系统已装好依赖则用 `-DEMBER_FETCH_DEPS=OFF`。

示例操作：左键拖动=环绕相机，滚轮=缩放，`R`=热重载配置文件，`ESC`=退出。压力测试配置：`./build/example_basic config/stress.ini`（100 万粒子容量）。

## 快速开始

```cpp
#include "ember/particle_system.hpp"

ember::ParticleSystem sys({150000, 40000});       // capacity, maxSpawnPerFrame
sys.setGravity({0.f, -7.f, 0.f});
sys.setDrag(0.05f);
sys.setTurbulence(0.4f);
sys.setAttractors({{/* 位置 */ {0, 1, 0}, /* 强度 */ 60.f}});
sys.setStreak(1.2f);                            // 拖尾拉伸（火花尾迹）
sys.setSortEnabled(true);                       // GPU 深度排序（OIT，正确 alpha 混合）
sys.setBloom(true);                             // HDR bloom 后处理
// 软粒子（需宿主把场景深度纹理传进来）：sys.setSoftParticles(true, depthTex, 0.6f);

auto& fountain = sys.addEmitter();
fountain.shape = ember::Emitter::Shape::Cone;
fountain.rate = 7000.f;
fountain.baseVelocity = {0.f, 10.f, 0.f};
fountain.speedMin = 7.f;   fountain.speedMax = 11.f;
fountain.lifeMin = 2.f;    fountain.lifeMax = 3.5f;
fountain.colorMin = {1.f, 0.8f, 0.3f, 1.f};
fountain.colorMax = {1.f, 0.2f, 0.05f, 1.f};

// 每帧（宿主渲染循环中）：
sys.update(dt);                                                 // GPU 仿真
sys.render(cam.view(), cam.proj(), fbWidth, fbHeight, cam.fovY); // instanced billboard 绘制
```

### 配置文件（可编辑空间）

编辑 `config/example.ini`（重力、风、湍流、混合模式、发射器全部参数、调色板、吸引子、渲染增强：拖尾/软粒子/bloom/排序），示例中按 `R` 即时生效：

```ini
[system]
gravity     = 0, -7, 0
turbulence  = 0.4
blend       = additive
forces      = gravity, drag, wind, turbulence, attractors, vortex, noise_wind
streak          = 1.2       # >0: 沿速度方向拖尾拉伸
soft_particles  = false     # 软粒子（需宿主提供场景深度纹理）
bloom           = true      # HDR bloom 后处理
bloom_threshold = 0.8       # 提亮阈值（默认 1.0）
sort            = false     # GPU 深度排序（OIT，正确 alpha 混合）

[palette "fire"]
colors = 1,0.95,0.55,1 | 1,0.6,0.15,1 | 1,0.18,0.05,1

[emitter "fountain"]
shape = cone
position = 0, 0, 0
rate = 7000
base_velocity = 0, 10, 0
spread = 0.25
life_min = 2    life_max = 3.5
size_min = 0.05 size_max = 0.11
palette = fire

[attractor]
position = 0, 0.6, 0
strength = 60
```

代码中调用：`sys.loadConfig("config/example.ini")` 或 `sys.apply(Config::fromFile(...))`。配置修改是**累积生效**（未写的键保持当前值），`capacity` 变更会重建缓冲区并清空粒子。诊断：配置加 `debug = true`（或环境变量 `EMBER_DEBUG=1`）会在 stderr 每 6 帧输出 alive/出生请求数/力掩码 + GL 错误扫描。

## 粒子尺寸 / 贴图 / 编辑器

- **尺寸与速度联动**：`sys.setSizeScale(1.5f)` 或 `[system] size_scale = 1.5` 全局放大粒子；`scale_speed_with_size = true`（默认）时出生速度按同比例放大，轨迹比例不变；`[emitter] speed_scale` 单独调速度、`speed_size_link` 让同一发射器里大粒子飞得更快
- **粒子贴图**：默认内置 64×64 径向渐变光斑（零依赖）；`sys.setSpriteTexture("sprites/glow.png")` 或 `[system] texture = ...` 换 PNG（stb_image，`EMBER_USE_STB`），加载失败自动回退内置；`sys.setSpriteSheet(cols, rows)` 启用帧动画；`T` 键切换贴图/程序化光斑
- **颜色渐变熄灭**：`[emitter] fade_color_min/max` 设定粒子死亡时渐变到的颜色（存进 `life.yzw`，仍是 64 字节）
- **交互式发射器编辑器**（示例，`EMBER_BUILD_EDITOR` 可关）：键位 `1-9` 选发射器、`WASD/空格/Shift` 移动、右键放置、`[ ]` 缩放形状、`,` `.` 锥角、`-` `=` rate、`P` 打印 INI 片段、`B` bloom 开关、`O` 深度排序开关

## 架构

### 粒子布局（CPU 与 GPU std430 严格一致，`static_assert sizeof==64`）

| 偏移 | 字段 | 含义 |
| --- | --- | --- |
| 0 | `pos` vec4 | xyz 位置，w = 世界尺寸 |
| 16 | `vel` vec4 | xyz 速度，w = 年龄 |
| 32 | `life` vec4 | x = 寿命；x<0 = 尸体槽；**yzw = 熄灭渐变目标色 RGB** |
| 48 | `color` vec4 | rgba |

### 帧管线（渲染不依赖 CPU 读回；唯一同步：16 字节 alive 读回，仅供 UI/API）

```
CPU 编码出生请求(发射器+burset, ~176B/条, 封顶 maxSpawnPerFrame)
   → 请求 SSBO + uSpawnRequestCount + uSpawnTotal
   → dispatch A(phase=0): 积分+死亡入栈            [barrier]
   → dispatch B(phase=1): GPU 采样出生(回收槽或 append) [barrier]
   → dispatch C(phase=2): 写 glDrawArraysIndirect 参数 [barrier]
   → 读回 uAlive（16 字节，仅供 aliveCount()/UI）→ swap cur/nxt
   → [可选] GPU bitonic 排序（远→近，uUseSorted=1 时 VS 经 sorted[] 取粒子）
   → glDrawArraysIndirect（实例数来自 GPU 参数缓冲）
```

出生请求 `SpawnRequest`（176B std430，见 `ember/emitters.hpp`）携带形状/锥角/速度/寿命/尺寸区间/调色板索引等参数，GPU 用哈希 RNG 逐粒子采样——CPU 每帧只上传几 KB 请求，不再生成/上传完整粒子。

### 力场（11 种，可独立开关）

启用/禁用：`sys.enableForce(Force::X)` / `sys.disableForce(Force::X)`；`Force` 枚举即 bit 序号，`sys.forceMask()` 可查。默认：既有 5 种开，新增 6 种关。

| 类别 | 力场 | 公式 / 行为 | 参数 API | 配置键 |
| --- | --- | --- | --- | --- |
| 基础 | 重力 | `a += g` | `setGravity` | `gravity` |
| | 阻力 | `a -= v·k`（linear）或 `-v·\|v\|·k`（quadratic） | `setDrag` + `setDragMode` | `drag` / `drag_mode` |
| | 均匀风 | `a += w` | `setWind` | `wind` |
| | 湍流 | `a += (noise3(p·s+t)·2−1)·k` | `setTurbulence` | `turbulence` |
| 空间场 | 点吸引子 | `a += d·s·r⁻³`（±=吸/斥，可多个） | `setAttractors` | `[attractor]` 段 |
| | 涡旋 | `a += cross(axis, d)·s/(r²+r0²)`（绕轴切向旋转，可多个） | `setVortexes` | `[vortex]` 段 |
| | 弹簧 | `a += (anchor−p)·k − v·c`（可多个） | `setSprings` | `[spring]` 段 |
| | 空间噪声风 | `a += dir·(noise3(p·scale+t·speed)·2−1)·amp` | `setNoiseWind` | `noise_wind_*` |
| | 行波 | `a += dir·sin(dot(p,k)+ωt)·amp` | `setWave` | `wave_*` |
| 效果 | 平面边界 | `y < planeY` 时 kill 或 bounce（恢复系数） | `setBoundary` | `boundary_mode/y/restitution` |

## 扩展：自定义 shader / 力场

默认 shader **优先从 `shaders/` 目录加载**（`particle.vert` / `particle.frag` / `simulate.comp` / `sort.comp` / bloom 三件套，目录可用 `sys.setShaderDirectory()` 改），改文件重启即生效；文件缺失或编译失败时自动回退到内嵌副本（嵌入零文件场景）。完全自定义则注入：

```cpp
ember::Shader render = ember::Shader::fromFiles({{GL_VERTEX_SHADER, "my.vert"},
                                                 {GL_FRAGMENT_SHADER, "my.frag"}});
ember::Shader sim = ember::Shader::fromFiles({{GL_COMPUTE_SHADER, "my.comp"}});
sys.setPrograms(std::move(render), std::move(sim));
```

自定义 shader 必须遵守的契约（uniform 名与 SSBO binding）：

| Binding | 缓冲 | 用途 |
| --- | --- | --- |
| 0 | `cur` / `particles` | 当前粒子（sim 只读 / 渲染 VS 只读） |
| 1 | `nxt` | 下一帧粒子（sim 写） |
| 2 | `req` | 出生请求（`SpawnRequest[]`，只读；GPU 采样出生） |
| 3 | `dead` | free-stack（coherent） |
| 4 | `counters` | `uAlive uDeadHead uSpawnRequestCount uCapacity`（coherent） |
| 5 | `attractors` | vec4 吸引子（只读） |
| 6 | `vortexes` | `Vortex` 数组（只读） |
| 7 | `springs` | `Spring` 数组（只读） |
| 8 | `sorted` | GPU 排序后的粒子索引（渲染 VS 只读，可选；未开排序时忽略） |
| 9 | `palette` | 调色板颜色 `vec4[]`（GPU 出生取色，只读） |
| 10 | `indirect` | 间接绘制参数（phase=2 写入；渲染 `glDrawArraysIndirect` 读取） |

> ⚠️ **破坏性变更（0.x）**：binding 2 语义从"完整 `Particle[]` 暂存"改为"`SpawnRequest[]` 出生请求"——自写出生逻辑的宿主 shader 需按新契约迁移。

sim uniform：`uDt uTime uPhase uForceMask uGravity uDrag uDragMode uWind uTurbulence uAttractorCount uVortexCount uSpringCount uNoiseWindDir uNoiseWindAmp uNoiseWindScale uNoiseWindSpeed uWaveDir uWaveK uWaveAmp uWaveOmega uBoundaryMode uBoundaryY uRestitution uSpawnTotal uFrameSeed`（`uPhase=0` 积分、`=1` 出生、`=2` 写间接参数，三个 dispatch；`uSpawnTotal`=本帧请求总粒子数、`uFrameSeed`=GPU 出生 RNG 种子；`uForceMask` bit i = `ember::Force` 枚举值 i，关闭的力零开销）；渲染 VS uniform：`uView uProj uSizeScale uStreak uUseSorted`（`uUseSorted=1` 时粒子索引取 `sorted[gl_InstanceID]`，=0 直接 `gl_InstanceID`；`uStreak>0` 四边形沿速度投影拉伸）；渲染 FS uniform：`uSprite uUseSprite uSheetCols uSheetRows uSceneDepth uInvViewProj uSoftRadius uViewportSize uUseSoft`（`uUseSprite=1` 贴图采样+帧动画，`=0` 程序化光斑；`uUseSoft=1` 按场景深度淡出；`vFadeRGB` 来自 `life.yzw` 实现熄灭渐变）。

## 嵌入其他项目

见 **[INTEGRATION.md（嵌入与集成指南）](INTEGRATION.md)**：CMake 三种嵌入方式、非 CMake 工作流、**单头文件版使用**、GLFW/SDL2/Qt/Win32/EGL 逐栈接入、与宿主渲染共存、macOS/4.1 兼容说明、FAQ。

## 性能说明

- 渲染与 CPU 读回解耦：绘制实例数由 GPU（间接参数缓冲）提供；每帧 CPU→GPU 只有少量出生请求（`发射器数 × 176B`），GPU→CPU 仅 16 字节 alive（`glGetBufferSubData`，仅供 `aliveCount()`/UI）
- `capacity` 按最坏活粒子数设定（内存 = capacity × 64B × 2 + 回收栈 × 4B + 排序索引 × 4B）；出生率 × 平均寿命 ≈ 稳态活粒子数
  - 1M 粒子 ≈ **130 MB** 显存（两个 64 MB 粒子缓冲 + 4 MB 回收栈 + 4 MB 排序索引）；4M ≈ 520 MB
- 参考实测（本机 AMD 驱动，GL 4.3）：`config/stress.ini`（容量 1M、关闭后处理）稳态 **~85 万活粒子 @ ~166fps**
- 出生率长期低于死亡率时，环形回收栈可能丢弃最旧条目（对应槽位保持为尸体，不产生视觉错误）
- 可选后处理按需开启（默认全部关闭，零开销）：bloom 每帧 3 张半分辨率 RGBA16F FBO + 2 次高斯模糊 pass；深度排序每帧 ~log²N 个极小的 compare-exchange dispatch（N=262144 时约 171 次，N=1M 时约 400 次——排序默认关闭，大容量下可降低排序频率或改用 radix）

## 许可证

MIT。
