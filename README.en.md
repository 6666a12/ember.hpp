# ember

A **modern OpenGL (4.3 core)** GPU particle animation library. Simulation runs on compute shaders; rendering uses instanced billboard quads (view-aligned). The core library **does not depend on any window system** — it can be embedded into any C++ stack (GLFW / SDL2 / Qt / Win32 / EGL / your own engine), and ships as a **single header** (`single_header/ember.hpp`) that injects the whole library into your project.

> 🌏 中文: [README.md](README.md) · Integration guide: [INTEGRATION.en.md](INTEGRATION.en.md) / [INTEGRATION.md](INTEGRATION.md)

## Features

- **GPU simulation**: three-stage compute pipeline (integrate / spawn / indirect draw args) with SSBO ping-pong buffers; particle sampling (shapes / cones / palettes / fade / size↔speed link) happens **entirely on the GPU** — the CPU encodes a few compact **spawn requests** (~176 B each) per frame and reads back 16 bytes
- **Automatic slot recycling**: a free-stack (atomic CAS pop + LIFO, concurrency-safe) reuses dead slots — no compaction, zero alive-count drift
- **Force fields**: gravity / drag (linear·quadratic) / uniform wind / value-noise turbulence / point attractors / vortices / springs / spatial noise wind / traveling waves / plane boundary (kill·bounce); every force can be **toggled independently** (`enableForce`/`disableForce` bitmask)
- **Emitters**: Point / Box / Sphere / Cone shapes with randomized speed / lifetime / size / color ranges; cone half-angle, size↔speed link (bigger particles fly faster)
- **Editable config layer**: INI-style files (`config/example.ini`) expose every parameter; examples hot-reload on `R`; programmatic APIs (`setGravity`/`setDrag`/`setAttractors`/…) coexist
- **Interactive emitter editor** (examples only, `EMBER_BUILD_EDITOR`): `1-9` pick source, `WASD` move, mouse place, `[ ]` scale shape, `- =` rate, `P` print config snippet
- **Sprites**: built-in radial gradient + optional PNG (stb_image), sprite-sheet frame animation, fade-to-color on death
- **Billboard quad rendering**: view-aligned quads (`TRIANGLE_STRIP`×4, no vertex attributes — expanded from `gl_VertexID` in the VS), replacing point sprites
- **Streak trails**: quads stretch along the view-projected velocity; `sys.setStreak(k)`
- **Soft particles**: sample a host-provided scene depth texture and fade near occluders (`sys.setSoftParticles(on, depthTex, radius)`)
- **GPU depth sort (OIT)**: compute bitonic sort reorders draws far→near for correct alpha blending (`sys.setSortEnabled(true)`); off by default
- **HDR bloom post-processing**: built-in FBO chain (bright pass → 2×2 downsample → two Gaussian blurs → composite onto the host framebuffer), `sys.setBloom(true)` / `B` key
- **Indirect rendering**: draw instance count comes from a GPU-written args buffer (`glDrawArraysIndirect`) — the CPU readback is not on the render path
- **Embed-friendly**: CMake `add_subdirectory` / `FetchContent` / `find_package`, direct source compilation, or the **single header** `single_header/ember.hpp`

## Dependencies

| Dependency | Role | Required | Notes |
| --- | --- | --- | --- |
| OpenGL **4.3 core** | runtime | ✅ | compute shaders + SSBO (macOS caps at 4.1 — unsupported, see INTEGRATION §8) |
| glad (v2, `gl:core=4.3`) | GL entry-point loader | ✅ | include its generated header + compile `glad.c`; `ember::gl::init(loader)` |
| glm (≥ 0.9.9) | math (header-only) | ✅ | vec/mat types |
| GLFW 3.4 | windows (examples / `ember_glfw` module / `EMBER_USE_GLFW`) | optional | not needed by the core |
| stb_image | PNG sprites (`EMBER_USE_STB`) | optional | without it `setSpriteTexture` falls back to the built-in gradient |
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

## Quick start

```cpp
#include "ember/particle_system.hpp"

ember::ParticleSystem sys({150000, 40000});       // capacity, maxSpawnPerFrame
sys.setGravity({0.f, -7.f, 0.f});
sys.setDrag(0.05f);
sys.setTurbulence(0.4f);
sys.setAttractors({{/* position */ {0, 1, 0}, /* strength */ 60.f}});
sys.setStreak(1.2f);                            // spark trails
sys.setSortEnabled(true);                       // GPU depth sort (OIT)
sys.setBloom(true);                             // HDR bloom
// Soft particles (host passes its scene depth texture):
//   sys.setSoftParticles(true, depthTex, 0.6f);

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
sort            = false     # GPU depth sort (OIT)

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

### Frame pipeline (rendering never depends on the CPU readback; the only sync is the 16-byte alive readback for UI/API)

```
CPU encodes spawn requests (emitters + bursts, ~176 B each, capped at maxSpawnPerFrame)
   → request SSBO + uSpawnRequestCount + uSpawnTotal
   → dispatch A (phase=0): integrate + retire into the free stack   [barrier]
   → dispatch B (phase=1): GPU samples spawns (recycle slot or append) [barrier]
   → dispatch C (phase=2): write glDrawArraysIndirect args           [barrier]
   → read back uAlive (16 bytes, for aliveCount()/UI) → swap cur/nxt
   → [optional] GPU bitonic sort (far→near; VS reads sorted[] when uUseSorted=1)
   → glDrawArraysIndirect (instance count from the GPU args buffer)
```

`SpawnRequest` (176 B std430, see `ember/emitters.hpp`) carries shape / cone / speed / life / size ranges / palette index etc.; the GPU samples each particle with a hash RNG — the CPU uploads only a few KB of requests per frame instead of full particles.

### Force fields (11, independently toggleable)

`sys.enableForce(Force::X)` / `sys.disableForce(Force::X)`; the `Force` enum value is the bit index, query with `sys.forceMask()`. Default: the original 5 forces on, the 6 newer ones off.

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
sys.setPrograms(std::move(render), std::move(sim));
```

Custom shaders must follow this contract (uniform names + SSBO bindings):

| Binding | Buffer | Purpose |
| --- | --- | --- |
| 0 | `cur` / `particles` | current particles (sim read / render VS read) |
| 1 | `nxt` | next-frame particles (sim write) |
| 2 | `req` | spawn requests (`SpawnRequest[]`, read-only; GPU samples spawns) |
| 3 | `dead` | free-slot stack (coherent) |
| 4 | `counters` | `uAlive uDeadHead uSpawnRequestCount uCapacity` (coherent) |
| 5 | `attractors` | vec4 attractors (read-only) |
| 6 | `vortexes` | `Vortex` array (read-only) |
| 7 | `springs` | `Spring` array (read-only) |
| 8 | `sorted` | GPU-sorted particle indices (render VS read, optional) |
| 9 | `palette` | palette colors `vec4[]` (GPU spawn, read-only) |
| 10 | `indirect` | indirect draw args (written by phase 2; read by `glDrawArraysIndirect`) |

> ⚠️ **Breaking change (0.x)**: binding 2 went from "full `Particle[]` staging" to "`SpawnRequest[]` spawn requests" — hosts with custom spawn shaders must migrate to the new contract.

sim uniforms: `uDt uTime uPhase uForceMask uGravity uDrag uDragMode uWind uTurbulence uAttractorCount uVortexCount uSpringCount uNoiseWindDir uNoiseWindAmp uNoiseWindScale uNoiseWindSpeed uWaveDir uWaveK uWaveAmp uWaveOmega uBoundaryMode uBoundaryY uRestitution uSpawnTotal uFrameSeed` (`uPhase=0` integrate, `=1` spawn, `=2` write indirect args — three dispatches; `uSpawnTotal` = total particles requested this frame, `uFrameSeed` = GPU spawn RNG seed; `uForceMask` bit i = `ember::Force` value i, disabled forces cost nothing); render VS uniforms: `uView uProj uSizeScale uStreak uUseSorted` (`uUseSorted=1` reads particles via `sorted[gl_InstanceID]`, else `gl_InstanceID`; `uStreak>0` stretches quads along the view-projected velocity); render FS uniforms: `uSprite uUseSprite uSheetCols uSheetRows uSceneDepth uInvViewProj uSoftRadius uViewportSize uUseSoft` (`uUseSprite=1` texture + frame animation, `=0` procedural glow; `uUseSoft=1` fades near scene depth; `vFadeRGB` comes from `life.yzw` for the fade-to-color effect).

## Embedding

See **[INTEGRATION.en.md](INTEGRATION.en.md)** (or the Chinese [INTEGRATION.md](INTEGRATION.md)): the three CMake consumption modes, non-CMake workflows, the **single-header edition**, per-stack wiring (GLFW/SDL2/Qt/Win32/EGL), coexistence with a host renderer, macOS/4.1 notes, FAQ.

## Performance

- Rendering is decoupled from the CPU readback: the instance count comes from a GPU-written indirect args buffer; per frame the CPU→GPU traffic is a few spawn requests (`emitter count × 176 B`) and the GPU→CPU traffic is a single 16-byte alive counter (for `aliveCount()`/UI only)
- Size `capacity` for the worst-case live count (memory = capacity × 64 B × 2 + free stack × 4 B + sort indices × 4 B); steady state ≈ spawn rate × average lifetime
  - 1M particles ≈ **130 MB** VRAM (two 64 MB particle buffers + 4 MB free stack + 4 MB sort indices); 4M ≈ 520 MB
- Reference measurement (local AMD driver, GL 4.3): `config/stress.ini` (1M capacity, post-processing off) holds **~850k live particles at ~166 fps**
- If the spawn rate stays below the death rate for a long time, the ring free-stack may drop its oldest entries (those slots stay corpses — no visual artifacts)
- Optional post-processing is opt-in (all off by default, zero cost): bloom = 3 half-res RGBA16F FBOs + 2 Gaussian blur passes per frame; depth sort = ~log²N tiny compare-exchange dispatches (≈171 at N=262144, ≈400 at N=1M — off by default; for huge counts reduce sort frequency or switch to a radix sort)

## License

MIT.
