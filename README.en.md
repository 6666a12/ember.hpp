# ember

A **modern OpenGL (4.3 core)** GPU particle animation library. Simulation runs on compute shaders; rendering uses instanced billboard quads (view-aligned). The core library **does not depend on any window system** — it can be embedded into any C++ stack (GLFW / SDL2 / Qt / Win32 / EGL / your own engine), and ships as a **single header** (`single_header/ember.hpp`) that injects the whole library into your project.

> 🌏 中文: [README.md](README.md) · Integration guide: [INTEGRATION.en.md](INTEGRATION.en.md) / [INTEGRATION.md](INTEGRATION.md)

## Features

- **GPU simulation**: integrate live indices, reserve birth slots in a batch, spawn, then publish indirect draw args. Particle and live-index buffers are ping-ponged; sampling happens on the GPU. The CPU encodes compact **spawn requests** (~176 B each) and, by default, reads back 20 bytes (GPU scheduling can avoid this synchronous read)
- **Automatic slot recycling**: batch reservation reuses free-stack slots without contended per-particle CAS; stable slots preserve particle identity
- **Force fields**: gravity / drag (linear·quadratic) / uniform wind / value-noise turbulence / point attractors / vortices / springs / spatial noise wind / traveling waves / plane boundary (kill·bounce); every force can be **toggled independently** (`enableForce`/`disableForce` bitmask)
- **Emitters**: Point / Box / Sphere / Cone shapes with randomized speed / lifetime / size / color ranges; cone half-angle, size↔speed link (bigger particles fly faster)
- **Editable config layer**: INI-style files (`config/example.ini`) expose every parameter; examples hot-reload on `R`; programmatic APIs (`setGravity`/`setDrag`/`setAttractors`/…) coexist
- **Interactive emitter editor** (examples only, `EMBER_BUILD_EDITOR`): `1-9` pick source, `WASD` move, mouse place, `[ ]` scale shape, `- =` rate, `P` print config snippet
- **Sprites**: built-in radial gradient + optional PNG (stb_image), sprite-sheet frame animation, fade-to-color on death
- **Billboard quad rendering**: view-aligned quads (`TRIANGLE_STRIP`×4, no vertex attributes — expanded from `gl_VertexID` in the VS), replacing point sprites
- **Streaks**: centered quads stretch along projected particle motion, including perspective depth motion; `sys.setStreak(k)`. This is instantaneous stretching without position history; see the [streak notes](docs/streak.md).
- **Billboard spin**: random static angle + age-based spin (`sys.setSpin(speed)` / `[system] spin`) — tumbling leaves/confetti/shards
- **Refractive particles (glass shards)**: screen-space refraction — particles sample the host's scene texture with per-particle facet offsets (`setOpenGLRefraction(sys, ...)` / `[system] refraction`); glass/heat/water/prism presets (`ember::Refraction::glass()/heat()/water()/prism()`); see `example_glass`
- **Soft particles**: sample a host-provided scene depth texture and fade near occluders (`setOpenGLSoftParticles(sys, on, depthTex, radius)`)
- **GPU depth sort**: compute bitonic sort reorders draws far→near for correct alpha blending (`sys.setSortEnabled(true)`); off by default
- **HDR bloom post-processing**: built-in FBO chain (bright pass → 2×2 downsample → two Gaussian blurs → composite onto the host framebuffer), `sys.setBloom(true)` / `B` key
- **Indirect rendering**: draw instance count comes from a GPU-written args buffer (`glDrawArraysIndirect`) — the CPU readback is not on the render path
- **Event sub-emission**: particles spawn a child batch in the same frame on death/bounce (`addEventEmitter` + `Emitter::onDeath/onBounce`, INI `[event "name"]` / `on_death` / `on_bounce`), chaining supported (multi-stage fireworks), child velocity inherits the parent via `inheritVelocity`; both backends match bit-for-bit
- **Lifecycle curves**: color/size over lifetime (`setColorOverLife` / `setSizeOverLife` / INI `[curves]`); the facade bakes 64-entry LUTs with multiply semantics, and an empty curve renders bit-identically to the previous behavior
- **Embed-friendly**: CMake `add_subdirectory` / `FetchContent` / `find_package`, direct source compilation, or the **single header** `single_header/ember.hpp`

## Backend organization

The CPU facade and the graphics backends are separate: `ember::core` depends only on glm; `ember::opengl` supplies GL 4.3, while the existing `ember` target and default constructor remain compatible; `ember::vulkan` (`-DEMBER_BUILD_VULKAN=ON`, default) supplies Vulkan 1.1. Use `-DEMBER_BUILD_OPENGL=OFF` for core only, or `-DEMBER_BUILD_GLFW=OFF` to omit the window module. Implement `ParticleBackend` to inject another backend with individually declared optional capabilities.

See the [backend contract and migration map](docs/backends.md) (Chinese). Two graphics backends are implemented: OpenGL 4.3 and Vulkan 1.1 (GPU scheduling, depth sorting, sprites, soft particles, refraction, bloom, event sub-emission and lifecycle curves are all aligned; `capabilities()` returns all true; Vulkan also supports host command-buffer recording via `beginVulkanFrame`/`endVulkanFrame`). The Vulkan module only builds when Vulkan-Headers and a loader are found; otherwise it is skipped silently.

Effect fixes, regression coverage and remaining approximations are recorded in the [effect audit](docs/effects-audit.md) (Chinese).

## Dependencies

The table below describes the default OpenGL build; core only requires glm.

| Dependency | Role | Required | Notes |
| --- | --- | --- | --- |
| OpenGL **4.3 core** | runtime | ✅ | compute shaders + SSBO (macOS caps at 4.1 — unsupported, see INTEGRATION §8) |
| glad (v2, `gl:core=4.3`) | GL entry-point loader | ✅ | include its generated header + compile `glad.c`; `ember::gl::init(loader)` |
| glm (≥ 0.9.9) | math (header-only) | ✅ | vec/mat types |
| GLFW 3.4 | windows (examples / `ember_glfw` module / `EMBER_USE_GLFW`) | optional | not needed by the core |
| stb_image | PNG sprites (`EMBER_USE_STB`) | optional | without it `setSpriteTexture` falls back to the built-in gradient |
| Vulkan-Headers + loader (1.1+) | Vulkan backend | optional | only for `-DEMBER_BUILD_VULKAN=ON` (default); missing → module skipped. glslangValidator builds the SPIR-V |
| CMake ≥ 3.16 | build | optional | non-CMake projects compile the sources / single header directly |
| Python 3 | only when FetchContent pulls glad | optional | glad generator (offline builds need jinja2) |

> The single-header edition (`single_header/ember.hpp`) has the same documented dependencies (glad/glm; GLFW/stb_image are opt-in macros) — see [INTEGRATION.en.md §3.4](INTEGRATION.en.md).

## Build & run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# run from the project root (a config path can be passed as the first argument)
./build/example_basic
./build/example_fireworks
```

Offline / restricted-network builds: the repo vendors dependency tarballs in `deps/`; configure with `-DEMBER_DEPS_DIR=<absolute path>` to go fully offline (glad generation still needs local Python 3 + jinja2). Use `-DEMBER_FETCH_DEPS=OFF` if deps are already installed system-wide.

Demo controls: left-drag = orbit camera, wheel = zoom, `R` = hot-reload config, `ESC` = quit. Stress test: `./build/example_basic config/stress.ini` (1M particle capacity).

```bash
./build/example_glass          # refractive glass shards (G cycles presets, SPACE bursts)
```

## Quick start

```cpp
#include "ember/particle_system.hpp"

ember::ParticleSystem sys({150000, 40000});       // capacity, maxSpawnPerFrame
sys.setGravity({0.f, -7.f, 0.f});
sys.setDrag(0.05f);
sys.setTurbulence(0.4f);
sys.setAttractors({{/* position */ {0, 1, 0}, /* strength */ 60.f}});
sys.setStreak(1.2f);                            // spark trails
sys.setSortEnabled(true);                       // GPU depth sort
sys.setBloom(true);                             // HDR bloom
// Soft particles (host passes its scene depth texture):
//   setOpenGLSoftParticles(sys, true, depthTex, 0.6f);

auto& fountain = sys.addEmitter();
fountain.shape = ember::Emitter::Shape::Cone;
fountain.rate = 7000.f;
fountain.baseVelocity = {0.f, 10.f, 0.f};
fountain.speedMin = 7.f;   fountain.speedMax = 11.f;
fountain.lifeMin = 2.f;    fountain.lifeMax = 3.5f;
fountain.colorMin = {1.f, 0.8f, 0.3f, 1.f};
fountain.colorMax = {1.f, 0.2f, 0.05f, 1.f};

// every frame (inside your render loop):
sys.update(dt);                                                 // GPU simulation
sys.render(cam.view(), cam.proj(), fbWidth, fbHeight, cam.fovY); // instanced billboards
```

### Config files (editable surface)

Edit `config/example.ini` (gravity, wind, turbulence, blend mode, every emitter parameter, palettes, attractors, render extras: streak / soft / bloom / sort) — hot-reloaded on `R`:

```ini
[system]
gravity     = 0, -7, 0
turbulence  = 0.4
blend       = additive
forces      = gravity, drag, wind, turbulence, attractors, vortex, noise_wind
streak          = 1.2       # >0: stretch quads along velocity
soft_particles  = false     # needs a host-provided scene depth texture
bloom           = true      # HDR bloom post-processing
bloom_threshold = 0.8       # bright-pass cutoff (default 1.0)
sort            = false     # GPU depth sort

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

In code: `sys.loadConfig("config/example.ini")` or `sys.apply(Config::fromFile(...))`. Config changes are **cumulative** (unmentioned keys keep their current value); changing `capacity` rebuilds the buffers and clears particles. Diagnostics: set `debug = true` in the config (or `EMBER_DEBUG=1`) to get per-6-frame stderr lines with alive / request count / force mask + a GL error sweep.

## Size / sprites / editor

- **Size↔speed link**: `sys.setSizeScale(1.5f)` or `[system] size_scale = 1.5` scales particles globally; with `scale_speed_with_size = true` (default) spawn speeds scale proportionally so trajectories stay correct; `[emitter] speed_scale` adjusts per-emitter speed, `speed_size_link` makes bigger particles fly faster within one emitter
- **Sprites**: built-in 64×64 radial gradient (zero deps); `sys.setSpriteTexture("sprites/glow.png")` or `[system] texture = ...` loads a PNG (stb_image, `EMBER_USE_STB`), falling back to the gradient on failure; `sys.setSpriteSheet(cols, rows)` enables frame animation; `T` toggles texture vs procedural glow
- **Fade-to-color**: `[emitter] fade_color_min/max` sets the RGB particles fade INTO at death (stored in `life.yzw`, still 64 bytes)
- **Interactive emitter editor** (examples, `EMBER_BUILD_EDITOR`): `1-9` select, `WASD/space/shift` move, right-click place, `[ ]` scale shape, `,` `.` cone angle, `-` `=` rate, `P` print INI snippet, `B` bloom toggle, `O` sort toggle

## Architecture

### Particle layout (CPU and GPU std430 identical, `static_assert sizeof==64`)

| Offset | Field | Meaning |
| --- | --- | --- |
| 0 | `pos` vec4 | xyz position, w = world size |
| 16 | `vel` vec4 | xyz velocity, w = age |
| 32 | `life` vec4 | x = lifetime; x<0 = corpse slot; **yzw = fade target RGB** |
| 48 | `color` vec4 | rgba |

### Default synchronous frame pipeline

```
CPU encodes spawn requests (emitters + bursts, ~176 B each, capped at maxSpawnPerFrame)
   → request SSBO + uSpawnRequestCount + uSpawnTotal
   → phase=0: integrate live indices, retire slots, append survivors to next list; death/bounce events queue per slot tags [barrier]
   → phase=4: event compaction (with event templates): child prefix sums and budget clamp [barrier]
   → phase=3: reserve the whole free/append batch (when spawning, event children included) [barrier]
   → phase=1: sample particles and append their indices (when spawning) [barrier]
   → phase=2: publish indirect instanceCount in one invocation [barrier]
   → read first five counters (20 B) → swap particles and live-index lists
   → [optional] GPU bitonic sort (far→near; VS reads sorted[] when uUseSorted=1)
   → glDrawArraysIndirect (instance count from the GPU args buffer)
```

`SpawnRequest` (176 B std430, see `ember/emitters.hpp`) carries shape / cone / speed / life / size ranges / palette index etc.; the GPU samples each particle with a hash RNG — the CPU uploads only a few KB of requests per frame instead of full particles.

### Force fields (10, independently toggleable)

`sys.enableForce(Force::X)` / `sys.disableForce(Force::X)`; the `Force` enum value is the bit index, query with `sys.forceMask()`. Default: the original 5 forces on, the 5 newer ones off.

| Category | Force | Formula / behavior | API | Config key |
| --- | --- | --- | --- | --- |
| Basic | Gravity | `a += g` | `setGravity` | `gravity` |
| | Drag | `a -= v·k` (linear) or `-v·\|v\|·k` (quadratic) | `setDrag` + `setDragMode` | `drag` / `drag_mode` |
| | Wind | `a += w` | `setWind` | `wind` |
| | Turbulence | `a += (noise3(p·s+t)·2−1)·k` | `setTurbulence` | `turbulence` |
| Spatial | Attractors | `a += d·s·r⁻³` (± attract/repel, multiple) | `setAttractors` | `[attractor]` |
| | Vortex | `a += cross(axis, d)·s/(r²+r0²)` (tangential swirl, multiple) | `setVortexes` | `[vortex]` |
| | Spring | `a += (anchor−p)·k − v·c` (multiple) | `setSprings` | `[spring]` |
| | Noise wind | `a += dir·(noise3(p·scale+t·speed)·2−1)·amp` | `setNoiseWind` | `noise_wind_*` |
| | Wave | `a += dir·sin(dot(p,k)+ωt)·amp` | `setWave` | `wave_*` |
| Effects | Boundary | `y < planeY` → kill or bounce (restitution) | `setBoundary` | `boundary_mode/y/restitution` |

## Custom shaders / forces

Default shaders load **from `shaders/` first** (`particle.vert` / `particle.frag` / `simulate.comp` / `sort.comp` / bloom trio; the directory is configurable via `sys.setShaderDirectory()`); missing or broken files fall back to the embedded copies (zero-file embedding). Full customization:

```cpp
ember::Shader render = ember::Shader::fromFiles({{GL_VERTEX_SHADER, "my.vert"},
                                                 {GL_FRAGMENT_SHADER, "my.frag"}});
ember::Shader sim = ember::Shader::fromFiles({{GL_COMPUTE_SHADER, "my.comp"}});
setOpenGLPrograms(sys.backend(), std::move(render), std::move(sim));
```

Custom shaders must follow this contract (uniform names + SSBO bindings):

| Binding | Buffer | Purpose |
| --- | --- | --- |
| 0 | `cur` / `particles` | current particles (sim reads and writes retirement tombstones; render VS reads) |
| 1 | `nxt` | next particles; phases 0/1 write |
| 2 | `req` | spawn requests (`SpawnRequest[]`, read-only; GPU samples spawns) |
| 3 | `dead` | free-slot stack (coherent) |
| 4 | `counters` | `uAlive uDeadHead uSpawnRequestCount uCapacity uAllocated uSpawnReuse uSpawnAppendBase uSpawnAccepted` (coherent) |
| 5 | `attractors` | vec4 attractors (read-only) |
| 6 | `vortexes` | `Vortex` array (read-only) |
| 7 | `springs` | `Spring[]`: std430 stride **32 B**, offsets anchor=0, stiffness=12, damping=16 |
| 8 | `sorted` | GPU-sorted particle indices (render VS read, optional) |
| 9 | `palette` | palette colors `vec4[]` (GPU spawn, read-only) |
| 10 | `indirect` | indirect draw args (written by phase 2; read by `glDrawArraysIndirect`) |
| 11 | `liveIndices` | current live indices; sim/render/sort read; swapped with next list after update |
| 12 | `nextLiveIndices / sortKeys` | next live-list output during sim; separate cached-depth buffer during sort |
| 13 | `schedule` | GPU scheduling metadata and indirect compute commands |

> ⚠️ **Breaking change (0.x)**: binding 2 went from "full `Particle[]` staging" to "`SpawnRequest[]` spawn requests" — hosts with custom spawn shaders must migrate to the new contract. The built-in shaders' loose uniforms are now std140 uniform blocks (contract above); the `ParticleSystem::setPrograms/setRefraction/setSoftParticles` members were removed in favor of the `ember/opengl.hpp` free functions `setOpenGLPrograms(sys.backend(), ...)` / `setOpenGLRefraction(sys, ...)` / `setOpenGLSoftParticles(sys, ...)` — the public facade no longer exposes GL types, so non-GL backends need not define behavior for them.

The built-in shaders no longer use loose uniforms — every scalar lives in a std140 uniform block (the same GLSL compiles to SPIR-V as-is; CPU mirrors with layout asserts are in `src/backends/opengl/params.hpp`): simulation `SimParams` (binding 14; `uPhase=0` integrate live indices, `=3` reserve birth slots, `=1` spawn, `=2` publish indirect args — up to four dispatches; `uSpawnTotal` = total particles requested this frame, `uFrameSeed` = GPU spawn RNG seed; `uForceMask` bit i = `ember::Force` value i, disabled forces cost nothing); render VS `DrawParams` (binding 17; `uUseSorted=1` reads particles via `sorted[gl_InstanceID]`, else `liveIndices[gl_InstanceID]`; `uStreak>0` stretches quads along the view-projected velocity); render FS `FragParams` (binding 18; `uUseSprite=1` texture + frame animation, `=0` procedural glow; `uUseSoft=1` fades near scene depth; `vFadeRGB` comes from `life.yzw` for the fade-to-color effect); sort `SortParams` (15), internal scheduling `ScheduleParams` (16), bloom `BloomParams` (19). Samplers carry explicit bindings: `uSprite`=0, `uSceneDepth`=1, `uSceneColor`=2; stage varyings carry explicit locations. Block members without an instance name share one global namespace, so the fragment block renames `uProj`/`uRefraction` to `uFragProj`/`uFragRefraction`. The backend still writes the legacy loose uniforms too, so custom shaders written against the old contract keep working; new custom shaders should declare the same block members (protocol detection sees block members as well).

## Embedding

See **[INTEGRATION.en.md](INTEGRATION.en.md)** (or the Chinese [INTEGRATION.md](INTEGRATION.md)): the three CMake consumption modes, non-CMake workflows, the **single-header edition**, per-stack wiring (GLFW/SDL2/Qt/Win32/EGL), coexistence with a host renderer, macOS/4.1 notes, FAQ.

## Performance

Reproducible CPU/GPU benchmarks, measurement definitions, and recorded results are documented in [benchmarks/README.md](benchmarks/README.md). Build with `-DEMBER_BUILD_BENCHMARKS=ON`, then run `python tools/run_benchmarks.py --output out/baseline-local`.

- Default mode synchronously reads five counters (20 B) per update. Opt in with `sys.setGpuDriven(true)` for GPU-generated integration/sort dispatch and asynchronous telemetry; use `pollStatistics()` for UI. `aliveCount()` stays exact and may wait. See [integration guide](INTEGRATION.en.md#gpu-scheduling-and-asynchronous-statistics-opt-in).
- Particle storage: capacity × 128 B for particles, × 4 B for free indices, × 8 B for live-index ping-pong, plus nextPow2(capacity) × 4 B for sorted indices. Sorting lazily allocates up to another nextPow2(capacity) × 4 B for cached depth keys.
  - 1M capacity uses about 144 MB (138 MiB), or 148 MB (142 MiB) with a full depth-key cache, excluding requests, palettes and post-processing.
- The benchmark report records the first measurements after the lifecycle fixes; measure again on the target GPU and actual integration workload.
- Retired slots remain in the free stack; no entries are dropped. Built-in integration and rendering visit only live indices; legacy custom simulation shaders can still scan the allocated extent.
- Optional post-processing is opt-in (all off by default): bloom = one full-res and two half-res RGBA16F FBOs + 2 Gaussian blur passes per frame; depth sort caches depth keys and merges intra-tile steps in shared memory (256-entry tiles), totaling 66 dispatches at N=262144 or 91 at N=1M, including initialization. Ordering remains exact, far to near.

## Validation and shader compatibility

Run `ctest --test-dir build --output-on-failure`. The GPU suites cover lifecycle, event sub-emission, sorting, bloom/occlusion, refraction, soft particles, lifecycle curves, host command-buffer recording and embedded-fallback regressions; `cross_backend_test` replays one command stream across GL / Vulkan-sync / Vulkan-GPU-scheduling legs and compares the simulation state frame by frame; Python checks verify generated artifacts.

See [INTEGRATION.en.md](INTEGRATION.en.md) for the optimized shader protocol and legacy fallback. Binding 12 and three trailing counter words are new; the first five counters, 32-byte Spring arrays and uInvProj = inverse(proj) are unchanged. Requests are capped at 4096 entries and `maxSpawnPerFrame` particles; bursts precede emitters. Edit shaders, run `python tools/sync_embedded_shaders.py`, then `python tools/amalgamate.py`.

## License

MIT.
