# ember Integration Guide (embedding into different tech stacks)

This guide is for developers embedding ember into an **existing C++ project**. The core idea: **ember never creates a GL context and never touches the window system** — you only need to do two things on top of your existing GL context:

1. Initialize glad with your loader: `ember::gl::init(loader)`
2. Call `sys.update(dt)` and `sys.render(view, proj, fbWidth, fbHeight, fovY)` every frame

`ember_glfw` (the window wrapper) is an optional convenience module, fully decoupled from the core. If you want the **whole library injected as a single header**, use `single_header/ember.hpp` (see §5).

---

## 1. Core design (why it embeds cleanly)

| Concern | How ember handles it |
| --- | --- |
| GL function loading | host-injected: `gl::init(GLADloadfunc)`, works with any `GetProcAddress`-style loader |
| Context ownership | never creates a context, never calls `glfwInit`, never takes over your main loop; only does `glGen*/glBufferData/glDispatchCompute/glDraw*` |
| State management | `render()` restores read/draw FBOs, viewport, depth state, face-culling and scissor enables; the host must rebind blend state, programs, VAOs, SSBOs and textures before subsequent draws |
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

This table describes the OpenGL backend. The CPU facade only requires glm; include `ember/system.hpp` and inject a backend through its constructor. See the [backend notes](docs/backends.md).

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
find_package(ember REQUIRED COMPONENTS opengl)
target_link_libraries(your_app PRIVATE ember::opengl)      # core + OpenGL
# window wrapper: COMPONENTS glfw + ember::ember_glfw (needs glfw3)
```

> Existing dependency targets are accepted; missing dependencies are located with `find_dependency`. `COMPONENTS core` needs only glm; `opengl` adds glad; `glfw` adds GLFW. Omitting components loads every module built in the installation. `ember::ember` remains the compatibility name for OpenGL.

For source builds, `EMBER_BUILD_OPENGL=OFF` builds only `ember::core`; `EMBER_BUILD_GLFW=OFF` keeps OpenGL without the window module. Both default to ON.

## 4. Non-CMake workflows

Compile `src/particle_system.cpp` for the CPU facade, then add the following backend files for OpenGL (also listed in `cmake/EmberOpenGLSources.cmake`):

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
optional: src/backends/opengl/stb_image.cpp (PNG), src/glfw_window.cpp (window)
headers: include/  (i.e. #include "ember/particle_system.hpp")
external deps: glad (compile its generated glad.c too), glm (header-only, add include path)
link: -lGL (Linux) / opengl32 (Windows) plus your window library
```

- **Makefile / Bazel / Meson / Xcode / Visual Studio**: just add the files above to your project — no CMake needed.
- **Dropping stb**: don't compile `src/backends/opengl/stb_image.cpp` and don't define `EMBER_USE_STB` → `setSpriteTexture` falls back to the built-in gradient.
- **Prebuilt static libraries**: link `ember`, `ember_core` and glad; add `ember_glfw` and GLFW when using the window module. Distribute the headers and corresponding archives. Consumers must recompile because the class layout changed.

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

ember::gl::init(+[](const char* name) -> GLADapiproc {
    return reinterpret_cast<GLADapiproc>(SDL_GL_GetProcAddress(name));
});

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

Key points:
- Set `setVersion(4, 3)` + `setProfile(CoreProfile)` on your `QSurfaceFormat`, or verify `QOpenGLContext::currentContext()->format()` in `initializeGL`.
- **All ember calls must happen in paintGL (the GL thread)** — never on the UI thread.
- Qt 6 needs the `QFunctionPointer` cast for `QOpenGLContext::currentContext()->getProcAddress`; you can also preload `QOpenGLFunctions_4_3_Core` and reuse it for glad, but direct `gl::init` is simplest.

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

// after: create window -> choose pixel format -> wglCreateContext -> wglMakeCurrent
ember::gl::init(winLoader);
```

### 6.5 EGL / headless / offscreen

```cpp
// same pattern as WGL, loader = eglGetProcAddress (also needs the library fallback):
ember::gl::init(+[](const char* name) -> GLADapiproc {
    return reinterpret_cast<GLADapiproc>(eglGetProcAddress(name));
});
```

Windowless rendering (offscreen FBO, servers, tests): create an EGL pbuffer context or use `EGL_KHR_surfaceless_context`; `sys.update(dt)` runs the compute pipeline fine; `render()` needs a real viewport — bind an FBO offscreen and pass its width/height and fov.

## 7. Coexistence with a host renderer

- **Depth**: particles default to additive blending, depth test off, no depth writes. If the scene needs particles occluded by geometry: `sys.setDepthTest(true); sys.setDepthWrite(true);` (draw opaque geometry first, then particles).
- **Coordinate/depth convention**: the projection passed to `render()` and host depth textures follow GL conventions (NDC z ∈ [-1,1], framebuffer origin bottom-left); hosts using [0,1] depth or top-left origins must adapt their projection and depth export.
- **Soft particles**: render the scene depth into a depth texture (`GL_DEPTH_COMPONENT24`, `Texture::uploadDepth` works) and call `setOpenGLSoftParticles(sys, true, depthTexId, radius)`; refresh it before the particles each frame.
- **Refractive particles**: particles with `[emitter] refractive = true` need the host to render its scene into a color texture (RGBA, `CLAMP_TO_EDGE` — `Texture::uploadRGBA8` already sets this), passed via `setOpenGLRefraction(sys, settings)` (`settings.sceneColorTex`); the refraction pass replaces pixels straight onto the host framebuffer after the bloom composite, with glints from the specular term (not bloom). See the `ScenePass` in `examples/glass.cpp`.
- **Bloom**: with `sys.setBloom(true)` the `render()` uses its own FBO chain (half-res RGBA16F bright/blur) and restores the host's currently bound framebuffer afterwards; no host changes needed.

Refraction also honors explicit `setDepthTest` / `setDepthWrite` (both default off). Its coverage includes particle alpha, lifetime fade and sprite mask; fully transparent fragments do not write depth. Depth refraction bends the ray using `ior` (`ior=1` is the identity), and `strength` scales the projected displacement. Simple/noise modes still interpret strength as a UV offset. Spin accepts signed angular velocity; soft radius must be positive. See the [effect audit](docs/effects-audit.md) for coverage and remaining approximations.
- **Blend state**: `render()` sets the blend function itself and does not restore it afterwards — if that conflicts with the host, re-set your state after the particles (`glDisable(GL_BLEND)` after render).
- **Viewport/scissor**: render() uses supplied dimensions at origin (0,0), then restores the host viewport. Scissor state is unchanged.
- **Resize**: just set the viewport again — no need to rebuild the system (pass the height from `framebufferSize()` each frame).
- **Multiple systems**: `ParticleSystem` instances don't interfere; each compiles its own default shaders (acceptable; share via `setOpenGLPrograms` with one Shader if you care).
- **Threading**: all ember calls must happen on the thread that owns the GL context (Qt's paintGL, your render thread, etc.).

Loader callbacks use `GLADloadfunc`. Pass `glfwGetProcAddress` directly; use typed adapters for SDL/Qt/EGL, converting the returned address rather than the callback type. Destroy GPU resources while their owning context is current. GLFW callback exceptions are deferred until the next event-processing wrapper call on the same thread.

Custom shader protocol: binding 4 allocates eight uints (32 B). The first five remain `uAlive/uDeadHead/uSpawnRequestCount/uCapacity/uAllocated`; the trailing fields are `uSpawnReuse/uSpawnAppendBase/uSpawnAccepted`. Default synchronous mode reads the first 20 B; GPU mode samples them asynchronously. Spring stride remains 32 B and depth reconstruction uses `uInvProj = inverse(proj)`.

- The built-in simulation declares `uInputAlive`, selecting the live-list protocol. Phase 0 integrates binding 11, appends survivors to binding 12 and increments indirect instanceCount; retirement writes tombstones to both particle buffers. Phase 3 reserves all recycled/appended birth slots in one invocation and updates the counters/reservation fields. Phase 1 samples births and appends their indices. Phase 2 publishes final indirect instanceCount in one invocation. SSBO barriers separate phases, with command/buffer-update barriers at completion; both particles and live lists are swapped after update.
- Legacy simulations without `uInputAlive` retain the allocated-extent, three-phase protocol: phase 0 integrates, phase 1 spawns, phase 2 writes binding 11 and draw args. The five-counter prefix is unchanged. Switching back to the optimized protocol copies current tombstones to the other particle buffer once.
- Sort shaders declaring `uTileMode` select the 256-thread tiled protocol. Binding 12 is rebound to a separate depth-key cache: `uMode=0/uTileMode=1` initializes and sorts tiles; `uMode=1/uTileMode=0` performs global steps with `uJ>=256`; `uMode=1/uTileMode=2` finishes `uJ=128..1` for the current `uK`. Legacy shaders without that uniform retain the 64-thread mode 0/1 protocol.
- Opting into either protocol requires implementing its complete layout and stage semantics, not merely adding a uniform. Sorting still uses `uAlive/uCapacity` and the capacity sentinel. Built-in equal-depth ties are ordered by stable slot ID. CPU API, stable particle identity and immediate `aliveCount()` semantics are unchanged.


### GPU scheduling and asynchronous statistics (opt-in)

The default mode still reads counters synchronously on every update. Enable GPU scheduling during initialization:

```cpp
sys.setGpuDriven(true);
// Each frame:
sys.update(dt);
sys.render(view, proj, width, height, fov);
const auto stats = sys.pollStatistics();
// stats.alive / stats.allocated belong to stats.frame, possibly an older update.
const auto lag = sys.updateSequence() - stats.frame;
```

Integration, sorting and drawing in `update()` / `render()` use GPU counts, independently of CPU telemetry. `pollStatistics()` checks fences with zero timeout and reads only completed, separate staging buffers. Without new data it returns the previous snapshot (initially all zero). When all four slots are busy, the update skips telemetry and increments the cumulative `droppedStatistics()` count instead of waiting.

`Statistics::frame` identifies the sampled update; `updateSequence()` identifies the latest submitted update. `clear()` and capacity changes cancel pending samples and publish known zero counts without resetting the sequence. `allocated` is the occupied slot extent including free slots. The final update is not guaranteed to have a ready sample: use `synchronizeStatistics()` for exact current counts. `aliveCount()` and `readParticles()` retain their immediate, exact semantics and can therefore synchronize in GPU mode; use polling for UI counters.

Disabling GPU scheduling synchronizes counts once. Polling, mode changes, updating, rendering and destruction require the owning GL context to be current; pass the plain statistics value to UI threads instead of invoking GL there. GPU mode skips the per-particle synchronous `EMBER_DEBUG` readback logs. This option currently has a C++ API only, without an INI key.

GPU shader protocol:

- Internal `schedule.comp` always uses its embedded copy and is not replaced by `setShaderDirectory()`. Binding 13 is a uint array: word 0 holds input alive, word 1 holds padded sort N, words 2–4 hold integration dispatch x/y/z, and word 5 onward holds consecutive three-word sort commands.
- Scheduler phase 0 resets binding 10 draw args, writes the request count and produces `ceil(alive/64)` integration groups. When sorting is enabled, scheduler phase 1 follows simulation phases 0/3/1/2 and generates sort commands: initialization, then `k=512..nextPow2(capacity)`, global steps `j=k/2..256` plus one local tail per k. Stages above actual padded N have x=0; an empty system has x=0 for every sort command. Shader-storage / command barriers publish indirect arguments.
- Custom simulations must implement the complete protocol with active `uInputAlive` and `uGpuDriven` uniforms. With `uGpuDriven=1`, integration bounds come from binding 13 word 0, not CPU `uInputAlive`. Sorting requires `uTileMode` and `uGpuDriven`, reading alive from binding 4 and padded N from binding 13 word 1. Synchronous mode sets `uGpuDriven=0` and uses the original uniforms.
- Incompatible custom programs throw when enabling GPU mode, replacing simulation or enabling sort. Synchronous mode retains legacy shader compatibility. Uniform checks identify a protocol, not proof that custom code implements it correctly.
- Capacity is checked against the device's X workgroup limit: both `ceil(capacity/64)` and `ceil(nextPow2(capacity)/256)` must fit. GPU sorting reserves keys for capacity and submits capacity-sized stage sequences with inactive GPU commands. Small populations and excess capacity can therefore have higher submission cost than synchronous mode.

This removes the forced counter-read dependency; OpenGL can still block on driver queues or allocations. See [benchmarks](benchmarks/README.md) for measured throughput.

## 8. Custom shaders / force extension contract

GLSL files in `shaders/` load first; simulation, particle rendering and sorting fall back to embedded copies on failure. Missing Bloom files disable that effect. Copy and modify, then:

```cpp
setOpenGLPrograms(sys.backend(), 
    ember::Shader::fromFiles({{GL_VERTEX_SHADER, "my.vert"},
                              {GL_FRAGMENT_SHADER, "my.frag"}}),
    ember::Shader::fromFiles({{GL_COMPUTE_SHADER, "my.comp"}}));
```

Contract (must be preserved, otherwise undefined behavior):

| Binding | Buffer | Notes |
| --- | --- | --- |
| 0 | `cur` / `particles` | sim reads and writes retirement tombstones; render VS reads (`Particle` array, std430) |
| 1 | `nxt` | next particles; phases 0/1 write |
| 2 | `req` | spawn requests (`SpawnRequest[]`, std430, see emitters.hpp), read-only; GPU samples spawns |
| 3 | `dead` | free-slot stack, `coherent` |
| 4 | `counters` | `uAlive uDeadHead uSpawnRequestCount uCapacity uAllocated uSpawnReuse uSpawnAppendBase uSpawnAccepted`, `coherent` |
| 5 | `attractors` | `vec4` array, read-only |
| 6 | `vortexes` | `Vortex` array (`vec4 center` + `vec4 axisStrength`), read-only |
| 7 | `springs` | `Spring[]`: std430 stride **32 B**, offsets anchor=0, stiffness=12, damping=16 |
| 8 | `sorted` | sorted particle indices (render VS read; only when `uUseSorted=1`) |
| 9 | `palette` | palette colors `vec4[]` (GPU spawn), read-only |
| 10 | `indirect` | indirect draw args (`vertexCount/instanceCount/firstVertex/baseInstance`; written by phase 2, read by `glDrawArraysIndirect`) |
| 11 | `liveIndices` | current live indices; sim/render/sort read; swapped with next list after update |
| 12 | `nextLiveIndices / sortKeys` | next live-list output during sim; separate cached-depth buffer during sort |
| 13 | `schedule` | GPU scheduling metadata and indirect compute commands |

> ⚠️ **Breaking change (0.x)**: binding 2 went from "full `Particle[]` staging" to "`SpawnRequest[]` spawn requests" — hosts with custom spawn shaders must migrate to the new contract.

- The built-in shaders no longer use loose uniforms — scalars are packed into std140 uniform blocks (the same GLSL compiles to SPIR-V; CPU mirrors with layout asserts live in `src/backends/opengl/params.hpp`): simulation `SimParams` binding 14, sort `SortParams` binding 15, internal scheduling `ScheduleParams` binding 16, render VS `DrawParams` binding 17, render FS `FragParams` binding 18, bloom `BloomParams` binding 19. Block members keep the old loose-uniform names, with one exception: the fragment block renames `uProj`/`uRefraction` to `uFragProj`/`uFragRefraction` (members of instance-less blocks share one global namespace and would collide with the vertex block). Sim block members: `uDt uTime uPhase uForceMask uGravity uDrag uDragMode uWind uTurbulence uAttractorCount uVortexCount uSpringCount uNoiseWindDir uNoiseWindAmp uNoiseWindScale uNoiseWindSpeed uWaveDir uWaveK uWaveAmp uWaveOmega uBoundaryMode uBoundaryY uRestitution uSpawnTotal uFrameSeed uInputAlive uGpuDriven` (`uPhase=0` integrate live indices, `=3` reserve birth slots, `=1` spawn, `=2` publish indirect args — up to four dispatches; `uSpawnTotal` = total particles requested this frame, `uFrameSeed` = GPU spawn RNG seed; `uForceMask` bit i = `ember::Force` value i, disabled forces cost nothing)
- Compatibility path: the backend still writes the legacy loose uniforms (VS `uView uProj uSizeScale uStreak uUseSorted`, FS `uSprite uUseSprite uSheetCols uSheetRows uSceneDepth uInvProj uSoftRadius uViewportSize uUseSoft`, etc.), so custom shaders written against the old contract need no changes; new custom shaders should declare the same block members (protocol detection sees block members too). Samplers carry explicit bindings: `uSprite`=0, `uSceneDepth`=1, `uSceneColor`=2. With `uUseSorted=1` the particle index comes from `sorted[gl_InstanceID]`, else `liveIndices[gl_InstanceID]`; `uStreak>0` stretches quads along the view-projected velocity; `uUseSprite=1` selects texture sampling + frame animation, `=0` the procedural glow; `uUseSoft=1` fades near scene depth; the `vFadeRGB` varying comes from `life.yzw` for fade-to-color
- Build-time SPIR-V: with `-DEMBER_BUILD_SPIRV=ON` (default) and glslangValidator on PATH, the `ember_spirv` target compiles all of `shaders/` into `build/generated/spirv/` (`--target-env vulkan1.0`), ready for a future Vulkan backend to load directly — no runtime compiler dependency
- Draw call: `glDrawArraysIndirect(GL_TRIANGLE_STRIP, 0)` — 4 vertices per particle, VS expands the quad from `gl_VertexID` in view space, no vertex attributes; the instance count comes from the GPU-written binding-10 args (CPU readback not on the render path)
- **Refractive particles (glass shards)**: particles with `[emitter] refractive = true` (or `BurstParams::refractive`) are sign-encoded in `pos.w` (negative size) and rendered in a separate pixel-replacement pass (blending off) that samples the host's scene color texture (**texture unit 2** `uSceneColor`); new FS uniforms: `uRefraction uRefrMode uRefrShape uRefrDome uRefrStrength uRefrIor uRefrTint uRefrAbsorption uRefrFresnel uRefrChroma uRefrSpecular uRefrLightDir`; shard silhouettes can be procedural polygons (`uRefrShape=1`: 3-5 sides, per-edge jitter), and `uRefrDome` controls the dome-normal strength (silhouette rim light + glint spots, 0 = flat facet); presets `ember::Refraction::glass()/heat()/water()/prism()`; the host renders its scene into a color texture and passes it via `setOpenGLRefraction(sys, ...)` (see §7)
- **Billboard spin**: `sys.setSpin(speed)` / `[system] spin` uses a stable-slot hash angle plus age-based spin (`uSpinSpeed`). A visible streak controls orientation; below 10% extra elongation it blends toward the ordinary orientation, preserving spin at rest. Refraction geometry and facet normals use the same final angle. See the [streak notes](docs/streak.md).
- `Particle` layout (CPU/GPU must match, 64 bytes): `pos(xyz,size) vel(xyz,age) life(lifetime; <0=corpse) color(rgba)`
- **Common extension points**: add custom forces in the force-accumulation section of `simulate()` (vortices, springs, noise fields, collisions); change billboard size/shape (or the streak/orientation logic) in the VS; swap the glow curve or add textures in the FS; change `sort.comp`'s key (e.g. distance instead of depth).

### Vulkan host integration (optional backend)

`ember::vulkan` is a peer of the GL backend and matches its feature set (GPU scheduling, depth sorting, PNG sprites, soft particles, refraction, bloom). Minimal integration:

```cpp
#include "ember/system.hpp"
#include "ember/vulkan.hpp"

ember::VulkanDevice device = ember::makeVulkanDevice();   // self-contained; use your own device in production
auto backend = ember::makeVulkanBackend();
ember::setVulkanContext(*backend, device.context());       // borrowed instance/physicalDevice/device/queue
ember::ParticleSystem sys({100000, 30000}, std::move(backend));

// Per frame: inject the host frame target (color/depth views) before rendering.
ember::VulkanFrameTarget target{};
target.colorView = myColorView;   // layout must be COLOR_ATTACHMENT_OPTIMAL
target.depthView = myDepthView;   // optional; layout must be DEPTH_STENCIL_ATTACHMENT_OPTIMAL
target.colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
target.depthFormat = VK_FORMAT_D32_SFLOAT;
target.width = 1920; target.height = 1080; target.frameIndex = frameCounter;
ember::setVulkanFrameTarget(sys.backend(), target);

sys.update(dt);
sys.render(view, proj, 1920.f, 1080.f, 50.f);
```

- **Coordinate/depth convention**: `render()` takes the same GL-style projection matrix as the GL backend; the vertex shader remaps clip z into Vulkan's `[0,w]` (via `EMBER_CLIP_VULKAN`) and uses a negative viewport height to flip Y, keeping the bottom-left screen origin for UV/`gl_FragCoord`. Hosts using a zero-to-one projection get numerically identical depth and can feed host depth textures straight into soft particles/refraction.
- **Injected resources are borrowed**: context, frame target, and soft-particle/refraction views/samplers stay owned by the host and must live while the backend uses them. The backend never takes over the swapchain and issues no cross-resource layout barriers (the host guarantees layouts on entry/exit of `render()`).
- **Soft particles / refraction**: `setVulkanSoftDepth(backend, depthView, sampler)` (clamp + linear sampler; view in `SHADER_READ_ONLY_OPTIMAL`); `setVulkanRefractionInputs(backend, colorView, depthView, sampler)`. Refraction mode 1 falls back to mode 0 without a depth texture, exactly like GL.
- **Bloom**: after `sys.setBloom(true)`, `render()` drives its own RGBA16F full/half-resolution chain. The host depth view is reused directly as the HDR particle pass depth attachment (`loadOp=LOAD`); occlusion engages automatically for `D16/D32/D24S8/D32S8` depth views, otherwise it is skipped (mirroring GL's `bits==0` branch). Any bloom resource failure disables bloom with a warning instead of throwing.
- **The single-header build stays GL-only**; the Vulkan backend ships with the modular build only.
- **Validation**: with `EMBER_VK_DEBUG=1`, `makeVulkanDevice()` enables validation layers (degrades silently when absent). Multi-backend coexistence needs no special handling: a GL context and a VkDevice can live in one process, each using its own handles.
- **Host command-buffer recording (WO-10)**: engines that own their command buffers can have `update()`/`render()` record into them instead of letting the backend submit:

```cpp
VkCommandBuffer cmd = /* your already-begun command buffer */;
ember::beginVulkanFrame(sys.backend(), cmd, frameCounter);
sys.update(dt);
sys.render(view, proj, 1920.f, 1080.f, 50.f);
ember::endVulkanFrame(sys.backend());
/* submit and wait your cmd yourself */
auto stats = sys.synchronizeStatistics(); // or aliveCount()/readParticles()
```

  Inside the window the backend does not submit, signal fences, or rotate its frame ring. The host must keep the `kFramesInFlight=2` resource discipline (do not record frame N+2 into a slot whose frame N command buffer is in flight) and must submit and wait its command buffer before exact statistics/readback calls. `synchronizeStatistics()`/`aliveCount()`/`readParticles()` throw `std::logic_error` while the window is open; `setVulkanFrameTarget` must be set before `beginVulkanFrame` and its `frameIndex` must resolve to the same slot.

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
The repo vendors its dependency tarballs in `deps/`: `cmake -S . -B build -DEMBER_DEPS_DIR=<absolute path to deps>` and FetchContent extracts locally — fully offline. The glad generator needs local Python 3 + `jinja2`. Generation uses `REPRODUCIBLE` and the bundled specification, without downloading gl.xml.

**Q: How do I wire up the single-header build?**
The essentials are glad (`glad.h` + `glad.c`) and glm. Copy `single_header/ember.hpp` into your project, `#define EMBER_IMPLEMENTATION` and include it once in one .cpp; compile `glad.c` as usual and call `ember::gl::init(loader)`. A complete example is `tests/single_header_test.cpp` (depends only on the single header + glad/glfw/glm — not on the ember library).

**Q: Can I drop the stb_image PNG dependency?**
Yes. Don't define `EMBER_USE_STB` (modular path: don't compile `src/backends/opengl/stb_image.cpp`) — `setSpriteTexture` falls back to the built-in gradient with a warning.

**Q: Will the interactive editor (WASD-move emitters etc.) leak into my project?**
No. The editor lives only in the example programs, gated by the CMake option `EMBER_BUILD_EDITOR` (default ON, examples only); `-DEMBER_BUILD_EDITOR=OFF` or dropping the examples removes it entirely. The library itself contains no editor code; the runtime toggle is `[system] editor = false`.

**Q: I only have OpenGL 3.3 — what now?**
Compute shaders (4.3) are a hard requirement. Fallback: CPU simulation + instanced rendering (write your own integrate loop into a particle SSBO; the render VS/FS and `render()` can still be reused); or port the simulation kernel to Vulkan/D3D12.

**Q: Does it work on macOS?**
No. macOS caps at GL 4.1 (no compute). Alternatives: port the simulation to Metal, or update particle data on the CPU/compute queue before `render()` (see §9).

**Q: Does the 20-byte per-frame readback stall?**
In default mode it can. Use `setGpuDriven(true)` with `pollStatistics()` to remove this per-update dependency (see section 7). Explicit exact queries still synchronize when needed.

**Q: How do I pick the particle capacity?**
Steady-state live particles ≈ total spawn rate × average lifetime. Set `capacity` to that plus headroom; `maxSpawnPerFrame` guards against single-frame spikes. Example: 7000/s × 2.75s ≈ 19k, config uses 150k for lots of headroom. See `config/stress.ini` (1M capacity, ~850k steady state).

**Q: Config file changes don't take effect?**
Check the path and the `[section]` syntax (`#` comments, `key = value`, comma-separated vectors). Parse errors throw a `std::runtime_error` with a line number; the examples print it and keep the previous config. Enum values like `blend`/`shape` are case-insensitive.

**Q: Do slots leak when the spawn rate drops below the death rate?**
No. Each retired slot is pushed once and reused before appending. The allocated extent can exceed the current population; clear() resets it.
