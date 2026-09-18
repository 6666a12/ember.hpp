# ember 使用指南（嵌入不同技术栈）

本文档面向把 ember 嵌入**现有 C++ 项目**的开发者。核心结论：**ember 不创建 GL 上下文、不依赖窗口系统**——你只需要在已有的 GL 上下文上做两件事：

1. 用你的加载器初始化 glad：`ember::gl::init(loader)`
2. 每帧调用 `sys.update(dt)` 和 `sys.render(view, proj, fbWidth, fbHeight, fovY)`

`ember_glfw`（窗口封装）只是可选便利模块，与核心库完全解耦。若你想"一个文件注入整个库"，直接用**单头文件版** `single_header/ember.hpp`（见 §5）。

---

## 1. 核心设计要点（为什么能嵌）

| 关注点 | ember 的做法 |
| --- | --- |
| GL 函数加载 | 宿主注入：`gl::init(GLADloadfunc)`，兼容任意 `GetProcAddress` 风格加载器 |
| 上下文归属 | 不创建上下文、不 `glfwInit`、不占主循环；只做 `glGen*/glBufferData/glDispatchCompute/glDraw*` |
| 状态管理 | render() 恢复读/写 FBO、视口、深度状态、面剔除及 scissor 开关；混合、程序、VAO、SSBO、纹理由宿主在后续绘制前重新设置 |
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

下表对应 OpenGL 后端。公共层仅依赖 glm，使用 `ember/system.hpp` 与注入后端构造函数，详见 [后端说明](docs/backends.md)。

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
find_package(ember REQUIRED COMPONENTS opengl)
target_link_libraries(your_app PRIVATE ember::opengl)      # core + OpenGL
# 窗口封装：COMPONENTS glfw + ember::ember_glfw（需要 glfw3）
```

> 包配置接受消费方已有的依赖目标；缺少时通过 `find_dependency` 寻找所选组件的依赖。`COMPONENTS core` 只需要 glm；`opengl` 再需要 glad；`glfw` 再需要 GLFW。不写组件时加载该安装中全部已构建模块。`ember::ember` 保留为 OpenGL 兼容目标。

源码构建可设置 `EMBER_BUILD_OPENGL=OFF` 只生成 `ember::core`，或设置 `EMBER_BUILD_GLFW=OFF` 保留 GL 后端、去掉窗口模块。默认均为 ON。

## 4. 非 CMake 工作流

公共层编译 `src/particle_system.cpp`，GL 后端额外编译以下后端文件（完整清单也见 `cmake/EmberOpenGLSources.cmake`）：

```
src/particle_system.cpp
src/backends/opengl/compat.cpp
src/backends/opengl/resources.cpp
src/backends/opengl/simulation.cpp
src/backends/opengl/statistics.cpp
src/backends/opengl/sort.cpp
src/backends/opengl/render.cpp
src/backends/opengl/shader.cpp
src/backends/opengl/shaders.cpp
可选：src/backends/opengl/stb_image.cpp（PNG），src/glfw_window.cpp（窗口）
头文件：include/  （即 #include "ember/particle_system.hpp"）
外部依赖：glad（生成的 glad.c 一并编译）、glm（仅头文件，加 include 路径）
链接：-lGL（Linux）/ opengl32（Windows）以及你的窗口库
```

- **Makefile / Bazel / Meson / Xcode / Visual Studio**：把上述文件加进工程即可，无需 CMake。
- **去掉 stb**：不编译 `src/backends/opengl/stb_image.cpp`、不加 `EMBER_USE_STB` 宏 → `setSpriteTexture` 回退内置渐变。
- **预编译静态库**：链接 `ember`、`ember_core` 和 glad；使用窗口模块时再加 `ember_glfw`、GLFW。分发头文件目录和相应库文件。此次类布局变化，消费方需要重新编译。

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

ember::gl::init(+[](const char* name) -> GLADapiproc {
    return reinterpret_cast<GLADapiproc>(SDL_GL_GetProcAddress(name));
});

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
        ember::gl::init(+[](const char* name) -> GLADapiproc {
            return reinterpret_cast<GLADapiproc>(QOpenGLContext::currentContext()->getProcAddress(name));
        });
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

static GLADapiproc winLoader(const char* name) {
    PROC p = wglGetProcAddress(name);
    const auto address = reinterpret_cast<std::intptr_t>(p);
    if (!p || address == 1 || address == 2 || address == 3 || address == -1) {
        HMODULE m = GetModuleHandleA("opengl32.dll");
        p = m ? GetProcAddress(m, name) : nullptr;
    }
    return reinterpret_cast<GLADapiproc>(p);
}

// 创建窗口 -> 选择像素格式 -> wglCreateContext -> wglMakeCurrent 之后：
ember::gl::init(winLoader);
```

### 6.5 EGL / 无头 / 离屏

```cpp
// 与 WGL 相同模式，loader 用 eglGetProcAddress（同样需回退库函数）：
ember::gl::init(+[](const char* name) -> GLADapiproc {
    return reinterpret_cast<GLADapiproc>(eglGetProcAddress(name));
});
```

无窗口渲染（离屏 FBO、服务器端、测试）：创建 EGL pbuffer 上下文或 `EGL_KHR_surfaceless_context`，`sys.update(dt)` 照常跑 compute；`render()` 需要真实视口，离屏时绑定 FBO 后传入其宽高与 fov 即可。

## 7. 与宿主渲染共存

- **深度**：粒子默认 additive、关闭深度测试、不写深度。若场景需要粒子被几何遮挡：`sys.setDepthTest(true); sys.setDepthWrite(true);`（先画不透明几何，再画粒子）。
- **坐标/深度约定**：`render()` 的投影矩阵与宿主深度纹理遵循 GL 约定（NDC z ∈ [-1,1]、帧缓冲原点左下）；使用 [0,1] 深度或左上原点坐标的宿主需自行适配投影与深度导出。
- **软粒子**：需要"粒子靠近遮挡面时淡出"时，宿主先把场景深度渲染进一张深度纹理（`GL_DEPTH_COMPONENT24`，`Texture::uploadDepth` 可用），再 `setOpenGLSoftParticles(sys, true, depthTexId, radius)`；每帧在粒子之前更新该深度即可。
- **折射粒子**：`[emitter] refractive = true` 的粒子需要宿主把场景渲染进一张颜色纹理（RGBA、`CLAMP_TO_EDGE`，`Texture::uploadRGBA8` 自带），`setOpenGLRefraction(sys, settings)`（`settings.sceneColorTex`）传入；折射 pass 在 bloom 合成后以像素替换直出宿主帧缓冲，高光靠 specular 项（不吃 bloom）。示例见 `examples/glass.cpp` 的 `ScenePass`。
- **bloom 后处理**：`sys.setBloom(true)` 时 `render()` 内部使用自带 FBO 链（RGBA16F 半分辨率提亮/模糊），结束后恢复宿主当前绑定的帧缓冲；宿主无需任何改动。

折射通道同样遵守显式的 `setDepthTest` / `setDepthWrite`；默认均关闭。折射覆盖度包含粒子 alpha、寿命淡出及 sprite 遮罩；完全透明的片元不写深度。depth 模式使用 `ior` 弯折视线，`ior=1` 不产生偏折，`strength` 缩放投影后的偏移。simple/noise 的 `strength` 仍是 UV 偏移。`spin` 支持正负角速度；`soft_radius` 必须大于零。效果边界与验证范围见 [效果逻辑审查](docs/effects-audit.md)。
- **混合状态**：`render()` 每次自行设置混合函数，渲染后不恢复混合开关——如与宿主冲突，宿主在粒子之后重设自己的状态即可（render 结束后 `glDisable(GL_BLEND)`）。
- **视口/裁剪**：render() 使用传入尺寸和 (0,0) 原点，结束后恢复宿主视口；不修改 scissor。
- **尺寸变化**：窗口 resize 后重设视口即可，无需重建系统（每帧从 `framebufferSize()` 传高度）。
- **多系统**：多个 `ParticleSystem` 实例互不干扰；注意每个实例各编译一份默认 shader（可接受；如需极致共享，用 `setOpenGLPrograms` 注入同一份 Shader）。
- **线程**：所有 ember 调用必须在拥有 GL 上下文的线程（Qt 的 paintGL、渲染线程等）。

加载回调采用 `GLADloadfunc`。GLFW 可直接传 `glfwGetProcAddress`；SDL/Qt/EGL 使用上面的有类型适配函数，转换返回地址而不是强转回调。GPU 对象必须在所属上下文仍 current 时销毁。GLFW 回调异常在同线程下一次事件处理包装调用中重新抛出。

自定义 Shader 协议：binding 4 现在分配八个 uint（32 B），前五个仍为 `uAlive/uDeadHead/uSpawnRequestCount/uCapacity/uAllocated`，尾部依次为 `uSpawnReuse/uSpawnAppendBase/uSpawnAccepted`。默认同步模式读取前 20 B，GPU 模式异步采样；`Spring[]` 步长为 32 B，深度使用 `uInvProj = inverse(proj)`。

- 内置仿真声明 `uInputAlive`，宿主据此选择优化协议。phase 0 遍历 binding 11 当前存活索引，写入 binding 12 下一帧索引，并把幸存数累计到间接 instanceCount；死亡槽位在两个粒子缓冲中都写墓碑。phase 3 用单 invocation 批量预留回收/追加槽位，更新尾部三个字段及计数。phase 1 采样出生、追加下一帧索引。phase 2 用单 invocation 发布最终间接数量。每阶段之间有 SSBO barrier，最终再加 command/buffer-update barrier；更新后交换粒子缓冲和索引缓冲。
- 旧版自定义仿真不声明 `uInputAlive` 时，仍按 `uAllocated` 范围运行旧三阶段流程：phase 0 积分、phase 1 出生、phase 2 写 binding 11 存活索引和间接数量。前五个字段布局不变。从旧程序切回优化协议时，库会一次性同步两份粒子缓冲的墓碑。
- 排序 Shader 声明 `uTileMode` 时启用 256 线程工作组的分块协议，binding 12 改绑独立深度键缓冲：`uMode=0/uTileMode=1` 初始化及局部排序；`uMode=1/uTileMode=0` 执行 `uJ>=256` 的全局步骤；`uMode=1/uTileMode=2` 合并当前 `uK` 下的 `uJ=128..1`。不声明该 uniform 的旧排序 Shader 仍按原来的 64 线程、mode 0/1 协议调度。
- 采用优化协议的自定义 Shader 必须完整遵守上述布局与阶段语义，不能仅增加一个 uniform。排序仍接收 `uAlive/uCapacity`，以 capacity 为无效槽位哨兵；内置排序同深度时按稳定槽位编号确定顺序。CPU API、稳定槽位身份和即时 `aliveCount()` 语义保持不变。


### GPU 调度与异步统计（可选）

默认模式继续每次 update 同步读取计数。需要减少 CPU/GPU 同步时，在初始化阶段启用：

```cpp
sys.setGpuDriven(true);
// 每帧：
sys.update(dt);
sys.render(view, proj, width, height, fov);
const auto stats = sys.pollStatistics();
// stats.alive / stats.allocated 对应 stats.frame，不一定是当前帧。
const auto lag = sys.updateSequence() - stats.frame;
```

`update()` 和 `render()` 的积分、排序和绘制数量均由 GPU 决定，不依赖统计快照。`pollStatistics()` 只用零超时检查 fence，只读取已完成的独立暂存缓冲；无新结果时返回旧快照，初始为全零。四个采样槽全忙时跳过本次采样，`droppedStatistics()` 累计跳过次数，不等待 GPU 腾出槽位。

`Statistics::frame` 是完成采样的 update 序号；`updateSequence()` 是已提交的 update 序号。`clear()` 和容量重建会取消旧采样、发布已知的零计数，但不会重置序号；`allocated` 表示已分配槽位范围，包含空闲槽位。没有最后一帧采样完成的保证：需要当前精确值时调用 `synchronizeStatistics()`。`aliveCount()` 和 `readParticles()` 仍返回即时准确结果，因此在 GPU 模式可能同步等待；不要在性能关键循环里用它们更新界面。

关闭 GPU 模式会执行一次精确计数同步。统计查询、模式切换、更新、渲染和销毁仍需所属 GL 上下文 current，不能从 UI 线程直接调用 GL；将返回的普通统计结构传给 UI 即可。GPU 模式跳过 `EMBER_DEBUG` 的逐粒子同步读回日志。模式目前仅由 C++ API 启用，没有 INI 配置键。

GPU 调度协议：

- 内部 `schedule.comp` 始终使用内嵌版本，不由 `setShaderDirectory()` 替换。binding 13 为 uint 数组：word 0 是积分前 alive，word 1 是排序 padded N，word 2–4 是积分间接 dispatch 的 x/y/z，word 5 起是排序的连续三字命令。
- 调度 phase 0 重置 binding 10 绘制参数、写出生请求数并生成 `ceil(alive/64)` 积分命令；开启排序时，仿真 phase 0/3/1/2 完成后由调度 phase 1 生成排序命令。排序顺序为初始化，再依次 `k=512..nextPow2(capacity)`，每级 `j=k/2..256` 的全局步骤及一个局部尾步骤。超出实际 padded N 的步骤 x=0；空系统所有排序步骤 x=0。shader-storage / command barrier 保证间接命令可见。
- 自定义仿真需完整保留 `uInputAlive` 与 `uGpuDriven` 的有效 uniform 和新协议；`uGpuDriven=1` 时从 binding 13 word 0 读取积分上限，不能使用 CPU 的 `uInputAlive`。排序需 `uTileMode` 与 `uGpuDriven`，从 binding 4 获取 alive、从 binding 13 word 1 获取 padded N。同步模式 `uGpuDriven=0` 仍使用原 uniform。
- 不兼容的自定义程序在启用 GPU 模式、替换仿真或开启排序时抛出异常。同步模式保留旧 shader 兼容路径。检测 uniform 只是协议标识，不能验证自定义 shader 的完整行为。
- GPU 模式的容量受设备 X 维 compute 工作组数量限制：`ceil(capacity/64)` 和 `ceil(nextPow2(capacity)/256)` 都不能超限，启用/扩容时检查。GPU 排序按容量预留深度键缓存，CPU 提交容量对应的步骤，GPU 跳过不需要的步骤；小负载或容量远大于存活量时，提交开销仍可能高于同步模式。

该模式去掉为计数而强制等待的依赖；OpenGL 驱动仍可能因队列、资源分配等原因阻塞，不能据此承诺每个 API 调用都无等待。吞吐收益以 [性能基准](benchmarks/README.md) 为准。

## 8. 自定义 shader / 力场扩展契约

默认优先加载 `shaders/` 文件；仿真、粒子渲染及排序缺失或编译失败时回退内嵌副本，Bloom 文件缺失则关闭该效果。复制修改后：

```cpp
setOpenGLPrograms(sys.backend(), 
    ember::Shader::fromFiles({{GL_VERTEX_SHADER, "my.vert"},
                              {GL_FRAGMENT_SHADER, "my.frag"}}),
    ember::Shader::fromFiles({{GL_COMPUTE_SHADER, "my.comp"}}));
```

契约（必须保持，否则行为未定义）：

| Binding | 缓冲 | 说明 |
| --- | --- | --- |
| 0 | `cur` / `particles` | sim 读取并在退休时写墓碑；渲染 VS 只读（`Particle` 数组，std430） |
| 1 | `nxt` | 下一帧粒子：phase 0/1 写入 |
| 2 | `req` | 出生请求（`SpawnRequest[]`，std430，见 emitters.hpp），只读；GPU 负责采样出生 |
| 3 | `dead` | free-stack，`coherent` |
| 4 | `counters` | `uAlive uDeadHead uSpawnRequestCount uCapacity uAllocated uSpawnReuse uSpawnAppendBase uSpawnAccepted`，`coherent` |
| 5 | `attractors` | `vec4` 数组，只读 |
| 6 | `vortexes` | `Vortex` 数组（`vec4 center` + `vec4 axisStrength`），只读 |
| 7 | `springs` | `Spring[]`: std430 stride **32 B**, offsets anchor=0, stiffness=12, damping=16 |
| 8 | `sorted` | 排序后的粒子索引（渲染 VS 只读；仅 `uUseSorted=1` 时使用） |
| 9 | `palette` | 调色板颜色 `vec4[]`（GPU 出生取色），只读 |
| 10 | `indirect` | 间接绘制参数（`vertexCount/instanceCount/firstVertex/baseInstance`；phase=2 写入，渲染 `glDrawArraysIndirect` 读取） |
| 11 | `liveIndices` | 当前存活索引；积分/渲染/排序读取，更新后与下一帧索引交换 |
| 12 | `nextLiveIndices / sortKeys` | 仿真时绑定下一帧索引；排序时绑定独立的深度键缓存 |
| 13 | `schedule` | GPU 调度元数据与间接 compute 命令 |

> ⚠️ **破坏性变更（0.x）**：binding 2 从"完整 `Particle[]` 暂存"改为"`SpawnRequest[]` 出生请求"——自写出生 shader 的宿主需按新契约迁移。

- 内置 shader 不再使用散 uniform——标量参数打包为 std140 uniform 块（同一 GLSL 可编译 SPIR-V；CPU 镜像与布局断言在 `src/backends/opengl/params.hpp`）：仿真 `SimParams` binding 14、排序 `SortParams` binding 15、内部调度 `ScheduleParams` binding 16、渲染 VS `DrawParams` binding 17、渲染 FS `FragParams` binding 18、bloom `BloomParams` binding 19。块成员名沿用原散 uniform 名；例外：fragment 块内 `uProj`/`uRefraction` 改名 `uFragProj`/`uFragRefraction`（无实例名的块成员共享全局命名空间，与 vertex 块冲突）。仿真块成员：`uDt uTime uPhase uForceMask uGravity uDrag uDragMode uWind uTurbulence uAttractorCount uVortexCount uSpringCount uNoiseWindDir uNoiseWindAmp uNoiseWindScale uNoiseWindSpeed uWaveDir uWaveK uWaveAmp uWaveOmega uBoundaryMode uBoundaryY uRestitution uSpawnTotal uFrameSeed uInputAlive uGpuDriven`（`uPhase=0` 积分、`=3` 批量分配、`=1` 出生、`=2` 发布间接绘制参数；有出生时最多四个 dispatch；`uSpawnTotal`=本帧请求总粒子数、`uFrameSeed`=GPU 出生 RNG 种子；`uForceMask` bit i = `ember::Force` 枚举值 i，关闭的力零开销）
- 兼容路径：后端仍同步写旧的散 uniform（VS `uView uProj uSizeScale uStreak uUseSorted`、FS `uSprite uUseSprite uSheetCols uSheetRows uSceneDepth uInvProj uSoftRadius uViewportSize uUseSoft` 等），旧契约自定义 shader 无需修改；新自定义 shader 应声明同名块成员（协议检测对块成员同样生效）。采样器带显式 binding：`uSprite`=0、`uSceneDepth`=1、`uSceneColor`=2。`uUseSorted=1` 时粒子索引取 `sorted[gl_InstanceID]`，=0 取 `liveIndices[gl_InstanceID]`；`uStreak>0` 时四边形沿速度投影方向拉伸；`uUseSprite=1` 走贴图采样 + 帧动画，`=0` 程序化光斑；`uUseSoft=1` 时按场景深度淡出；`vFadeRGB` varying 来自 `life.yzw`，实现熄灭渐变
- 构建期 SPIR-V：`-DEMBER_BUILD_SPIRV=ON`（默认）且找到 glslangValidator 时，`ember_spirv` 目标把 `shaders/` 全部编译到 `build/generated/spirv/`（`--target-env vulkan1.0`），Vulkan 后端直接加载这些二进制，无运行时编译器依赖
- 绘制调用：`glDrawArraysIndirect(GL_TRIANGLE_STRIP, 0)`——每粒子 4 个顶点，VS 由 `gl_VertexID` 在视图空间展开四边形，无顶点属性；实例数来自 binding 10 的 GPU 写入参数（CPU 读回不参与渲染）
- **折射粒子（玻璃渣）**：`[emitter] refractive = true`（`BurstParams::refractive`）的粒子符号编码在 `pos.w`（负尺寸），渲染走独立 pass——像素替换（混合关闭）、FS 采样宿主场景纹理（**纹理单元 2** `uSceneColor`）；新增 FS uniform：`uRefraction uRefrMode uRefrShape uRefrDome uRefrStrength uRefrIor uRefrTint uRefrAbsorption uRefrFresnel uRefrChroma uRefrSpecular uRefrLightDir`；碎片轮廓可选程序多边形（`uRefrShape=1`：3–5 边、逐边抖动），`uRefrDome` 控制曲面法线强度（剪影 rim 光 + 点状高光，0 = 平面片）；预设 `ember::Refraction::glass()/heat()/water()/prism()`，宿主把场景渲染进颜色纹理后 `setOpenGLRefraction(sys, ...)` 传入（见 §7）
- **通用 billboard 自旋**：`sys.setSpin(speed)` / `[system] spin`——VS 按稳定槽位哈希角 + 年龄自旋（`uSpinSpeed` uniform）旋转 quad。明显拉伸时 `streak` 决定朝向；额外拉伸不足 10% 时向普通朝向过渡，静止时保留自旋。折射面片与其法线使用同一最终角度，见 [拖尾说明](docs/streak.md)。
- `Particle` 布局（CPU/GPU 必须一致，64 字节）：`pos(xyz,size) vel(xyz,age) life(lifetime; <0=尸体) color(rgba)`
- **常见扩展点**：在 `simulate()` 的力累加段加自定义力（涡旋、弹簧、噪声场、碰撞）；在 VS 里换 billboard 尺寸/形状（或改拖尾拉伸、朝向逻辑）；在 FS 里换光斑曲线或加纹理；排序可换 `sort.comp` 的 key（如按距离而非深度）。

### Vulkan 宿主接入（可选后端）

`ember::vulkan` 与 GL 后端并列，功能与 GL 对齐（GPU 调度、深度排序、PNG 贴图、软粒子、折射、Bloom）。最小接入流程：

```cpp
#include "ember/system.hpp"
#include "ember/vulkan.hpp"

ember::VulkanDevice device = ember::makeVulkanDevice();   // 自建设备；生产环境可换成宿主已有 device
auto backend = ember::makeVulkanBackend();
ember::setVulkanContext(*backend, device.context());       // 借用 instance/physicalDevice/device/queue
ember::ParticleSystem sys({100000, 30000}, std::move(backend));

// 每帧：把宿主的帧目标（颜色/深度 view）注入后再渲染
ember::VulkanFrameTarget target{};
target.colorView = myColorView;   // 布局须为 COLOR_ATTACHMENT_OPTIMAL
target.depthView = myDepthView;   // 可选；布局须为 DEPTH_STENCIL_ATTACHMENT_OPTIMAL
target.colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
target.depthFormat = VK_FORMAT_D32_SFLOAT;
target.width = 1920; target.height = 1080; target.frameIndex = frameCounter;
ember::setVulkanFrameTarget(sys.backend(), target);

sys.update(dt);
sys.render(view, proj, 1920.f, 1080.f, 50.f);
```

- **坐标/深度**：`render()` 接受与 GL 完全相同的投影矩阵；顶点着色器经 `EMBER_CLIP_VULKAN` 把 clip z 重映射到 Vulkan 的 `[0,w]`，并用负 viewport 高度翻 Y，因此屏幕 UV/`gl_FragCoord` 保持左下原点。使用零到一投影的 Vulkan 宿主得到数值一致的深度，可直接把宿主深度纹理交给软粒子/折射。
- **注入资源只借用不接管**：context、帧目标、软粒子/折射的 view/sampler 都由宿主拥有，必须在后端使用期间保持有效。后端不接管交换链、不发跨资源 layout barrier（进入/退出 render 的布局由宿主保证）。
- **软粒子/折射**：`setVulkanSoftDepth(backend, depthView, sampler)`（采样器需 clamp + linear；view 为 `SHADER_READ_ONLY_OPTIMAL`）；`setVulkanRefractionInputs(backend, colorView, depthView, sampler)`。折射 mode 1 无深度时回退 mode 0，与 GL 一致。
- **Bloom**：`sys.setBloom(true)` 后 `render()` 使用自有的 RGBA16F 全/半分辨率链；宿主深度 view 会作为 HDR 粒子 pass 的深度附件直接复用（`loadOp=LOAD`），在宿主深度为 `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`、格式为 `D16/D32/D24S8/D32S8` 时自动开启遮挡，否则跳过（对照 GL 的 `bits==0` 分支）。任何 bloom 资源创建失败会关闭 bloom 并打印警告，不抛出。
- **单头文件版仍为 GL-only**，Vulkan 后端只随模块化构建提供。
- **验证**：`EMBER_VK_DEBUG=1` 时 `makeVulkanDevice()` 会启用校验层（缺失则静默降级）。多后端共存无需特殊处理：GL context 与 VkDevice 可同时存活，各自使用自己的句柄。
- **录入宿主 command buffer（WO-10）**：宿主引擎自己管理 command buffer 时，可用窗口模式让 `update()`/`render()` 录进它，而不是后端自提交：

```cpp
VkCommandBuffer cmd = /* 宿主已 vkBeginCommandBuffer 的 cmd */;
ember::beginVulkanFrame(sys.backend(), cmd, frameCounter);
sys.update(dt);
sys.render(view, proj, 1920.f, 1080.f, 50.f);
ember::endVulkanFrame(sys.backend());
/* 宿主自行 vkEndCommandBuffer + vkQueueSubmit + vkWaitForFences */
auto stats = sys.synchronizeStatistics(); // 或 aliveCount()/readParticles()
```

  窗口内后端不自提交、不 signal fence、不轮换帧环；宿主必须遵守 `kFramesInFlight=2` 的资源纪律（第 N 帧 cmd 在飞时不得给同帧槽录第 N+2 帧），并在调用精确统计/读回前先提交并等待自己的 cmd。`synchronizeStatistics()`/`aliveCount()`/`readParticles()` 在窗口内调用会抛 `std::logic_error`；`setVulkanFrameTarget` 须在 `beginVulkanFrame` 之前设置，且其 `frameIndex` 必须与 `beginVulkanFrame` 的 `frameIndex` 同槽。

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
仓库自带依赖包：`cmake -S . -B build -DEMBER_DEPS_DIR=<deps 目录绝对路径>`，FetchContent 从本地解包，全程不联网。glad 生成器需要本机 Python 3 + `jinja2`；构建启用 `REPRODUCIBLE`，使用依赖包内的规范，不再在线拉取 gl.xml。

**Q：单头文件版怎么搭构建？**
核心是 glad（`glad.h` + `glad.c`）和 glm。把 `single_header/ember.hpp` 拷进工程，某个 .cpp 里 `#define EMBER_IMPLEMENTATION` 后 include 一次；glad.c 照常编译并 `ember::gl::init(loader)`。完整示例见 `tests/single_header_test.cpp`（只依赖单头文件 + glad/glfw/glm，不链 ember 库）。

**Q：PNG 贴图依赖 stb_image，能去掉吗？**
能。不定义 `EMBER_USE_STB`（模块化路径：不编译 `src/backends/opengl/stb_image.cpp`），`setSpriteTexture` 回退内置渐变并打印警告。

**Q：示例里的交互式编辑器（WASD 移动发射器等）会进我的项目吗？**
不会。编辑器只在示例程序里，由 CMake 选项 `EMBER_BUILD_EDITOR`（默认 ON，仅作用于 example 目标）门控；`-DEMBER_BUILD_EDITOR=OFF` 或去掉示例即可完全排除。库本身不包含任何编辑器代码，运行时开关是 `[system] editor = false`。

**Q：我只有 OpenGL 3.3 怎么办？**
compute shader（4.3）是硬依赖。降级方案：CPU 仿真 + instanced 渲染（自己写积分循环写入粒子 SSBO，渲染管线仍可复用 VS/FS 与 `render()`）；或迁移到 Vulkan/D3D12 移植仿真内核。

**Q：macOS 能用吗？**
不能。macOS 最高 GL 4.1（无 compute）。替代：Metal 移植仿真逻辑，或 `render()` 前在 CPU/计算队列上更新粒子数据（见 §9）。

**Q：每帧读回 20 字节会不会卡？**
默认模式可能等待 GPU。启用 `setGpuDriven(true)` 并用 `pollStatistics()` 更新界面，可去掉逐帧计数依赖（见第 7 节）。显式精确查询仍可能同步等待。

**Q：粒子数量上限怎么选？**
稳态活粒子数 ≈ 总出生率 × 平均寿命。`capacity` 取该值再加余量；`maxSpawnPerFrame` 防止单帧洪峰。示例 7000/s × 2.75s ≈ 19k，配置 150k 留有大量余量。参考 `config/stress.ini`（容量 1M，约 85 万稳态）。

**Q：配置文件改了没生效？**
检查路径与 `[section]` 写法（`#` 注释、`key = value`、向量逗号分隔）。解析错误会抛出带行号的 `std::runtime_error`，示例会打印并保留旧配置。`blend`/`shape` 等枚举值不区分大小写。

**Q：出生率低于死亡率时槽位会泄漏吗？**
不会。死亡槽位只入栈一次，并优先复用。已分配范围可能大于当前存活数量；clear() 会重置该范围。
