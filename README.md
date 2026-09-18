# ember

基于**现代 OpenGL（4.3 core）** 和 **Vulkan 1.1** 的 GPU 粒子动画库。仿真跑在 compute shader 上，渲染用 instanced billboard quad（视图对齐四边形）。核心库**不依赖任何窗口系统**，可嵌入任意 C++ 技术栈（GLFW / SDL2 / Qt / Win32 / EGL / 自有引擎），也提供**单头文件版**（`single_header/ember.hpp`，一个文件注入整个库）。

> 🌏 English: [README.en.md](README.en.md) · 嵌入指南: [INTEGRATION.md](INTEGRATION.md) / [INTEGRATION.en.md](INTEGRATION.en.md)

## 特性

- **GPU 仿真**：按存活索引积分，批量分配出生槽位，再发布间接绘制参数；粒子和存活索引均使用乒乓缓冲。出生采样全部在 GPU 完成，CPU 每帧编码少量**出生请求**（~176B/条），默认同步读回 20 字节（GPU 调度可免除这次同步读回）
- **槽位自动回收**：free-stack 批量预留并复用死亡槽位，出生线程无需竞争 CAS；稳定槽位保持粒子身份
- **力场**：重力 / 阻力（线性·二次）/ 均匀风 / 值噪声湍流 / 点吸引子 / 涡旋 / 弹簧 / 空间噪声风 / 行波 / 平面边界（kill·bounce），全部力可**独立开关**（`enableForce`/`disableForce` bitmask）
- **发射器**：Point / Box / Sphere / Cone 四种形状，速度 / 寿命 / 尺寸 / 颜色随机区间；锥形开口角、尺寸↔速度联动（大粒子飞得更快）
- **可编辑配置层**：INI 式配置文件（`config/example.ini`）暴露全部参数；示例按 `R` 键**热重载**；API 层 `setGravity`/`setDrag`/`setAttractors` 等可编程设置，两条路径并存
- **交互式发射器编辑器**（示例内，`EMBER_BUILD_EDITOR` 宏可整体关掉）：`1-9` 选源、`WASD` 移动、鼠标放置、`[ ]` 缩放形状、`- =` 调 rate、`P` 打印成配置片段
- **粒子贴图**：内置径向渐变光斑 + 可选 PNG（stb_image），sprite sheet 帧动画，颜色渐变熄灭（fade color）
- **Billboard quad 渲染**：每粒子一个视图对齐四边形（`TRIANGLE_STRIP`×4，无顶点属性，VS 里由 `gl_VertexID` 展开），替代点精灵——不受点尺寸/旋转限制
- **拖尾拉伸（streak）**：四边形沿粒子的屏幕运动方向居中拉长，考虑透视下的纵深速度；`sys.setStreak(k)`。这是瞬时拉伸，不记录历史轨迹，详见 [拖尾说明](docs/streak.md)。
- **Billboard 自旋（spin）**：随机静态角 + 年龄自旋（`sys.setSpin(speed)` / `[system] spin`），落叶/纸屑/碎片翻滚
- **折射粒子（玻璃渣）**：屏幕空间折射——粒子按面片法线偏移采样宿主场景纹理（`setOpenGLRefraction(sys, ...)` / `[system] refraction`），玻璃/热浪/水珠/棱镜预设（`ember::Refraction::glass()/heat()/water()/prism()`）；示例 `example_glass`
- **软粒子**：采样宿主提供的场景深度纹理，粒子接近遮挡面时淡出（`setOpenGLSoftParticles(sys, on, depthTex, radius)`）
- **GPU 深度排序**：compute bitonic 排序按视图深度远→近重排绘制顺序，正确 alpha 混合（`sys.setSortEnabled(true)`）；排序默认关闭
- **HDR bloom 后处理**：自带 FBO 链（提亮→2×2 降采样→两次高斯模糊→叠加回宿主帧缓冲），`sys.setBloom(true)` / `B` 键切换
- **间接绘制**：渲染实例数来自 GPU 写入的绘制参数缓冲（`glDrawArraysIndirect`），CPU 读回不参与渲染路径
- **事件子发射**：粒子死亡/反弹时在同一帧于 GPU 触发子模板批量出生（`addEventEmitter` + `Emitter::onDeath/onBounce`，INI `[event "name"]` / `on_death` / `on_bounce`），支持链式（烟花多级炸开）、`inheritVelocity` 继承父速度，双后端语义逐位对齐
- **生命周期曲线**：颜色/尺寸随寿命比例变化（`setColorOverLife` / `setSizeOverLife` / INI `[curves]`），facade 烘焙 64 项 LUT，乘法语义、空曲线与现状逐位一致
- **嵌入友好**：CMake `add_subdirectory` / `FetchContent` / `find_package` 三种消费方式；非 CMake 项目可直接编译源码；或使用**单头文件版** `single_header/ember.hpp`

## 后端组织

公共层与图形后端已分离：`ember::core` 仅依赖 glm；`ember::opengl` 提供 GL 4.3 实现，原 `ember` 目标和默认构造方式继续有效；`ember::vulkan`（`-DEMBER_BUILD_VULKAN=ON`，默认开）提供 Vulkan 1.1 实现。`-DEMBER_BUILD_OPENGL=OFF` 只构建公共层，`-DEMBER_BUILD_GLFW=OFF` 去掉窗口依赖。通过 `ParticleBackend` 可以注入其他后端并逐项声明功能支持。

源码分工、接口约定和接入顺序见 [后端说明](docs/backends.md)。当前提供两个可运行后端：OpenGL 4.3 与 Vulkan 1.1（GPU 调度、深度排序、贴图、软粒子、折射、Bloom、事件子发射、生命周期曲线全部对齐，`capabilities()` 全开；Vulkan 另支持 `beginVulkanFrame`/`endVulkanFrame` 宿主 command buffer 录入）。Vulkan 模块只在找到 Vulkan-Headers 与加载器时构建，缺失时静默跳过。

效果逻辑、已修复问题与回归覆盖见 [效果审查记录](docs/effects-audit.md)。

## 依赖

下表针对默认 OpenGL 构建；公共层只需要 glm。

| 依赖 | 角色 | 必需性 | 说明 |
| --- | --- | --- | --- |
| OpenGL **4.3 core** | 运行时 | ✅ | compute shader + SSBO（macOS 最高 4.1 不支持，见 INTEGRATION 第 8 节） |
| glad（v2，`gl:core=4.3`） | GL 入口加载 | ✅ | 生成的头文件 + `glad.c` 编译进项目；`ember::gl::init(loader)` 注入 |
| glm（≥ 0.9.9） | 数学库（仅头文件） | ✅ | vec/mat 类型 |
| GLFW 3.4 | 窗口（仅示例/`ember_glfw` 模块/`EMBER_USE_GLFW`） | 可选 | 核心库不需要 |
| stb_image | PNG 贴图（`EMBER_USE_STB`） | 可选 | 未定义时 `setSpriteTexture` 回退内置渐变 |
| Vulkan-Headers + 加载器（1.1+） | Vulkan 后端 | 可选 | 仅 `-DEMBER_BUILD_VULKAN=ON`（默认）时需要；缺失时跳过该模块，另有 glslangValidator 构建 SPIR-V |
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

```bash
./build/example_glass          # 折射玻璃渣（G 切换预设，SPACE 爆裂）
```

## 快速开始

```cpp
#include "ember/particle_system.hpp"

ember::ParticleSystem sys({150000, 40000});       // capacity, maxSpawnPerFrame
sys.setGravity({0.f, -7.f, 0.f});
sys.setDrag(0.05f);
sys.setTurbulence(0.4f);
sys.setAttractors({{/* 位置 */ {0, 1, 0}, /* 强度 */ 60.f}});
sys.setStreak(1.2f);                            // 拖尾拉伸（火花尾迹）
sys.setSortEnabled(true);                       // GPU 深度排序（正确 alpha 混合）
sys.setBloom(true);                             // HDR bloom 后处理
// 软粒子（需宿主把场景深度纹理传进来）：setOpenGLSoftParticles(sys, true, depthTex, 0.6f);

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
sort            = false     # GPU 深度排序（正确 alpha 混合）

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

### 默认同步模式的帧管线

```
CPU 编码出生请求（发射器+burst，~176B/条，封顶 maxSpawnPerFrame）
   → 请求 SSBO + uSpawnRequestCount + uSpawnTotal
   → phase=0: 按存活索引积分，死亡入栈，幸存者写下一帧索引；死亡/反弹按槽位标签入事件队列 [barrier]
   → phase=4: 事件压缩（有事件模板时）：子实例前缀和与预算截断 [barrier]
   → phase=3: 单 invocation 批量预留回收/追加槽位（有出生时，含事件子粒子）[barrier]
   → phase=1: GPU 采样出生，写粒子与下一帧索引（有出生时）[barrier]
   → phase=2: 单 invocation 发布间接绘制实例数 [barrier]
   → 读回前五个计数器（20 B）→ swap 粒子缓冲与存活索引缓冲
   → [可选] GPU bitonic 排序（远→近，uUseSorted=1 时 VS 经 sorted[] 取粒子）
   → glDrawArraysIndirect（实例数来自 GPU 参数缓冲）
```

出生请求 `SpawnRequest`（176B std430，见 `ember/emitters.hpp`）携带形状/锥角/速度/寿命/尺寸区间/调色板索引等参数，GPU 用哈希 RNG 逐粒子采样——CPU 每帧只上传几 KB 请求，不再生成/上传完整粒子。

### 力场（10 种，可独立开关）

启用/禁用：`sys.enableForce(Force::X)` / `sys.disableForce(Force::X)`；`Force` 枚举即 bit 序号，`sys.forceMask()` 可查。默认：既有 5 种开，新增 5 种关。

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
setOpenGLPrograms(sys.backend(), std::move(render), std::move(sim));
```

自定义 shader 必须遵守的契约（uniform 名与 SSBO binding）：

| Binding | 缓冲 | 用途 |
| --- | --- | --- |
| 0 | `cur` / `particles` | 当前粒子（sim 读取并在退休时写墓碑 / 渲染 VS 只读） |
| 1 | `nxt` | 下一帧粒子：phase 0/1 写入 |
| 2 | `req` | 出生请求（`SpawnRequest[]`，只读；GPU 采样出生） |
| 3 | `dead` | free-stack（coherent） |
| 4 | `counters` | `uAlive uDeadHead uSpawnRequestCount uCapacity uAllocated uSpawnReuse uSpawnAppendBase uSpawnAccepted`（coherent） |
| 5 | `attractors` | vec4 吸引子（只读） |
| 6 | `vortexes` | `Vortex` 数组（只读） |
| 7 | `springs` | `Spring[]`: std430 stride **32 B**, offsets anchor=0, stiffness=12, damping=16 |
| 8 | `sorted` | GPU 排序后的粒子索引（渲染 VS 只读，可选；未开排序时忽略） |
| 9 | `palette` | 调色板颜色 `vec4[]`（GPU 出生取色，只读） |
| 10 | `indirect` | 间接绘制参数（phase=2 写入；渲染 `glDrawArraysIndirect` 读取） |
| 11 | `liveIndices` | 当前存活索引；积分/渲染/排序读取，更新后与下一帧索引交换 |
| 12 | `nextLiveIndices / sortKeys` | 仿真时绑定下一帧索引；排序时绑定独立的深度键缓存 |
| 13 | `schedule` | GPU 调度元数据与间接 compute 命令 |

> ⚠️ **破坏性变更（0.x）**：binding 2 语义从"完整 `Particle[]` 暂存"改为"`SpawnRequest[]` 出生请求"——自写出生逻辑的宿主 shader 需按新契约迁移。内置 shader 的散 uniform 已收敛为 std140 uniform 块（契约见上）；`ParticleSystem::setPrograms/setRefraction/setSoftParticles` 成员已移除，改用 `ember/opengl.hpp` 的 `setOpenGLPrograms(sys.backend(), ...)` / `setOpenGLRefraction(sys, ...)` / `setOpenGLSoftParticles(sys, ...)`——公共 facade 不再出现 GL 类型，非 GL 后端无需为这些入口定义行为。

内置 shader 不再使用散 uniform——标量参数全部打包进 std140 uniform 块（同一份 GLSL 可直接编译 SPIR-V；CPU 镜像与布局断言见 `src/backends/opengl/params.hpp`）：仿真 `SimParams`（binding 14；`uPhase=0` 积分、`=3` 批量分配、`=1` 出生、`=2` 发布间接绘制参数；有出生时最多四个 dispatch；`uSpawnTotal`=本帧请求总粒子数、`uFrameSeed`=GPU 出生 RNG 种子；`uForceMask` bit i = `ember::Force` 枚举值 i，关闭的力零开销）；渲染 VS `DrawParams`（binding 17；`uUseSorted=1` 时粒子索引取 `sorted[gl_InstanceID]`，=0 取 `liveIndices[gl_InstanceID]`；`uStreak>0` 四边形沿速度投影拉伸）；渲染 FS `FragParams`（binding 18；`uUseSprite=1` 贴图采样+帧动画，`=0` 程序化光斑；`uUseSoft=1` 按场景深度淡出；`vFadeRGB` 来自 `life.yzw` 实现熄灭渐变）；排序 `SortParams`（15）、内部调度 `ScheduleParams`（16）、bloom `BloomParams`（19）。采样器带显式 binding：`uSprite`=0、`uSceneDepth`=1、`uSceneColor`=2；stage 间 varying 带显式 location。无实例名的块成员共享全局命名空间，故 fragment 块内 `uProj`/`uRefraction` 改名 `uFragProj`/`uFragRefraction`。后端仍同步写旧的散 uniform，按旧契约编写的自定义 shader 继续可用；新自定义 shader 建议直接声明同名块成员（协议检测对块成员同样生效）。

## 嵌入其他项目

见 **[INTEGRATION.md（嵌入与集成指南）](INTEGRATION.md)**：CMake 三种嵌入方式、非 CMake 工作流、**单头文件版使用**、GLFW/SDL2/Qt/Win32/EGL 逐栈接入、与宿主渲染共存、macOS/4.1 兼容说明、FAQ。

## 性能说明

可重复的性能基准、CPU/GPU 指标口径和实测数据见 [benchmarks/README.md](benchmarks/README.md)。使用 `-DEMBER_BUILD_BENCHMARKS=ON` 构建，运行 `python tools/run_benchmarks.py --output out/baseline-local`。

- 默认每帧同步读回五个计数器（20 B）。启用 `sys.setGpuDriven(true)` 后，GPU 生成积分/排序 dispatch，统计异步采样；界面使用 `pollStatistics()`。`aliveCount()` 保持即时准确，可能等待。详见 [集成指南](INTEGRATION.md#gpu-调度与异步统计可选)。
- 显存：capacity × 128 B 粒子双缓冲，加回收栈 capacity × 4 B、存活索引双缓冲 capacity × 8 B，以及 nextPow2(capacity) × 4 B 排序索引。排序按需再分配最多 nextPow2(capacity) × 4 B 深度键缓存。
  - 1M 容量约 144 MB（138 MiB），排序键满额分配后约 148 MB（142 MiB），不含请求、调色板和后处理。
- 生命周期修复后的首份实测数据见基准报告；集成时仍应在目标 GPU 和实际场景上测量。
- 死亡槽位保留在回收栈中，不丢弃条目。内置积分和渲染只访问存活索引；旧版自定义仿真仍可扫描已分配范围。
- 可选后处理按需开启（默认全部关闭）：bloom 每帧 1 张全分辨率和 2 张半分辨率 RGBA16F FBO + 2 次高斯模糊 pass；排序缓存深度键，在 256 项 shared-memory tile 内合并比较步骤，N=262144 / 1M 时分别共 66 / 91 次 dispatch（含初始化），仍为精确远到近排序。

## 验证与 Shader 兼容性

运行 `ctest --test-dir build --output-on-failure`。GPU 测试覆盖生命周期、事件子发射、排序、Bloom/遮挡、折射、软粒子、生命周期曲线、宿主 command buffer 与内嵌回退；`cross_backend_test` 对同一指令流逐帧对拍 GL / Vulkan 同步 / Vulkan GPU 调度三条腿的模拟状态一致性；Python 检查生成文件同步。

Shader 优化协议与旧版兼容路径见 [INTEGRATION.md](INTEGRATION.md)。新增 binding 12 与三个计数器尾部字段；前五个计数器、32 字节 `Spring` 数组、`uInvProj = inverse(proj)` 保持不变。请求同时受 4096 条请求和 `maxSpawnPerFrame` 粒子预算限制，burst 先于 emitter 接受。编辑 Shader 后先运行 `python tools/sync_embedded_shaders.py`，再运行 `python tools/amalgamate.py`。

## 许可证

MIT。
