# ember Integration Guide (embedding into different tech stacks)

This guide is for developers embedding ember into an **existing C++ project**. The core idea: **ember never creates a GL context and never touches the window system** — you only need to do two things on top of your existing GL context:

1. Initialize glad with your loader: `ember::gl::init(loader)`
2. Call `sys.update(dt)` and `sys.render(view, proj, fbWidth, fbHeight, fovY)` every frame

`ember_glfw` (the window wrapper) is an optional convenience module, fully decoupled from the core. If you want the **whole library injected as a single header**, use `single_header/ember.hpp` (see §5).

---

## 1. Core design (why it embeds cleanly)

| Concern | How ember handles it |
| --- | --- |
| GL function loading | host-injected: `gl::init(void* (*)(const char*))`, works with any `GetProcAddress`-style loader |
| Context ownership | never creates a context, never calls `glfwInit`, never takes over your main loop; only does `glGen*/glBufferData/glDispatchCompute/glDraw*` |
| State pollution | `render()` sets blend/depth itself and restores `glDepthMask(GL_TRUE)`; bloom uses its own FBO chain and restores the host's bound framebuffer afterwards; everything else (viewport, clears, your VAOs) stays the host's business |
| Multi-instance | any number of `ParticleSystem` instances can coexist (each owns its SSBOs/programs), e.g. one per scene |

**Minimum embedding code** (inside a project that already has a GL context):

```cpp
#include "ember/particle_system.hpp"

ember::gl::init(/* your loader, see per-stack examples below */);
ember::ParticleSystem sys({100000, 30000});
sys.setGravity({0, -9.81f, 0});

// every frame:
sys.update(dt);
sys.render(view, proj, (float)fbWidthPx, (float)fbHeightPx, fovYDeg);
```

## 2. Dependency checklist

| Dependency | Role | Required | How to get it / notes |
| --- | --- | --- | --- |
| OpenGL **4.3 core** | runtime | ✅ | driver; compute shaders + SSBO are hard requirements (macOS caps at 4.1 — see §9) |
| **glad** (v2, `gl:core=4.3`) | GL entry points | ✅ | add the generated `glad.h` (include path) + `glad.c` (compile it); the CMake path fetches it via FetchContent (needs Python 3 + jinja2 to generate); offline: `deps/glad-v2.0.8.tar.gz` |
| **glm** (≥ 0.9.9) | math (header-only) | ✅ | vendored `deps/glm-1.0.1.tar.gz` or system package |
| **GLFW** (3.4) | windows (examples / `ember_glfw` / `EMBER_USE_GLFW`) | optional | not needed by the core; enables the window module in the single header |
| **stb_image** | PNG sprites (`EMBER_USE_STB`) | optional | vendored in `third_party/stb/`; without it `setSpriteTexture` falls back to the built-in gradient |
| CMake ≥ 3.16 | build | optional | non-CMake projects compile the sources or the single header directly |
| Python 3 + jinja2 | only when FetchContent pulls glad | optional | glad generator |

> **Single-header dependencies** (§5): glad (`glad.h` + `glad.c` compiled into your project) + glm are required; GLFW only when `EMBER_USE_GLFW` is defined; stb_image only when `EMBER_USE_STB` is defined (and `STB_IMAGE_IMPLEMENTATION` is defined in some TU).

## 3. Embedding into a CMake project (three modes)

### 3.1 add_subdirectory (source as dependency, simplest)

```cmake
add_subdirectory(third_party/ember)          # drop the library source into your project
target_link_libraries(your_app PRIVATE ember)          # no window wrapper needed
# with the window wrapper:  PRIVATE ember_glfw
```

Deps (glad/glm/glfw) are fetched by ember's own FetchContent by default. Declare FetchContent entries with the same names **before** `add_subdirectory` to reuse your own copies:

```cmake
include(FetchContent)
FetchContent_Declare(glm  GIT_REPOSITORY ...)
FetchContent_Declare(glad GIT_REPOSITORY ...)
FetchContent_Declare(glfw GIT_REPOSITORY ...)
add_subdirectory(third_party/ember)
```

### 3.2 FetchContent (pull ember from remote)

```cmake
include(FetchContent)
FetchContent_Declare(ember
  GIT_REPOSITORY https://github.com/you/ember
  GIT_TAG        v0.1.0)
set(EMBER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)   # library only
FetchContent_MakeAvailable(ember)
target_link_libraries(your_app PRIVATE ember)
```

### 3.3 install + find_package (prebuilt distribution)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --install build --prefix ~/local
# then in any project:
#   -DCMAKE_PREFIX_PATH=~/local
```

```cmake
find_package(ember REQUIRED)
target_link_libraries(your_app PRIVATE ember::ember)       # core
# window wrapper: ember::ember_glfw (needs glfw3)
```

> Note: the `ember::ember` export references the `glad`/`glm` targets, so consumers must provide them first (their own FetchContent or an installed copy). `emberConfig.cmake` tries `find_dependency(glad/glm/glfw3)`.

## 4. Non-CMake workflows

The core library is only **3 .cpp files** (one of them optional) — any build system can swallow it:

```
compile: src/shader.cpp  src/particle_system.cpp  [src/stb_image.cpp — PNG sprites, optional]
headers: include/  (i.e. #include "ember/particle_system.hpp")
external deps: glad (compile its generated glad.c too), glm (header-only, add include path)
link: -lGL (Linux) / opengl32 (Windows) plus your window library
```

- **Makefile / Bazel / Meson / Xcode / Visual Studio**: just add the files above to your project — no CMake needed.
- **Dropping stb**: don't compile `src/stb_image.cpp` and don't define `EMBER_USE_STB` → `setSpriteTexture` falls back to the built-in gradient.
- **Prebuilt static library**: `ember.a` (glad either linked into the same library or separately), distribute the header directory + library, consumers add the include path manually.

## 5. Single-header edition (single_header/ember.hpp)

One header injects the entire library — declarations AND implementation, **no ember sources to compile**:

```cpp
// in some .cpp (only ONE translation unit in the whole project):
#define EMBER_IMPLEMENTATION      // expands the implementation into this TU
#define EMBER_USE_GLFW            // optional: GLFW window convenience module
#define EMBER_USE_STB             // optional: PNG sprites (also define STB_IMAGE_IMPLEMENTATION somewhere)
#include "ember.hpp"

int main() {
    ember::Window win(1280, 720, "demo");   // creates a 4.3 core context + gl::init internally
    ember::ParticleSystem sys({100000, 30000});
    sys.setGravity({0.f, -9.81f, 0.f});
    // main loop: win.pollEvents(); sys.update(dt); sys.render(view, proj, w, h, fov);
}
```

- **How it's produced**: `single_header/ember.hpp` is generated from the modular sources by `tools/amalgamate.py` (after changing the library run `python tools/amalgamate.py` to regenerate; `--verify` checks for drift).
- **Dependencies** (see §2): glad (`glad.h` + `glad.c` compiled into your project) + glm; GLFW / stb_image are opt-in macros. **Without those macros** the core particle functionality works fine — you only lose the window module and PNG sprites.
- **Relationship to the modular build**: identical code, identical contract; the single header is the distribution form, the modular sources are the development form. `tests/single_header_test.cpp` compiles the whole library from the single header and runs all assertions (including a pixel check) to guarantee equivalence.
- **Custom shaders**: same contract as the modular build (§8); `shaders/` files load first, falling back to the embedded copies when missing.

## 6. Per-stack wiring

### 6.1 GLFW (ready-made: ember_glfw / EMBER_USE_GLFW)

```cpp
#include "ember/glfw_window.hpp"
ember::Window win(1280, 720, "demo");   // creates a 4.3 core context + gl::init internally
// main loop: win.pollEvents(); sys.update(win.deltaTime()); ...
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
// main loop: SDL_Event handling -> sys.update(dt) -> glClear -> sys.render(...) -> SDL_GL_SwapWindow
```

### 6.3 Qt (QOpenGLWidget)

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

Key points:
- Set `setVersion(4, 3)` + `setProfile(CoreProfile)` on your `QSurfaceFormat`, or verify `QOpenGLContext::currentContext()->format()` in `initializeGL`.
- **All ember calls must happen in paintGL (the GL thread)** — never on the UI thread.
- Qt 6 needs the `QFunctionPointer` cast for `QOpenGLContext::currentContext()->getProcAddress`; you can also preload `QOpenGLFunctions_4_3_Core` and reuse it for glad, but direct `gl::init` is simplest.

### 6.4 Win32 / WGL

```cpp
#include <windows.h>
#include "ember/particle_system.hpp"

static void* winLoader(const char* name) {
    // wglGetProcAddress covers core/extension functions beyond 1.1;
    // a few base functions (e.g. glGetString) need the GetProcAddress fallback.
    void* p = reinterpret_cast<void* (*)(const char*)>(wglGetProcAddress(name));
    if (!p) {
        HMODULE m = GetModuleHandleA("opengl32.dll");
        if (m) p = reinterpret_cast<void* (*)(const char*)>(GetProcAddress(m, name));
    }
    return p;
}

// after: create window -> choose pixel format -> wglCreateContext -> wglMakeCurrent
ember::gl::init(winLoader);
```

### 6.5 EGL / headless / offscreen

```cpp
// same pattern as WGL, loader = eglGetProcAddress (also needs the library fallback):
ember::gl::init(reinterpret_cast<void* (*)(const char*)>(eglGetProcAddress));
```

Windowless rendering (offscreen FBO, servers, tests): create an EGL pbuffer context or use `EGL_KHR_surfaceless_context`; `sys.update(dt)` runs the compute pipeline fine; `render()` needs a real viewport — bind an FBO offscreen and pass its width/height and fov.

## 7. Coexistence with a host renderer

- **Depth**: particles default to additive blending, depth test off, no depth writes. If the scene needs particles occluded by geometry: `sys.setDepthTest(true); sys.setDepthWrite(true);` (draw opaque geometry first, then particles).
- **Soft particles**: render the scene depth into a depth texture (`GL_DEPTH_COMPONENT24`, `Texture::uploadDepth` works) and call `sys.setSoftParticles(true, depthTexId, radius)`; refresh it before the particles each frame.
- **Bloom**: with `sys.setBloom(true)` the `render()` uses its own FBO chain (half-res RGBA16F bright/blur) and restores the host's currently bound framebuffer afterwards; no host changes needed.
- **Blend state**: `render()` sets the blend function itself and does not restore it afterwards — if that conflicts with the host, re-set your state after the particles (`glDisable(GL_BLEND)` after render).
- **Viewport/scissor**: `render()` never calls `glViewport`/`glScissor` — that's the host's job.
- **Resize**: just set the viewport again — no need to rebuild the system (pass the height from `framebufferSize()` each frame).
- **Multiple systems**: `ParticleSystem` instances don't interfere; each compiles its own default shaders (acceptable; share via `setPrograms` with one Shader if you care).
- **Threading**: all ember calls must happen on the thread that owns the GL context (Qt's paintGL, your render thread, etc.).

## 8. Custom shaders / force extension contract

Default GLSL lives in `shaders/` (the canonical copies; the embedded copies inside `src/particle_system.cpp` are what actually runs). Copy and modify, then:

```cpp
sys.setPrograms(
    ember::Shader::fromFiles({{GL_VERTEX_SHADER, "my.vert"},
                              {GL_FRAGMENT_SHADER, "my.frag"}}),
    ember::Shader::fromFiles({{GL_COMPUTE_SHADER, "my.comp"}}));
```

Contract (must be preserved, otherwise undefined behavior):

| Binding | Buffer | Notes |
| --- | --- | --- |
| 0 | `cur` / `particles` | sim read; render VS read (`Particle` array, std430) |
| 1 | `nxt` | sim write (whole struct) |
| 2 | `req` | spawn requests (`SpawnRequest[]`, std430, see emitters.hpp), read-only; GPU samples spawns |
| 3 | `dead` | free-slot stack, `coherent` |
| 4 | `counters` | `uAlive uDeadHead uSpawnRequestCount uCapacity`, `coherent` |
| 5 | `attractors` | `vec4` array, read-only |
| 6 | `vortexes` | `Vortex` array (`vec4 center` + `vec4 axisStrength`), read-only |
| 7 | `springs` | `Spring` array (`vec3 anchor` + 2 floats), read-only |
| 8 | `sorted` | sorted particle indices (render VS read; only when `uUseSorted=1`) |
| 9 | `palette` | palette colors `vec4[]` (GPU spawn), read-only |
| 10 | `indirect` | indirect draw args (`vertexCount/instanceCount/firstVertex/baseInstance`; written by phase 2, read by `glDrawArraysIndirect`) |

> ⚠️ **Breaking change (0.x)**: binding 2 went from "full `Particle[]` staging" to "`SpawnRequest[]` spawn requests" — hosts with custom spawn shaders must migrate to the new contract.

- sim uniforms: `uDt uTime uPhase uForceMask uGravity uDrag uDragMode uWind uTurbulence uAttractorCount uVortexCount uSpringCount uNoiseWindDir uNoiseWindAmp uNoiseWindScale uNoiseWindSpeed uWaveDir uWaveK uWaveAmp uWaveOmega uBoundaryMode uBoundaryY uRestitution uSpawnTotal uFrameSeed` (`uPhase=0` integrate, `=1` spawn, `=2` write indirect args — three dispatches; `uSpawnTotal` = total particles requested this frame, `uFrameSeed` = GPU spawn RNG seed; `uForceMask` bit i = `ember::Force` value i, disabled forces cost nothing)
- render VS uniforms: `uView uProj uSizeScale uStreak uUseSorted` (`uUseSorted=1` reads particles via `sorted[gl_InstanceID]`, else `gl_InstanceID`; `uStreak>0` stretches quads along the view-projected velocity); render FS uniforms: `uSprite uUseSprite uSheetCols uSheetRows uSceneDepth uInvViewProj uSoftRadius uViewportSize uUseSoft` (`uUseSprite=1` texture + frame animation, `=0` procedural glow; `uUseSoft=1` fades near scene depth; `vFadeRGB` varying comes from `life.yzw` for fade-to-color)
- Draw call: `glDrawArraysIndirect(GL_TRIANGLE_STRIP, 0)` — 4 vertices per particle, VS expands the quad from `gl_VertexID` in view space, no vertex attributes; the instance count comes from the GPU-written binding-10 args (CPU readback not on the render path)
- `Particle` layout (CPU/GPU must match, 64 bytes): `pos(xyz,size) vel(xyz,age) life(lifetime; <0=corpse) color(rgba)`
- **Common extension points**: add custom forces in the force-accumulation section of `simulate()` (vortices, springs, noise fields, collisions); change billboard size/shape (or the streak/orientation logic) in the VS; swap the glow curve or add textures in the FS; change `sort.comp`'s key (e.g. distance instead of depth).

## 9. Compatibility & fallbacks (macOS / OpenGL 4.1)

### Compatibility matrix

| Platform | GL version | ember status |
| --- | --- | --- |
| Windows / Linux (NVIDIA/AMD/Intel drivers) | 4.3+ | ✅ full features |
| macOS | max **4.1** | ❌ unsupported (below) |
| Web / WASM | no GL 4.3 | ❌ needs a port (WebGPU etc.) |
| Embedded / low-end GPUs | < 4.3 | ❌ needs a fallback (below) |

**Root cause**: two core dependencies are 4.3 features — **compute shaders** (GPU simulation) and **SSBOs** (particle buffers, also read directly by the render VS). macOS stopped updating OpenGL in 2018 and caps at 4.1; neither feature is available.

### Fallback paths (increasing effort; each is a separate project)

**Path A: CPU simulation (minimal change, 3.3+ compatible)**
Integrate on the CPU each frame (reuse `Emitter` spawn logic and the force formulas), write results into a particle buffer; switch rendering from "VS reads SSBO" to **attrib-based instancing** (`glVertexAttribDivisor`, a 3.3 feature):

```cpp
// every frame:
std::vector<Particle> cpu;
for (std::size_t i = 0; i < sys.emitterCount(); ++i)
    sys.emitter(i)->spawn(cpu, maxN, dt, rng);
integrateCpu(cpu, dt, gravity, attractors, ...);   // your CPU integrate loop
glBindBuffer(GL_ARRAY_BUFFER, particleVbo);
glBufferData(GL_ARRAY_BUFFER, cpu.size() * sizeof(Particle), cpu.data(), GL_DYNAMIC_DRAW);
glDrawArraysInstanced(GL_POINTS, 0, 1, (GLsizei)cpu.size()); // one attribute per component
```

**Path B: transform feedback simulation (still full GPU, 4.1 compatible)**
Move the compute integration into **vertex shader + glTransformFeedback** (an old 4.1 technique): one vertex per particle, integrate in the VS and write back to a buffer, then feed it back as the next frame's input. Rendering switches to attrib-based instancing too. Performance is close to compute, but the implementation effort is the largest (double buffering, primitive-restart semantics, attribute layout alignment).

**Path C: Metal port**
Translate the simulation kernels (integration + free-stack recycling + force fields) into Metal compute and swap the render pipeline for Metal. All look parameters (config files, emitters, forces) are reused; only the render/simulation backend is replaced. This is the best experience on macOS and Apple's recommended direction (Metal 3 fully supersedes OpenGL).

**Bottom line**: ember does **not** ship a 4.1/Metal fallback — GPU simulation and SSBO rendering are its core design. If you have a hard macOS/low-end-GPU requirement, start with Path A (smallest change) or spin up a Metal backend per Path C; contributions as a separate backend (`EMBER_BACKEND_METAL`-style) are welcome.

## 10. FAQ

**Q: How do I build behind a corporate proxy / offline?**
The repo vendors its dependency tarballs in `deps/`: `cmake -S . -B build -DEMBER_DEPS_DIR=<absolute path to deps>` and FetchContent extracts locally — fully offline. The one exception: the glad generator needs local Python 3 + `jinja2`, and the first generation pulls gl.xml from Khronos (the result is cached in the build dir; offline afterwards).

**Q: How do I wire up the single-header build?**
The essentials are glad (`glad.h` + `glad.c`) and glm. Copy `single_header/ember.hpp` into your project, `#define EMBER_IMPLEMENTATION` and include it once in one .cpp; compile `glad.c` as usual and call `ember::gl::init(loader)`. A complete example is `tests/single_header_test.cpp` (depends only on the single header + glad/glfw/glm — not on the ember library).

**Q: Can I drop the stb_image PNG dependency?**
Yes. Don't define `EMBER_USE_STB` (modular path: don't compile `src/stb_image.cpp`) — `setSpriteTexture` falls back to the built-in gradient with a warning.

**Q: Will the interactive editor (WASD-move emitters etc.) leak into my project?**
No. The editor lives only in the example programs, gated by the CMake option `EMBER_BUILD_EDITOR` (default ON, examples only); `-DEMBER_BUILD_EDITOR=OFF` or dropping the examples removes it entirely. The library itself contains no editor code; the runtime toggle is `[system] editor = false`.

**Q: I only have OpenGL 3.3 — what now?**
Compute shaders (4.3) are a hard requirement. Fallback: CPU simulation + instanced rendering (write your own integrate loop into a particle SSBO; the render VS/FS and `render()` can still be reused); or port the simulation kernel to Vulkan/D3D12.

**Q: Does it work on macOS?**
No. macOS caps at GL 4.1 (no compute). Alternatives: port the simulation to Metal, or update particle data on the CPU/compute queue before `render()` (see §9).

**Q: Does the 16-byte per-frame readback stall?**
`glGetBufferSubData` triggers an implicit sync; on modern drivers the overhead is sub-millisecond one-off. The render path no longer depends on it (the instance count comes from GPU indirect args) — the readback only feeds `aliveCount()`/UI. If you're latency-sensitive (VR), move the readback to frame-end + consume at frame-start (double buffering).

**Q: How do I pick the particle capacity?**
Steady-state live particles ≈ total spawn rate × average lifetime. Set `capacity` to that plus headroom; `maxSpawnPerFrame` guards against single-frame spikes. Example: 7000/s × 2.75s ≈ 19k, config uses 150k for lots of headroom. See `config/stress.ini` (1M capacity, ~850k steady state).

**Q: Config file changes don't take effect?**
Check the path and the `[section]` syntax (`#` comments, `key = value`, comma-separated vectors). Parse errors throw a `std::runtime_error` with a line number; the examples print it and keep the previous config. Enum values like `blend`/`shape` are case-insensitive.

**Q: Do slots leak when the spawn rate drops below the death rate?**
Some slots stay as "corpses" (no visual impact, just capacity); if the mismatch persists, reconfigure capacity per the steady-state formula. When the ring free-stack overflows it drops its oldest entries — no double-write race.
