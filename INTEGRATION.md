# ember 使用指南（嵌入不同技术栈）

本文档面向把 ember 嵌入**现有 C++ 项目**的开发者。核心结论：**ember 不创建 GL 上下文、不依赖窗口系统**——你只需要在已有的 GL 上下文上做两件事：

1. 用你的加载器初始化 glad：`ember::gl::init(loader)`
2. 每帧调用 `sys.update(dt)` 和 `sys.render(view, proj, fbWidth, fbHeight, fovY)`

`ember_glfw`（窗口封装）只是可选便利模块，与核心库完全解耦。若你想"一个文件注入整个库"，直接用**单头文件版** `single_header/ember.hpp`（见 §5）。

---

## 1. 核心设计要点（为什么能嵌）

| 关注点 | ember 的做法 |
| --- | --- |
| GL 函数加载 | 宿主注入：`gl::init(void* (*)(const char*))`，兼容任意 `GetProcAddress` 风格加载器 |
| 上下文归属 | 不创建上下文、不 `glfwInit`、不占主循环；只做 `glGen*/glBufferData/glDispatchCompute/glDraw*` |
| 状态污染 | `render()` 只设置混合/深度并自行恢复 `glDepthMask(GL_TRUE)`；bloom 自建 FBO 链、结束后恢复宿主当前绑定的帧缓冲；其余状态（视口、清屏、自己的 VAO）由宿主控制 |
| 多实例 | 任意数量 `ParticleSystem` 可共存（各自持有 SSBO/程序），例如每场景一个 |

**嵌入最低代码量**（在已有 GL 上下文的项目里）：

```cpp
#include "ember/particle_system.hpp"

ember::gl::init(/* 你的 loader，见下文各栈 */);
ember::ParticleSystem sys({100000, 30000});
sys.setGravity({0, -9.81f, 0});

// 每帧：
sys.update(dt);
sys.render(view, proj, (float)fbWidthPx, (float)fbHeightPx, fovYDeg);
```

## 2. 依赖清单

| 依赖 | 角色 | 必需 | 获取 / 说明 |
| --- | --- | --- | --- |
| OpenGL **4.3 core** | 运行时 | ✅ | 驱动；compute shader + SSBO 是硬依赖（macOS 4.1 封顶，见 §9） |
| **glad**（v2，`gl:core=4.3`） | GL 入口加载 | ✅ | 生成 `glad.h`（include 路径）+ `glad.c`（编译进工程）；CMake 路径由 FetchContent 自动拉取（需 Python 3 + jinja2 生成），离线用 `deps/glad-v2.0.8.tar.gz` |
| **glm**（≥ 0.9.9） | 数学（仅头文件） | ✅ | 仓库自带 `deps/glm-1.0.1.tar.gz` 或系统包 |
| **GLFW**（3.4） | 窗口（示例 / `ember_glfw` / `EMBER_USE_GLFW`） | 可选 | 核心库不需要；单头文件版用 `EMBER_USE_GLFW` 开启窗口模块 |
| **stb_image** | PNG 贴图（`EMBER_USE_STB`） | 可选 | 仓库自带 `third_party/stb/`；未定义时 `setSpriteTexture` 回退内置渐变 |
| CMake ≥ 3.16 | 构建 | 可选 | 非 CMake 项目直接编译源码或单头文件 |
| Python 3 + jinja2 | 仅 FetchContent 拉 glad 时 | 可选 | glad 生成器 |

> **单头文件版依赖**（§5）：glad（`glad.h` + `glad.c`）+ glm 为必需；GLFW 仅当定义 `EMBER_USE_GLFW`；stb_image 仅当定义 `EMBER_USE_STB`（并在某 TU 定义 `STB_IMAGE_IMPLEMENTATION`）。

## 3. 嵌入 CMake 项目（三种方式）

### 3.1 add_subdirectory（源码即依赖，最简单）

```cmake
add_subdirectory(third_party/ember)          # 库源码放进你的项目
target_link_libraries(your_app PRIVATE ember)          # 不需要窗口封装
# 或带窗口封装： PRIVATE ember_glfw
```

依赖（glad/glm/glfw）默认由 ember 自己 FetchContent 拉取，你在 `add_subdirectory` **之前**声明同名 FetchContent 即可复用你自己的副本：

```cmake
include(FetchContent)
FetchContent_Declare(glm  GIT_REPOSITORY ...)
FetchContent_Declare(glad GIT_REPOSITORY ...)
FetchContent_Declare(glfw GIT_REPOSITORY ...)
add_subdirectory(third_party/ember)
```

### 3.2 FetchContent（从远程拉取 ember）

```cmake
include(FetchContent)
FetchContent_Declare(ember
  GIT_REPOSITORY https://github.com/you/ember
  GIT_TAG        v0.1.0)
set(EMBER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)   # 只要库
FetchContent_MakeAvailable(ember)
target_link_libraries(your_app PRIVATE ember)
```

### 3.3 install + find_package（预编译分发）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --install build --prefix ~/local
# 之后在任意项目：
#   -DCMAKE_PREFIX_PATH=~/local
```

```cmake
find_package(ember REQUIRED)
target_link_libraries(your_app PRIVATE ember::ember)       # 核心
# 窗口封装： ember::ember_glfw（需要 glfw3）
```

> 注意：`ember::ember` 的导出会引用 `glad`/`glm` 目标，消费方需先提供它们（自己的 FetchContent 或已安装）。`emberConfig.cmake` 会尝试 `find_dependency(glad/glm/glfw3)`。

## 4. 非 CMake 工作流

核心库只有 **3 个 .cpp**（其中一个可选），任何构建系统都能吃：

```
编译：src/shader.cpp  src/particle_system.cpp  [src/stb_image.cpp — PNG 贴图，可选]
头文件：include/  （即 #include "ember/particle_system.hpp"）
外部依赖：glad（生成的 glad.c 一并编译）、glm（仅头文件，加 include 路径）
链接：-lGL（Linux）/ opengl32（Windows）以及你的窗口库
```

- **Makefile / Bazel / Meson / Xcode / Visual Studio**：把上述文件加进工程即可，无需 CMake。
- **去掉 stb**：不编译 `src/stb_image.cpp`、不加 `EMBER_USE_STB` 宏 → `setSpriteTexture` 回退内置渐变。
- **预编译静态库**：`ember.a`（glad 需要打进同一个库或单独链接），分发头文件目录 + 库文件，用户侧手动加 include 路径。

## 5. 单头文件版（single_header/ember.hpp）

一个头文件注入整个库——声明 + 实现都在里面，**无需编译任何 ember 源码**：

```cpp
// 某个 .cpp（整个工程只允许一个 TU 这样写）：
#define EMBER_IMPLEMENTATION      // 展开实现段（编译本 TU 内）
#define EMBER_USE_GLFW            // 可选：GLFW 窗口便利模块
#define EMBER_USE_STB             // 可选：PNG 贴图（还需在某个 TU 定义 STB_IMAGE_IMPLEMENTATION）
#include "ember.hpp"

int main() {
    ember::Window win(1280, 720, "demo");   // 内部完成 4.3 core 上下文 + gl::init
    ember::ParticleSystem sys({100000, 30000});
    sys.setGravity({0.f, -9.81f, 0.f});
    // 主循环：win.pollEvents(); sys.update(dt); sys.render(view, proj, w, h, fov);
}
```

- **生成方式**：`single_header/ember.hpp` 由 `tools/amalgamate.py` 从模块化源码生成（改库后运行 `python tools/amalgamate.py` 重新生成；`--verify` 可检查漂移）。
- **依赖**（见 §2）：glad（`glad.h` + `glad.c` 编译进工程）、glm；GLFW / stb_image 按宏开启。**没有定义这些宏时**，核心粒子功能照常工作，只是没有窗口模块与 PNG 贴图。
- **与模块化版的关系**：同一份代码、同一契约；单头文件是发布形态，模块化源码是开发形态。`tests/single_header_test.cpp` 用单头文件编译整库并通过全部断言（含像素检查），保证两者等价。
- **自定义 shader**：契约与模块化版完全一致（§8）；`shaders/` 目录文件优先加载，缺失回退内嵌副本。

## 6. 逐技术栈接入

### 6.1 GLFW（现成：ember_glfw / EMBER_USE_GLFW）

```cpp
#include "ember/glfw_window.hpp"
ember::Window win(1280, 720, "demo");   // 内部完成 4.3 core 上下文 + gl::init
// 主循环里：win.pollEvents(); sys.update(win.deltaTime()); ...
```

### 6.2 SDL2

```cpp
#include <SDL.h>
#include "ember/particle_system.hpp"

SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

SDL_Window* w = SDL_CreateWindow("demo", 0, 0, 1280, 720, SDL_WINDOW_OPENGL);
SDL_GLContext ctx = SDL_GL_CreateContext(w);
SDL_GL_MakeCurrent(w, ctx);

ember::gl::init(reinterpret_cast<void* (*)(const char*)>(SDL_GL_GetProcAddress));

ember::ParticleSystem sys({100000, 30000});
// 主循环：SDL_Event 处理 -> sys.update(dt) -> glClear -> sys.render(...) -> SDL_GL_SwapWindow
```

### 6.3 Qt（QOpenGLWidget）

```cpp
// widget.h
#include <QOpenGLWidget>
#include <QMatrix4x4>
#include "ember/particle_system.hpp"

class ParticleView : public QOpenGLWidget {
protected:
    void initializeGL() override {
        ember::gl::init(reinterpret_cast<void* (*)(const char*)>(
            QOpenGLContext::currentContext()->getProcAddress));
        sys = std::make_unique<ember::ParticleSystem>(ember::ParticleSystem::Settings{100000, 30000});
        sys->setGravity({0.f, -9.81f, 0.f});
    }
    void paintGL() override {
        glViewport(0, 0, width(), height());
        glClearColor(0.02f, 0.02f, 0.04f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        QMatrix4x4 v, p;
        v.lookAt({0.f, 3.f, 10.f}, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
        p.perspective(50.f, aspectRatio(), 0.05f, 500.f);
        glm::mat4 view = glm::make_mat4(v.constData());
        glm::mat4 proj = glm::make_mat4(p.constData());
        sys->render(view, proj, (float)width(), (float)height(), 50.f);
    }
private:
    std::unique_ptr<ember::ParticleSystem> sys;
};
```

关键点：
- 构造 `QSurfaceFormat` 时设 `setVersion(4, 3)` + `setProfile(CoreProfile)`，或在 `initializeGL` 里校验 `QOpenGLContext::currentContext()->format()`。
- **所有 ember 调用必须发生在 paintGL（GL 线程）**，不能在 UI 线程调用。
- Qt 6 需要 `QOpenGLContext::currentContext()->getProcAddress` 的 `QFunctionPointer` 转换；也可用 `QOpenGLFunctions_4_3_Core` 前置加载后让 glad 复用，但直接 `gl::init` 最简单。

### 6.4 Win32 / WGL

```cpp
#include <windows.h>
#include "ember/particle_system.hpp"

static void* winLoader(const char* name) {
    // wglGetProcAddress 覆盖 1.1 之后的核心/扩展函数；
    // 极少数基础函数（如 glGetString）要回退 GetProcAddress。
    void* p = reinterpret_cast<void* (*)(const char*)>(wglGetProcAddress(name));
    if (!p) {
        HMODULE m = GetModuleHandleA("opengl32.dll");
        if (m) p = reinterpret_cast<void* (*)(const char*)>(GetProcAddress(m, name));
    }
    return p;
}

// 创建窗口 -> 选择像素格式 -> wglCreateContext -> wglMakeCurrent 之后：
ember::gl::init(winLoader);
```

### 6.5 EGL / 无头 / 离屏

```cpp
// 与 WGL 相同模式，loader 用 eglGetProcAddress（同样需回退库函数）：
ember::gl::init(reinterpret_cast<void* (*)(const char*)>(eglGetProcAddress));
```

无窗口渲染（离屏 FBO、服务器端、测试）：创建 EGL pbuffer 上下文或 `EGL_KHR_surfaceless_context`，`sys.update(dt)` 照常跑 compute；`render()` 需要真实视口，离屏时绑定 FBO 后传入其宽高与 fov 即可。

## 7. 与宿主渲染共存

- **深度**：粒子默认 additive、关闭深度测试、不写深度。若场景需要粒子被几何遮挡：`sys.setDepthTest(true); sys.setDepthWrite(true);`（先画不透明几何，再画粒子）。
- **软粒子**：需要"粒子靠近遮挡面时淡出"时，宿主先把场景深度渲染进一张深度纹理（`GL_DEPTH_COMPONENT24`，`Texture::uploadDepth` 可用），再 `sys.setSoftParticles(true, depthTexId, radius)`；每帧在粒子之前更新该深度即可。
- **bloom 后处理**：`sys.setBloom(true)` 时 `render()` 内部使用自带 FBO 链（RGBA16F 半分辨率提亮/模糊），结束后恢复宿主当前绑定的帧缓冲；宿主无需任何改动。
- **混合状态**：`render()` 每次自行设置混合函数，渲染后不恢复混合开关——如与宿主冲突，宿主在粒子之后重设自己的状态即可（render 结束后 `glDisable(GL_BLEND)`）。
- **视口/裁剪**：`render()` 不调用 `glViewport`/`glScissor`，由宿主负责。
- **尺寸变化**：窗口 resize 后重设视口即可，无需重建系统（每帧从 `framebufferSize()` 传高度）。
- **多系统**：多个 `ParticleSystem` 实例互不干扰；注意每个实例各编译一份默认 shader（可接受；如需极致共享，用 `setPrograms` 注入同一份 Shader）。
- **线程**：所有 ember 调用必须在拥有 GL 上下文的线程（Qt 的 paintGL、渲染线程等）。

## 8. 自定义 shader / 力场扩展契约

默认 GLSL 在 `shaders/`（规范副本，内嵌于 `src/particle_system.cpp` 的为运行时实际使用）。复制修改后：

```cpp
sys.setPrograms(
    ember::Shader::fromFiles({{GL_VERTEX_SHADER, "my.vert"},
                              {GL_FRAGMENT_SHADER, "my.frag"}}),
    ember::Shader::fromFiles({{GL_COMPUTE_SHADER, "my.comp"}}));
```

契约（必须保持，否则行为未定义）：

| Binding | 缓冲 | 说明 |
| --- | --- | --- |
| 0 | `cur` / `particles` | sim 只读；渲染 VS 只读（`Particle` 数组，std430） |
| 1 | `nxt` | sim 写（整结构写） |
| 2 | `req` | 出生请求（`SpawnRequest[]`，std430，见 emitters.hpp），只读；GPU 负责采样出生 |
| 3 | `dead` | free-stack，`coherent` |
| 4 | `counters` | `uAlive uDeadHead uSpawnRequestCount uCapacity`，`coherent` |
| 5 | `attractors` | `vec4` 数组，只读 |
| 6 | `vortexes` | `Vortex` 数组（`vec4 center` + `vec4 axisStrength`），只读 |
| 7 | `springs` | `Spring` 数组（`vec3 anchor` + 2 floats），只读 |
| 8 | `sorted` | 排序后的粒子索引（渲染 VS 只读；仅 `uUseSorted=1` 时使用） |
| 9 | `palette` | 调色板颜色 `vec4[]`（GPU 出生取色），只读 |
| 10 | `indirect` | 间接绘制参数（`vertexCount/instanceCount/firstVertex/baseInstance`；phase=2 写入，渲染 `glDrawArraysIndirect` 读取） |

> ⚠️ **破坏性变更（0.x）**：binding 2 从"完整 `Particle[]` 暂存"改为"`SpawnRequest[]` 出生请求"——自写出生 shader 的宿主需按新契约迁移。

- sim uniform：`uDt uTime uPhase uForceMask uGravity uDrag uDragMode uWind uTurbulence uAttractorCount uVortexCount uSpringCount uNoiseWindDir uNoiseWindAmp uNoiseWindScale uNoiseWindSpeed uWaveDir uWaveK uWaveAmp uWaveOmega uBoundaryMode uBoundaryY uRestitution uSpawnTotal uFrameSeed`（`uPhase=0` 积分、`=1` 出生、`=2` 写间接绘制参数，三个 dispatch；`uSpawnTotal`=本帧请求总粒子数、`uFrameSeed`=GPU 出生 RNG 种子；`uForceMask` bit i = `ember::Force` 枚举值 i，关闭的力零开销）
- 渲染 VS uniform：`uView uProj uSizeScale uStreak uUseSorted`（`uUseSorted=1` 时粒子索引取 `sorted[gl_InstanceID]`，=0 直接 `gl_InstanceID`；`uStreak>0` 时四边形沿速度投影方向拉伸）；渲染 FS uniform：`uSprite uUseSprite uSheetCols uSheetRows uSceneDepth uInvViewProj uSoftRadius uViewportSize uUseSoft`（`uUseSprite=1` 走贴图采样 + 帧动画，`=0` 程序化光斑；`uUseSoft=1` 时按场景深度淡出；`vFadeRGB` varying 来自 `life.yzw`，实现熄灭渐变）
- 绘制调用：`glDrawArraysIndirect(GL_TRIANGLE_STRIP, 0)`——每粒子 4 个顶点，VS 由 `gl_VertexID` 在视图空间展开四边形，无顶点属性；实例数来自 binding 10 的 GPU 写入参数（CPU 读回不参与渲染）
- `Particle` 布局（CPU/GPU 必须一致，64 字节）：`pos(xyz,size) vel(xyz,age) life(lifetime; <0=尸体) color(rgba)`
- **常见扩展点**：在 `simulate()` 的力累加段加自定义力（涡旋、弹簧、噪声场、碰撞）；在 VS 里换 billboard 尺寸/形状（或改拖尾拉伸、朝向逻辑）；在 FS 里换光斑曲线或加纹理；排序可换 `sort.comp` 的 key（如按距离而非深度）。

## 9. 兼容性与降级（macOS / OpenGL 4.1）

### 兼容性矩阵

| 平台 | GL 版本 | ember 状态 |
| --- | --- | --- |
| Windows / Linux（NVIDIA/AMD/Intel 驱动） | 4.3+ | ✅ 全功能 |
| macOS | 最高 **4.1** | ❌ 不支持（见下） |
| Web / WASM | 无 GL 4.3 | ❌ 需移植（WebGPU 等） |
| 嵌入式 / 低端 GPU | < 4.3 | ❌ 需降级路径（见下） |

**根因**：ember 的两处核心依赖都是 4.3 特性——**compute shader**（GPU 仿真）和 **SSBO**（粒子缓冲，渲染 VS 也直接读它）。macOS 从 2018 年起不再升级 OpenGL，停留在 4.1，这两项都不可用。

### 降级路径（工作量递增，均为独立工程）

**路径 A：CPU 仿真（最小改动，兼容 3.3+）**
每帧在 CPU 上完成积分（复用 `Emitter` 出生逻辑与力场公式），把结果写进粒子缓冲；渲染侧把"VS 读 SSBO"改成**属性式实例化**（`glVertexAttribDivisor`，3.3 特性）：

```cpp
// 每帧：
std::vector<Particle> cpu;
for (std::size_t i = 0; i < sys.emitterCount(); ++i)
    sys.emitter(i)->spawn(cpu, maxN, dt, rng);
integrateCpu(cpu, dt, gravity, attractors, ...);   // 你的 CPU 积分循环
glBindBuffer(GL_ARRAY_BUFFER, particleVbo);
glBufferData(GL_ARRAY_BUFFER, cpu.size() * sizeof(Particle), cpu.data(), GL_DYNAMIC_DRAW);
glDrawArraysInstanced(GL_POINTS, 0, 1, (GLsizei)cpu.size()); // 属性 0/1/2/3 各取一个分量
```

**路径 B：transform feedback 仿真（仍全 GPU，4.1 可用）**
把 compute 积分移到 **vertex shader + glTransformFeedback**（老牌 4.1 技术）：每粒子一个顶点，VS 里积分并把结果写回缓冲区，再以 feedback 缓冲作为下一帧输入。渲染同样改属性式实例化。性能接近 compute，但实现量最大（双缓冲、primitive restart 语义、attribute 布局对齐）。

**路径 C：Metal 移植**
仿真内核（积分 + free-stack 回收 + 力场）直接翻译成 Metal compute，渲染换成 Metal 管线。外观参数（配置文件、发射器、力场）全部复用，只有渲染/仿真后端替换。这是 macOS 上体验最好的方案，也是 Apple 官方推荐方向（Metal 3 全面取代 OpenGL）。

**结论**：ember 本身**不内置** 4.1/Metal 降级——GPU 仿真和 SSBO 渲染是它的核心设计。若你有 macOS/低端 GPU 的硬需求，建议按路径 A 起步（改动最小），或按路径 C 立项做 Metal 后端；本项目欢迎以独立后端（`EMBER_BACKEND_METAL` 之类）的形式贡献。

## 10. FAQ

**Q：公司内网 / 无外网环境怎么构建？**
仓库自带依赖包：`cmake -S . -B build -DEMBER_DEPS_DIR=<deps 目录绝对路径>`，FetchContent 从本地解包，全程不联网。唯一例外：glad 生成器需要本机 Python 3 + `jinja2`，且首次生成会从 Khronos 拉取 gl.xml（生成结果缓存在构建目录，之后可离线）。

**Q：单头文件版怎么搭构建？**
核心是 glad（`glad.h` + `glad.c`）和 glm。把 `single_header/ember.hpp` 拷进工程，某个 .cpp 里 `#define EMBER_IMPLEMENTATION` 后 include 一次；glad.c 照常编译并 `ember::gl::init(loader)`。完整示例见 `tests/single_header_test.cpp`（只依赖单头文件 + glad/glfw/glm，不链 ember 库）。

**Q：PNG 贴图依赖 stb_image，能去掉吗？**
能。不定义 `EMBER_USE_STB`（模块化路径：不编译 `src/stb_image.cpp`），`setSpriteTexture` 回退内置渐变并打印警告。

**Q：示例里的交互式编辑器（WASD 移动发射器等）会进我的项目吗？**
不会。编辑器只在示例程序里，由 CMake 选项 `EMBER_BUILD_EDITOR`（默认 ON，仅作用于 example 目标）门控；`-DEMBER_BUILD_EDITOR=OFF` 或去掉示例即可完全排除。库本身不包含任何编辑器代码，运行时开关是 `[system] editor = false`。

**Q：我只有 OpenGL 3.3 怎么办？**
compute shader（4.3）是硬依赖。降级方案：CPU 仿真 + instanced 渲染（自己写积分循环写入粒子 SSBO，渲染管线仍可复用 VS/FS 与 `render()`）；或迁移到 Vulkan/D3D12 移植仿真内核。

**Q：macOS 能用吗？**
不能。macOS 最高 GL 4.1（无 compute）。替代：Metal 移植仿真逻辑，或 `render()` 前在 CPU/计算队列上更新粒子数据（见 §9）。

**Q：每帧读回 16 字节会不会卡？**
`glGetBufferSubData` 触发隐式同步，现代驱动开销约亚毫秒级（一次性）。渲染路径已不依赖该读回（实例数来自 GPU 间接参数），读回仅供 `aliveCount()`/UI。若对延迟敏感（VR/低延迟），可改为帧尾读回 + 帧首消费（多缓冲）。

**Q：粒子数量上限怎么选？**
稳态活粒子数 ≈ 总出生率 × 平均寿命。`capacity` 取该值再加余量；`maxSpawnPerFrame` 防止单帧洪峰。示例 7000/s × 2.75s ≈ 19k，配置 150k 留有大量余量。参考 `config/stress.ini`（容量 1M，约 85 万稳态）。

**Q：配置文件改了没生效？**
检查路径与 `[section]` 写法（`#` 注释、`key = value`、向量逗号分隔）。解析错误会抛出带行号的 `std::runtime_error`，示例会打印并保留旧配置。`blend`/`shape` 等枚举值不区分大小写。

**Q：出生率低于死亡率时槽位会泄漏吗？**
会有一部分槽位保持为"尸体"（视觉无影响，仅占容量）；长期不匹配时建议按稳态公式重配 capacity。环形 free-stack 的条目溢出时丢弃最旧者，不会产生双写竞态。
