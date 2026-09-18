#include "common.hpp"
namespace ember::detail::opengl {
const char* const kScheduleComp = R"GLSL(#version 430 core
// Internal scheduler: never overridden via setShaderDirectory. All commands
// are bounded by capacity, which the host validates against GL device limits.
layout(local_size_x=1) in;
layout(binding=4,std430) buffer Counters {
    uint alive; uint deadHead; uint requests; uint capacity; uint allocated;
};
layout(binding=10,std430) buffer Draw {
    uint vertexCount; uint instanceCount; uint firstVertex; uint baseInstance;
};
// words[0]=inputAlive, [1]=padded sort size, [2..4]=integration command,
// [5..]=three-word tiled sort commands in host submission order.
layout(binding=13,std430) buffer Schedule { uint words[]; };
// Event queue header (WO-08): reset here alongside the other frame counters.
layout(binding=22,std430) buffer EventQueue { uint head; uint totalChildren; };
// CPU mirror: ScheduleParams in src/backends/opengl/params.hpp.
layout(binding = 16, std140) uniform ScheduleParams {
    int uSchedulePhase; uint uRequestCount;
};
uint padded(uint n) {
    uint p=1u; while(p<n) p<<=1u; return p;
}
void command(uint offset,uint x) {words[offset]=x;words[offset+1u]=1u;words[offset+2u]=1u;}
void main() {
    uint count=min(alive,capacity);
    if(uSchedulePhase==0) {
        words[0]=count;
        command(2u,(count+63u)/64u);
        requests=uRequestCount;
        vertexCount=4u;instanceCount=0u;firstVertex=0u;baseInstance=0u;
        head=0u;totalChildren=0u;
    } else {
        uint n=padded(count),maxN=padded(capacity);
        words[1]=n;
        uint groups=count==0u?0u:(n+255u)/256u;
        command(5u,groups);
        uint offset=8u;
        for(uint k=512u;k<=maxN;k<<=1u) {
            uint stageGroups=k<=n?groups:0u;
            for(uint j=k>>1u;j>=256u;j>>=1u) {command(offset,stageGroups);offset+=3u;}
            command(offset,stageGroups);offset+=3u;
        }
    }
}
)GLSL";

const char* const kSimulateComp = R"GLSL(#version 430 core
// Stable slots: phase 0 integrates live indices; phase 3 reserves spawn slots;
// phase 1 spawns; phase 2 publishes draw args. Live lists are ping-ponged.
// GPU scheduling: uGpuDriven=1 reads input count from binding 13 word 0;
// uGpuDriven=0 uses uInputAlive. schedule.comp resets draw/request counters
// before phase 0. Keep both uniforms active for host protocol detection.
layout(local_size_x = 64) in;

struct Particle {
    vec4 pos;   // xyz position, w = size
    vec4 vel;   // xyz velocity, w = age
    vec4 life;  // x = lifetime (negative => corpse slot owned by the free stack)
    vec4 color; // rgba
};

layout(binding = 0, std430) buffer BufCur   { Particle cur[]; };
layout(binding = 1, std430) buffer BufNext  { Particle nxt[]; };
// Spawn requests: the CPU encodes emitters/bursts as compact requests; this
// shader samples the actual particles (shape, cone, palette, fade, size<->speed
// link) with its own hash RNG. Layout matches ember::SpawnRequest (std430).
struct SpawnRequest {
    vec4  colorMin;
    vec4  colorMax;
    vec3  position;
    float radius;
    vec3  axis;          // cone axis (normalized; zero = no cone)
    float coneAngle;     // degrees; >0 => cone emission
    vec3  dir;           // normalized base velocity (zero = uniform sphere)
    float spread;        // 0..1 mix toward a random sphere direction
    vec3  extents;       // box half-extents
    float speedMin;      // already includes speedScale
    float speedMax;
    float speedSizeLink; // per-particle: speed *= 1 + link*(size/sizeMid - 1)
    float lifeMin;
    float lifeMax;
    float sizeMin;
    float sizeMax;
    int   shape;         // 0 point, 1 box, 2 sphere, 3 cone
    int   paletteIdx;    // base index into the palette buffer (-1 = colorMin/Max)
    int   flags;         // bit0 = fade-to-color, bit1 = refractive (sign-encoded in pos.w)
    uint  base;          // prefix offset: spawns work ids [base, base+count)
    uint  count;
    int   paletteCount;  // colors in this palette range
    vec3  fadeMin;       // fade target range (RGB)
    uint  pad2;
    vec3  fadeMax;
    uint  pad3;
};
layout(binding = 2, std430) readonly  buffer BufSpawnReq { SpawnRequest req[]; };
// Event sub-emission (WO-08): per-slot metadata, templates and a bounded GPU
// queue. Slot metadata is one uvec2: x = event tag (pad2 of the request),
// y = birth id — a slot-independent genealogy hash. CPU spawns take
// uFrameSeed ^ (j*phi); event children take parentBirth ^ (k*phi). Event seeds
// derive from the PARENT'S BIRTH ID, never the slot — slot identity permutes
// under free-stack recycling, which would otherwise make grandchildren values
// nondeterministic across runs/backends. (Merged into one buffer: NVIDIA GL
// exposes only 16 compute SSBO blocks, and this shader is at the limit.)
layout(binding = 20, std430) buffer BufSlotMeta { uvec2 slotMeta[]; };
layout(binding = 21, std430) readonly buffer BufEventTemplates { SpawnRequest eventTemplates[]; };
struct GpuEvent {          // std430, 48 B
    vec3 pos;
    uint templateIdx;
    vec3 vel;
    uint parentBirth;      // parent's birth id (slot-independent, see BufBirthIds)
    uint base;             // filled by phase 4 (0xFFFFFFFF = budget exhausted)
    uint pad0;
    uint pad1;
    uint pad2;
};
layout(binding = 22, std430) coherent buffer BufEventQueue {
    uint eventHead;
    uint eventTotalChildren;
    uint eventPad0;
    uint eventPad1;
    GpuEvent events[];
};
const uint kMaxEventInstances = 1024u;
const uint kMaxEventChildren = 4096u;
layout(binding = 9, std430) readonly  buffer BufPalette { vec4 palette[]; };
layout(binding = 10, std430) buffer BufIndirectArgs { // uPhase=2 write (draw count)
    uint vertexCount;    // always 4 (quad)
    uint instanceCount;  // alive
    uint firstVertex;    // 0
    uint baseInstance;   // 0
} indirectArgs;
layout(binding = 3, std430) coherent  buffer BufDead  { uint dead[]; };
layout(binding = 4, std430) coherent  buffer BufCtr   {
    uint uAlive;
    uint uDeadHead;
    uint uSpawnRequestCount;
    uint uCapacity;
    uint uAllocated; // stable occupied-slot extent, changed only during spawning
    uint uSpawnReuse; // phase 3 -> phase 1: recycled portion of this batch
    uint uSpawnAppendBase;
    uint uSpawnAccepted;
};
layout(binding = 11, std430) readonly buffer BufLive { uint liveIndices[]; };
layout(binding = 12, std430) writeonly buffer BufNextLive { uint nextLiveIndices[]; };
layout(binding = 5, std430) readonly buffer BufAttr { vec4 attractors[]; };

struct Vortex {
    vec4 center;        // xyz position, w = radius (falloff scale)
    vec4 axisStrength;  // xyz axis, w = strength
};
layout(binding = 6, std430) readonly buffer BufVortex { Vortex vortexes[]; };

struct Spring {
    vec3  anchor;
    float stiffness;
    float damping;
};
layout(binding = 7, std430) readonly buffer BufSpring { Spring springs[]; };

// All scalar state lives in one std140 block (no loose uniforms: the same
// source must compile to SPIR-V for Vulkan). CPU mirror: SimParams in
// src/backends/opengl/params.hpp. Force switches: bit i corresponds to
// ember::Force enum value i (0 gravity .. 9 boundary).
layout(binding = 14, std140) uniform SimParams {
    vec3  uGravity;          float uDrag;
    vec3  uWind;             float uTurbulence;
    vec3  uNoiseWindDir;     float uNoiseWindAmp;
    float uNoiseWindScale;   float uNoiseWindSpeed;
    float uWaveAmp;          float uWaveOmega;
    vec3  uWaveDir;          float uDt;
    vec3  uWaveK;            float uTime;
    uint  uForceMask;        int   uDragMode;      // 0 = linear, 1 = quadratic
    int   uAttractorCount;   int   uVortexCount;
    int   uSpringCount;      int   uBoundaryMode;  // 1 = kill, 2 = bounce (mask bit 9)
    float uBoundaryY;        float uRestitution;
    uint  uSpawnTotal;       uint  uFrameSeed;     // phase-1 request total / RNG seed
    uint  uInputAlive;       int   uGpuDriven;     // population at update start / mode
    int   uPhase;            uint  uEventTemplates; int uPad1; int uPad2;
};
layout(binding=13,std430) readonly buffer Schedule { uint scheduleWords[]; };

// Deterministic hash RNG (splitmix-ish) — per-particle state derived from the
// work id + frame seed, so concurrent spawn threads never share mutable state.
uint hashUint(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float rngUnit(inout uint s) { s = hashUint(s + 0x9E3779B9u); return float(s) / 4294967296.0; }
float rngRange(inout uint s, float a, float b) { return a + (b - a) * rngUnit(s); }
vec3 rngSphere(inout uint s) {
    float z = rngRange(s, -1.0, 1.0);
    float a = rngRange(s, 0.0, 6.283185307179586);
    float r = sqrt(max(0.0, 1.0 - z * z));
    return vec3(r * cos(a), r * sin(a), z);
}
vec3 rngVec3(inout uint s, vec3 a, vec3 b) {
    return vec3(rngRange(s, a.x, b.x), rngRange(s, a.y, b.y), rngRange(s, a.z, b.z));
}
void basis(vec3 ax, out vec3 u, out vec3 v) {
    u = normalize(cross(ax, abs(ax.y) > 0.9 ? vec3(1, 0, 0) : vec3(0, 1, 0)));
    v = cross(ax, u);
}

vec3 samplePosition(inout uint s, SpawnRequest r) {
    if (r.shape == 1) { // box
        return r.position + rngVec3(s, -r.extents, r.extents);
    }
    if (r.shape == 2) { // sphere (uniform radius, matching the CPU sampler)
        return r.position + rngSphere(s) * rngRange(s, 0.0, r.radius);
    }
    if (r.shape == 3) { // cone base disc
        vec3 ax = length(r.axis) > 1e-4 ? normalize(r.axis) : vec3(0, 1, 0);
        vec3 u, v; basis(ax, u, v);
        float rad = r.radius * sqrt(rngUnit(s));
        float a = rngRange(s, 0.0, 6.283185307179586);
        return r.position + (u * cos(a) + v * sin(a)) * rad;
    }
    return r.position; // point
}

vec3 sampleDirection(inout uint s, SpawnRequest r) {
    if (r.coneAngle > 0.0 && length(r.axis) > 1e-4) {
        vec3 ax = normalize(r.axis);
        vec3 u, v; basis(ax, u, v);
        float cosA = cos(radians(r.coneAngle));
        float cosT = mix(cosA, 1.0, rngUnit(s)); // cos-weighted on the cap
        float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
        float phi = rngRange(s, 0.0, 6.283185307179586);
        return normalize(ax * cosT + (u * cos(phi) + v * sin(phi)) * sinT);
    }
    float bl = length(r.dir);
    if (bl < 1e-4) return rngSphere(s);
    if (r.spread <= 0.01) return normalize(r.dir);
    return normalize(mix(normalize(r.dir), rngSphere(s), r.spread));
}

// ---- shared sampling (mirrors the CPU Emitter::spawn formulas) ----
Particle sampleParticle(SpawnRequest r, uint s) {
    Particle p;
    float size = rngRange(s, r.sizeMin, r.sizeMax);
    p.pos = vec4(samplePosition(s, r), (r.flags & 2) != 0 ? -size : size);
    vec3 dirv = sampleDirection(s, r);
    float speed = rngRange(s, r.speedMin, r.speedMax);
    if (r.speedSizeLink > 0.0) {
        float sizeMid = 0.5 * (r.sizeMin + r.sizeMax);
        if (sizeMid > 0.0) speed *= 1.0 + r.speedSizeLink * (size / sizeMid - 1.0);
    }
    p.vel = vec4(dirv * speed, 0.0); // age starts at 0
    p.life.x = rngRange(s, r.lifeMin, r.lifeMax);
    if (r.paletteIdx >= 0 && r.paletteCount > 0) {
        int ci = r.paletteIdx + int(rngUnit(s) * float(r.paletteCount)) % r.paletteCount;
        p.color = palette[ci];
    } else {
        p.color = vec4(rngRange(s, r.colorMin.r, r.colorMax.r),
                       rngRange(s, r.colorMin.g, r.colorMax.g),
                       rngRange(s, r.colorMin.b, r.colorMax.b),
                       rngRange(s, r.colorMin.a, r.colorMax.a));
    }
    // Fade target color (RGB) goes into life.yzw; unset => identity (no shift).
    if ((r.flags & 1) != 0) p.life.yzw = rngVec3(s, r.fadeMin, r.fadeMax);
    else p.life.yzw = p.color.rgb;
    return p;
}

// Phase 3 reserves a disjoint slot range once for the whole dispatch. No CAS
// retries or per-particle global allocator atomics in the spawn hot path.
uint spawnSlot(uint j) {
    return j < uSpawnReuse ? dead[uDeadHead + uSpawnReuse - 1u - j]
                           : uSpawnAppendBase + j - uSpawnReuse;
}
void writeSpawned(uint j, uint slot, Particle p, uint tag, uint birth) {
    slotMeta[slot] = uvec2(tag, birth); // persisted for onDeath/onBounce (chaining included)
    nxt[slot] = p;
    nextLiveIndices[indirectArgs.instanceCount + j] = slot;
}

void spawnFromRequest(uint j) {
    if (j >= uSpawnAccepted) return;
    // Requests are sorted by ascending base — binary search the owner.
    int lo = 0, hi = int(uSpawnRequestCount) - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (req[mid].base <= j) lo = mid;
        else hi = mid - 1;
    }
    SpawnRequest r = req[lo];
    uint birth = uFrameSeed ^ (j * 0x9E3779B9u);
    writeSpawned(j, spawnSlot(j), sampleParticle(r, birth), r.pad2, birth);
}

// Events triggered during phase 0 are appended to a bounded queue; phase 4
// computes the child slot prefix and phase 1 samples the children.
void enqueueEvent(vec3 pos, vec3 vel, uint templateIdx, uint parentBirth) {
    uint q = atomicAdd(eventHead, 1u);
    if (q < kMaxEventInstances) {
        events[q].pos = pos;
        events[q].vel = vel;
        events[q].templateIdx = templateIdx;
        events[q].parentBirth = parentBirth;
    }
}

void spawnFromEvent(uint j) {
    if (j >= uSpawnAccepted || uEventTemplates == 0u) return;
    uint e = j - uSpawnTotal;
    uint head = min(eventHead, kMaxEventInstances);
    if (head == 0u) return;
    // Instance table: base is non-decreasing; exhausted entries are sentinels.
    int lo = 0, hi = int(head) - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (events[mid].base <= e) lo = mid;
        else hi = mid - 1;
    }
    GpuEvent inst = events[lo];
    if (inst.base == 0xFFFFFFFFu) return;
    SpawnRequest r = eventTemplates[inst.templateIdx];
    uint k = e - inst.base;
    if (k >= r.count) return;
    r.position = inst.pos; // children are born at the parent's death point
    uint birth = inst.parentBirth ^ (k * 0x9E3779B9u);
    Particle p = sampleParticle(r, birth);
    p.vel.xyz += inst.vel * uintBitsToFloat(r.pad3);
    writeSpawned(j, spawnSlot(j), p, r.pad2, birth);
}

float hash11(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

// Smooth 3D value noise (lattice interpolation) — drifting gust-like turbulence.
// (name avoids the built-in noise3() family from GLSL)
float snoise3(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash11(i);
    float n100 = hash11(i + vec3(1, 0, 0));
    float n010 = hash11(i + vec3(0, 1, 0));
    float n110 = hash11(i + vec3(1, 1, 0));
    float n001 = hash11(i + vec3(0, 0, 1));
    float n101 = hash11(i + vec3(1, 0, 1));
    float n011 = hash11(i + vec3(0, 1, 1));
    float n111 = hash11(i + vec3(1, 1, 1));
    return mix(
        mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
        mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y),
        f.z);
}

// Kill a particle: queue an onDeath event, recycle its slot, shrink the alive
// count, leave a corpse marker. `emitDeath` is false for the NaN guard (which
// must not produce events); lifetime and boundary kills pass true.
void retire(uint i, bool emitDeath) {
    if (emitDeath && uEventTemplates != 0u) {
        const uvec2 meta = slotMeta[i];
        if ((meta.x & 0xFFFFu) != 0u && (meta.x & 0xFFFFu) - 1u < uEventTemplates) {
            Particle p = cur[i];
            enqueueEvent(p.pos.xyz, p.vel.xyz, (meta.x & 0xFFFFu) - 1u, meta.y);
        }
    }
    uint h = atomicAdd(uDeadHead, 1u);
    dead[h % uCapacity] = i;
    atomicAdd(uAlive, -1u);
    nxt[i] = Particle(vec4(0.0), vec4(0.0), vec4(-1.0), vec4(0.0));
    // Keep both buffers' tombstones valid for slot-order readback and for
    // switching to a legacy custom shader that scans the allocated extent.
    cur[i] = nxt[i];
}

bool simulate(uint i) {
    Particle p = cur[i];
    if (p.life.x < 0.0) return false;
    // NaN guard: a NaN particle is retired instead of poisoning the buffer
    // (NaN positions can also stall some drivers during rasterization).
    if (isnan(p.pos.x) || isnan(p.pos.y) || isnan(p.pos.z) ||
        isnan(p.vel.x) || isnan(p.vel.y) || isnan(p.vel.z)) {
        retire(i, false);
        return false;
    }
    p.vel.w += uDt;                      // age
    if (p.vel.w >= p.life.x) {
        retire(i, true);
        return false;
    }

    // ---- force accumulation (each term guarded by uForceMask) -------------
    vec3 acc = vec3(0.0);
    if ((uForceMask & (1u << 0u)) != 0u) acc += uGravity;                 // gravity
    if ((uForceMask & (1u << 1u)) != 0u) {                                // drag
        if (uDragMode == 1) acc -= p.vel.xyz * length(p.vel.xyz) * uDrag; // quadratic
        else acc -= p.vel.xyz * uDrag;                                    // linear
    }
    if ((uForceMask & (1u << 2u)) != 0u) acc += uWind;                    // uniform wind
    if ((uForceMask & (1u << 3u)) != 0u && uTurbulence > 0.0) {           // turbulence
        vec3 n = vec3(snoise3(p.pos.xyz * 0.3 + uTime),
                      snoise3(p.pos.xyz * 0.3 + uTime * 1.31 + 7.7),
                      snoise3(p.pos.xyz * 0.3 + uTime * 0.67 + 3.3));
        acc += (n * 2.0 - 1.0) * uTurbulence;
    }
    if ((uForceMask & (1u << 4u)) != 0u) {                                // point attractors
        for (int k = 0; k < uAttractorCount; ++k) {
            vec3 d = attractors[k].xyz - p.pos.xyz;
            float r2 = dot(d, d) + 1e-4;     // softening
            float inv = inversesqrt(r2);
            acc += d * (attractors[k].w * inv * inv * inv); // inverse-square, sign = attract/repel
        }
    }
    if ((uForceMask & (1u << 5u)) != 0u && uVortexCount > 0) {            // vortex swirls
        for (int k = 0; k < uVortexCount; ++k) {
            Vortex v = vortexes[k];
            vec3 d = v.center.xyz - p.pos.xyz;
            float r2 = dot(d, d) + 1e-4;
            float fall = v.axisStrength.w / (r2 + v.center.w * v.center.w); // smooth core, ~1/r² outside
            acc += cross(v.axisStrength.xyz, d) * fall;                     // tangential swirl
        }
    }
    if ((uForceMask & (1u << 6u)) != 0u && uSpringCount > 0) {            // spring-to-anchor
        for (int k = 0; k < uSpringCount; ++k) {
            Spring s = springs[k];
            vec3 d = s.anchor - p.pos.xyz;
            acc += d * s.stiffness - p.vel.xyz * s.damping;
        }
    }
    if ((uForceMask & (1u << 7u)) != 0u && uNoiseWindAmp != 0.0) {        // signed spatial noise wind
        float n = snoise3(p.pos.xyz * uNoiseWindScale + uTime * uNoiseWindSpeed);
        acc += uNoiseWindDir * (n * 2.0 - 1.0) * uNoiseWindAmp;
    }
    if ((uForceMask & (1u << 8u)) != 0u && uWaveAmp != 0.0) {             // signed traveling wave
        float phase = dot(p.pos.xyz, uWaveK) + uTime * uWaveOmega;
        acc += uWaveDir * (sin(phase) * uWaveAmp);
    }

    // Semi-implicit Euler integration.
    p.vel.xyz += acc * uDt;
    p.vel.xyz = clamp(p.vel.xyz, vec3(-500.0), vec3(500.0)); // guard BEFORE displacement
    p.pos.xyz += p.vel.xyz * uDt;

    // ---- boundary plane (y < uBoundaryY) -----------------------------------
    if ((uForceMask & (1u << 9u)) != 0u && uBoundaryMode != 0 && p.pos.y < uBoundaryY) {
        if (uBoundaryMode == 1) {          // kill
            retire(i, true);
            return false;
        }
        p.pos.y = uBoundaryY;              // bounce
        if (p.vel.y < 0.0) p.vel.y = -p.vel.y * uRestitution;
        if (abs(p.vel.y) < 0.1) p.vel.y = 0.0; // settle on the plane
        if (uEventTemplates != 0u) {       // onBounce: every contact triggers
            const uvec2 meta = slotMeta[i];
            const uint onBounce = meta.x >> 16;
            if (onBounce != 0u && onBounce - 1u < uEventTemplates)
                enqueueEvent(p.pos.xyz, p.vel.xyz, onBounce - 1u, meta.y);
        }
    }
    nxt[i] = p;
    return true;
}

void main() {
    if (uPhase == 0) {
        uint j = gl_GlobalInvocationID.x;
        if (j >= (uGpuDriven != 0 ? scheduleWords[0] : uInputAlive)) return;
        uint i = liveIndices[j];
        if (simulate(i)) {
            uint drawIndex = atomicAdd(indirectArgs.instanceCount, 1u);
            nextLiveIndices[drawIndex] = i;
        }
    } else if (uPhase == 4) {
        // Serial prefix sum over this frame's event instances (<= 1024 adds);
        // exhausting the child budget marks later instances as sentinels.
        if (gl_GlobalInvocationID.x == 0u) {
            uint head = min(eventHead, kMaxEventInstances);
            uint running = 0u;
            for (uint q = 0u; q < head; ++q) {
                if (running >= kMaxEventChildren) {
                    events[q].base = 0xFFFFFFFFu;
                    continue;
                }
                events[q].base = running;
                running = min(running + eventTemplates[events[q].templateIdx].count,
                              kMaxEventChildren);
            }
            eventTotalChildren = running;
        }
    } else if (uPhase == 3 && gl_GlobalInvocationID.x == 0u) {
        // Integration has finished and no producer/consumer overlaps this
        // reservation. Recycle first, then append; bounded without atomics.
        // Event children are part of the same batch and share the accounting.
        uint batchTotal = uSpawnTotal + eventTotalChildren;
        uSpawnReuse = min(batchTotal, uDeadHead);
        uSpawnAppendBase = uAllocated;
        uint appendCount = min(batchTotal - uSpawnReuse, uCapacity - uAllocated);
        uSpawnAccepted = uSpawnReuse + appendCount;
        uDeadHead -= uSpawnReuse;
        uAllocated += appendCount;
        uAlive += uSpawnAccepted;
    } else if (uPhase == 1) {
        uint j = gl_GlobalInvocationID.x;
        if (j < uSpawnTotal) spawnFromRequest(j);
        else spawnFromEvent(j);
    } else if (uPhase == 2) {
        if (gl_GlobalInvocationID.x == 0u) indirectArgs.instanceCount = uAlive;
    }
}
)GLSL";

const char* const kParticleVert = R"GLSL(#version 430 core
// gl_VertexID/gl_InstanceID are gl_VertexIndex/gl_InstanceIndex in Vulkan GLSL
// (same base-offset semantics here: firstVertex/baseInstance are always 0).
#if defined(VULKAN)
#define EMBER_VERTEX_INDEX gl_VertexIndex
#define EMBER_INSTANCE_INDEX gl_InstanceIndex
#else
#define EMBER_VERTEX_INDEX gl_VertexID
#define EMBER_INSTANCE_INDEX gl_InstanceID
#endif

// Instanced billboard quad: 4 vertices per particle (GL_TRIANGLE_STRIP),
// the quad is expanded in view space from gl_VertexID — no vertex attributes.

// Vulkan descriptor sets cannot alias one binding to different descriptor
// types per stage (GL's per-stage namespaces allow that), so the render
// resources move to a dedicated set with non-colliding binding numbers.
#if defined(EMBER_CLIP_VULKAN)
#define EMBER_RENDER_SET set = 0,
#define EMBER_BIND_CUR 0
#define EMBER_BIND_SORTED 1
#define EMBER_BIND_LIVE 2
#define EMBER_BIND_DRAW_PARAMS 6
#define EMBER_BIND_CURVES 8
#else
#define EMBER_RENDER_SET
#define EMBER_BIND_CUR 0
#define EMBER_BIND_SORTED 8
#define EMBER_BIND_LIVE 11
#define EMBER_BIND_DRAW_PARAMS 17
#define EMBER_BIND_CURVES 23
#endif

struct Particle {
    vec4 pos;
    vec4 vel;
    vec4 life;
    vec4 color;
};
layout(EMBER_RENDER_SET binding = EMBER_BIND_CUR, std430) readonly buffer BufCur    { Particle particles[]; };
layout(EMBER_RENDER_SET binding = EMBER_BIND_SORTED, std430) readonly buffer BufSorted { uint sorted[]; };

layout(EMBER_RENDER_SET binding = EMBER_BIND_LIVE, std430) readonly buffer BufLive { uint liveIndices[]; };

// Baked lifecycle curves (WO-09): color-over-life (rgba) and size-over-life.
// CPU mirror: CurvesParams in src/backends/opengl/params.hpp.
layout(EMBER_RENDER_SET binding = EMBER_BIND_CURVES, std430) readonly buffer BufCurves {
    vec4  colorLut[64];
    float sizeLut[64];
};

// Scalar state in one std140 block (Vulkan-compilable; CPU mirror: DrawParams
// in src/backends/opengl/params.hpp).
layout(EMBER_RENDER_SET binding = EMBER_BIND_DRAW_PARAMS, std140) uniform DrawParams {
    mat4 uView; mat4 uProj;
    float uSizeScale;
    float uStreak;    // >0: centered stretch along projected particle motion
    float uSpinSpeed; // signed angular velocity; zero disables normal-particle spin
    int   uUseSorted; // 1 = fetch particle via sorted[gl_InstanceID] (GPU sort)
    int   uRefraction;// 0 = normal pass, 1 = refraction pass
};

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out float vFade;
layout(location = 3) out vec3 vFadeRGB;
layout(location = 4) out float vViewZ;         // view-space z, for soft particles in the FS
layout(location = 5) out vec3 vFacetNormal;    // view-space pseudo-normal (refraction offset direction)
layout(location = 6) out float vAngle;         // billboard rotation (rad): dome normals rotate with the shard
layout(location = 7) flat out uint vShardSeed; // per-particle hash: polygon shard silhouette in the FS

uint hashUint(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float hashAngle(uint id) { return float(hashUint(id)) / 4294967296.0 * 6.28318530718; }
vec2 hashTilt(uint id) {
    uint a = hashUint(id);
    uint b = hashUint(id ^ 0x9E3779B9u);
    return vec2(float(a) / 4294967296.0 - 0.5, float(b) / 4294967296.0 - 0.5);
}

void main() {
#if defined(EMBER_DEBUG_FULLSCREEN)
    const vec2 dc = vec2(float(EMBER_VERTEX_INDEX & 1) * 2.0 - 1.0,
                         float((EMBER_VERTEX_INDEX >> 1) & 1) * 2.0 - 1.0);
    gl_Position = vec4(dc, 0.0, 1.0);
    vUV = dc * 0.5 + 0.5;
    vColor = vec4(1.0); vFade = 1.0; vFadeRGB = vec3(1.0);
    vViewZ = 0.0; vFacetNormal = vec3(0.0, 0.0, 1.0); vAngle = 0.0; vShardSeed = 0u;
    return;
#endif
    const uint idx = (uUseSorted != 0) ? sorted[EMBER_INSTANCE_INDEX] : liveIndices[EMBER_INSTANCE_INDEX];
    const Particle p = particles[idx];

    // Assign outputs unconditionally (strict drivers reject varyings that are
    // only written on some paths).
    // Lifecycle curves: multiply color and size by the baked LUT. A disabled
    // curve is all-ones, so x * 1.0 is bit-identical to the plain path.
    const float ageFrac = clamp(p.vel.w / max(p.life.x, 1e-4), 0.0, 1.0);
    const uint  lutIdx = min(uint(ageFrac * 63.0 + 0.5), 63u);
    const vec4  colorMul = colorLut[lutIdx];
    const float sizeMul = sizeLut[lutIdx];
    vColor = p.color * colorMul;
    vFade = 1.0 - ageFrac;
    vFadeRGB = p.life.yzw * colorMul.rgb;
    vShardSeed = hashUint(idx);
    vAngle = 0.0;

    // Refractive particles are sign-encoded in pos.w (size < 0). Cull instances
    // that do not belong to the current pass (uRefraction): a degenerate quad.
    const bool isRefractive = p.pos.w < 0.0;
    if (isRefractive != (uRefraction != 0)) {
        gl_Position = vec4(0.0, 0.0, 3.0, 1.0);
        vUV = vec2(0.0);
        vViewZ = 0.0;
        vFacetNormal = vec3(0.0, 0.0, 1.0);
        return;
    }
    const float size = abs(p.pos.w) * sizeMul;

    // Default orientation. A visible streak overrides it below; a vanishing
    // streak returns continuously to this angle instead of snapping at rest.
    float ang = 0.0;
    if (uSpinSpeed != 0.0 || uRefraction != 0)
        ang = hashAngle(idx) + p.vel.w * uSpinSpeed;

    const vec3 viewPos = (uView * vec4(p.pos.xyz, 1.0)).xyz;
    // Clipping belongs to uProj; a fixed view-Z cutoff breaks close near
    // planes and valid orthographic ranges that straddle the view origin.
    vViewZ = viewPos.z;

    const float hs = 0.5 * size * uSizeScale;
    // Quad corner in [-1,1]^2, triangle-strip order.
    const vec2 c = vec2(float(EMBER_VERTEX_INDEX & 1) * 2.0 - 1.0,
                        float((EMBER_VERTEX_INDEX >> 1) & 1) * 2.0 - 1.0);
    vUV = c * 0.5 + 0.5;

    float elong = 1.0;
    if (uStreak > 0.0) {
        // Differentiate the perspective projection at the particle center,
        // then express that motion in view-plane units at its current depth.
        // Dropping velocity.z is only correct for orthographic projection:
        // off-axis depth motion moves on screen; camera-ray motion does not.
        const vec3 vv = (uView * vec4(p.vel.xyz, 0.0)).xyz;
        vec2 motion = vv.xy;
        if (uProj[3][3] == 0.0 && abs(viewPos.z) > 1e-6)
            motion -= viewPos.xy * (vv.z / viewPos.z);
        const float speed = length(motion);
        const float extra = uStreak * speed;
        elong += extra;
        if (speed > 1e-6) {
            const float target = atan(motion.y, motion.x);
            // Up to 10% elongation, blend the shortest angular path from the
            // ordinary billboard orientation. Above it, velocity fully wins.
            const float weight = smoothstep(0.0, 0.1, extra);
            if (weight >= 1.0) ang = target;
            else ang += atan(sin(target - ang), cos(target - ang)) * weight;
        }
    }
    vAngle = ang;
    const float ca = cos(ang), sa = sin(ang);
    const vec2 offset = vec2(c.x * elong * ca - c.y * sa,
                             c.x * elong * sa + c.y * ca) * hs;

    // Geometry, dome coordinates and facet normal share one final orientation.
    if (uRefraction != 0) {
        const vec2 tilt = hashTilt(idx) * 1.2;
        const vec3 n = normalize(vec3(tilt, 1.0));
        vFacetNormal = vec3(n.x * ca - n.y * sa, n.x * sa + n.y * ca, n.z);
    } else {
        vFacetNormal = vec3(0.0, 0.0, 1.0);
    }
    gl_Position = uProj * vec4(viewPos + vec3(offset, 0.0), 1.0);
#if defined(EMBER_CLIP_VULKAN)
    // The host keeps a GL-style projection (NDC z in [-1,1]); Vulkan's clip
    // volume is z in [0,w]. Remap here and read depth back accordingly in the
    // fragment shader (see EMBER_DEPTH_TO_NDC).
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
#endif
}
)GLSL";

const char* const kParticleFrag = R"GLSL(#version 430 core
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in float vFade;
layout(location = 3) in vec3 vFadeRGB;
layout(location = 4) in float vViewZ;
layout(location = 5) in vec3 vFacetNormal;
layout(location = 6) in float vAngle;         // billboard rotation (rad), matches the quad orientation
layout(location = 7) flat in uint vShardSeed; // per-particle hash: polygon shard silhouette
layout(location = 0) out vec4 frag;

// Depth convention: GL clip space maps NDC z in [-1,1]; Vulkan uses [0,1].
// The host depth texture and gl_FragCoord.z follow the active convention, so
// both reconstruction paths stay correct by picking one conversion helper.
#if defined(EMBER_CLIP_VULKAN)
#define EMBER_DEPTH_TO_NDC(store) (store)
#else
#define EMBER_DEPTH_TO_NDC(store) ((store) * 2.0 - 1.0)
#endif

// Samplers carry explicit bindings (required for SPIR-V; GL 4.2+ honors
// them, so the host no longer needs glUniform1i for texture units).
// Vulkan moves render resources to a dedicated set (see particle.vert); GL
// keeps the legacy per-stage bindings.
#if defined(EMBER_CLIP_VULKAN)
#define EMBER_RENDER_SET set = 0,
#define EMBER_BIND_SPRITE 3
#define EMBER_BIND_DEPTH 4
#define EMBER_BIND_COLOR 5
#define EMBER_BIND_FRAG_PARAMS 7
#else
#define EMBER_RENDER_SET
#define EMBER_BIND_SPRITE 0
#define EMBER_BIND_DEPTH 1
#define EMBER_BIND_COLOR 2
#define EMBER_BIND_FRAG_PARAMS 18
#endif
layout(EMBER_RENDER_SET binding = EMBER_BIND_SPRITE) uniform sampler2D uSprite;
layout(EMBER_RENDER_SET binding = EMBER_BIND_DEPTH) uniform sampler2D uSceneDepth;
layout(EMBER_RENDER_SET binding = EMBER_BIND_COLOR) uniform sampler2D uSceneColor;
// Scalar state in one std140 block (CPU mirror: FragParams in
// src/backends/opengl/params.hpp).
layout(EMBER_RENDER_SET binding = EMBER_BIND_FRAG_PARAMS, std140) uniform FragParams {
    mat4 uInvProj; mat4 uFragProj; // renamed: uProj belongs to DrawParams (VS)
    vec2 uViewportSize; float uSoftRadius; int uUseSprite; // viewport px / soft fade / texture path
    int uUseSoft;      int uSheetCols;   int uSheetRows;   int uFragRefraction;
    int uRefrMode;     int uRefrShape;   float uRefrDome;  float uRefrStrength;
    float uRefrIor;    float uRefrAbsorption; float uRefrFresnel; float uRefrChroma;
    vec3 uRefrTint;    float uRefrSpecular;
    vec3 uRefrLightDir; float uRefrPad;   // glint light direction (view space)
};

float refrHash(vec2 p) {
    p = fract(p * 0.1031);
    return fract((p.x + p.y) * (p.x + p.y + 33.33) + p.y * p.x);
}
float refrNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(refrHash(i), refrHash(i + vec2(1, 0)), f.x),
               mix(refrHash(i + vec2(0, 1)), refrHash(i + vec2(1, 1)), f.x), f.y);
}

// Irregular polygon shard silhouette: 3-5 sides from the particle seed, with
// per-edge radial jitter (fracture nicks at the vertices). Returns the
// boundary radius in quad-local units (circumradius 1) at angle theta.
float shardRadius(float theta, uint seed) {
    const float PI = 3.14159265359;
    const float TAU = 6.28318530718;
    const float sides = 3.0 + float(seed % 3u);          // triangle / quad / pentagon
    const float sector = TAU / sides;
    const float a = mod(theta, TAU);
    const float k = floor(a / sector);
    const float local = mod(a, sector) - sector * 0.5;   // [-sector/2, sector/2]
    const float edge = cos(PI / sides) / cos(local);     // regular N-gon boundary
    const float j = 0.62 + 0.38 * refrHash(vec2(float(seed % 8191u) * 0.013,
                                                k * 3.71 + float((seed >> 13) % 8191u) * 0.007));
    return edge * j;
}

void main() {
    vec2 uv = vUV;
    vec4 texel = vec4(1.0);
    const vec2 dc = vUV * 2.0 - 1.0;   // quad-local coords [-1,1]
    const float rq = length(dc);
    float shardR = 1.0;                // silhouette radius at this angle (polygon shards)
    if (uFragRefraction != 0 && uRefrShape == 1) {
        // Polygon shard silhouette replaces the sprite mask entirely.
        shardR = shardRadius(atan(dc.y, dc.x), vShardSeed);
        if (rq > shardR) discard;
    } else if (uUseSprite != 0) {
        if (uSheetCols > 1 || uSheetRows > 1) {
            // age-based frame animation (vFade = 1 - age/life)
            float t = 1.0 - vFade;
            int total = uSheetCols * uSheetRows;
            int frame = int(clamp(t, 0.0, 0.999) * float(total));
            const vec2 grid = vec2(uSheetCols, uSheetRows);
            const vec2 cell = vec2(float(frame % uSheetCols), float(frame / uSheetCols)) / grid;
            // Linear filtering must stay inside this cell, rather than blend
            // in neighboring animation frames along the billboard edge.
            const vec2 inset = min(0.5 / vec2(textureSize(uSprite, 0)), 0.5 / grid);
            uv = clamp(cell + vUV / grid, cell + inset, cell + 1.0 / grid - inset);
        }
        texel = texture(uSprite, uv);
        if (texel.a < 0.02) discard;
    } else {
        // procedural soft disc + halo
        if (rq > 1.0) discard;
        texel.a = (1.0 - smoothstep(0.0, 1.0, rq)) + (1.0 - smoothstep(0.0, 1.0, rq * rq)) * 0.5;
    }

    // ---- refractive particles (glass shards) --------------------------------
    // Sample the host's scene color texture with a per-particle facet offset,
    // composite in-shader and replace the pixel (blending is disabled by the
    // host for this pass). Shape comes from the sprite mask above.
    if (uFragRefraction != 0) {
        const float coverage = clamp(vFade * vColor.a * texel.a, 0.0, 1.0);
        if (coverage <= 0.0) discard;
        const vec2 screenUV = gl_FragCoord.xy / uViewportSize;
        const vec3 n = normalize(vFacetNormal);
        // Dome-perturbed normal (per-PIXEL): the shard is lit like a tiny curved
        // facet — fresnel becomes a silhouette rim and the glint shrinks to a
        // spot. domeR = 1 exactly at the silhouette (shape-aware for polygons).
        vec3 nF = n;
        if (uRefrDome > 0.0) {
            const float domeR = clamp(rq / shardR, 0.0, 1.0);
            const vec2 dir = dc / max(rq, 1e-4);
            const float ca = cos(vAngle), sa = sin(vAngle);
            const vec2 dirV = vec2(dir.x * ca - dir.y * sa, dir.x * sa + dir.y * ca);
            nF = normalize(vec3(n.xy + dirV * (domeR * uRefrDome), n.z));
        }
        vec2 off;
        if (uRefrMode == 2) {
            // heat shimmer: noise-driven offset animated over the particle age
            const vec2 t = vec2(vFade * 6.2831, vFade * 3.7);
            off = vec2(refrNoise(screenUV * 8.0 + t) - 0.5) * (2.0 * uRefrStrength);
        } else if (uRefrMode == 1) {
            // Bend the view ray by the supplied refractive index, intersect
            // the scene's depth plane, then project the displaced sample.
            // IOR=1 is the identity. Missing depth is handled by the host's
            // simple-mode fallback; a cleared depth texel uses a unit plane.
            const float sd = texture(uSceneDepth, screenUV).r;
            const vec4 clip = vec4(screenUV * 2.0 - 1.0, EMBER_DEPTH_TO_NDC(sd), 1.0);
            const vec4 sv = uInvProj * clip;
            const float sceneZ = sd < 1.0 && abs(sv.w) > 1e-6 ? sv.z / sv.w : vViewZ - 1.0;
            const vec4 pv = uInvProj * vec4(screenUV * 2.0 - 1.0, EMBER_DEPTH_TO_NDC(gl_FragCoord.z), 1.0);
            const vec3 origin = pv.xyz / pv.w;
            const vec3 incident = uFragProj[3][3] == 0.0 ? normalize(origin) : vec3(0, 0, -1);
            const vec3 ray = refract(incident, n, 1.0 / uRefrIor);
            off = vec2(0.0);
            if (ray.z < -1e-6 && sceneZ < vViewZ && uRefrIor != 1.0) {
                const vec3 hit = origin + ray * ((sceneZ - vViewZ) / ray.z);
                const vec4 projected = uFragProj * vec4(hit, 1.0);
                off = (projected.xy / projected.w * 0.5 + 0.5 - screenUV) * uRefrStrength;
            }
        } else {
            off = n.xy * uRefrStrength;
        }
        vec3 col;
        if (uRefrChroma > 0.0) {
            col.r = texture(uSceneColor, screenUV + off * (1.0 + uRefrChroma)).r;
            col.g = texture(uSceneColor, screenUV + off).g;
            col.b = texture(uSceneColor, screenUV + off * (1.0 - uRefrChroma)).b;
        } else {
            col = texture(uSceneColor, screenUV + off).rgb;
        }
        vec3 refr = col * uRefrTint;
        const vec3 original = texture(uSceneColor, screenUV).rgb;
        vec3 outc = mix(original, refr, uRefrAbsorption);
        // Surface reflections (fresnel rim + glint spot) are ADDED by the glass,
        // not transmitted — they must not be attenuated by uRefrAbsorption.
        if (uRefrFresnel > 0.0)
            outc += pow(1.0 - abs(dot(nF, vec3(0.0, 0.0, 1.0))), uRefrFresnel) * vec3(1.0);
        if (uRefrSpecular > 0.0) {
            const vec3 rv = reflect(vec3(0.0, 0.0, -1.0), nF);
            outc += pow(max(dot(rv, uRefrLightDir), 0.0), 24.0) * uRefrSpecular;
        }
        // Keep pixel replacement, but fade the distortion/reflections into
        // the original scene using lifetime, sprite coverage and emitter alpha.
        frag = vec4(mix(original, outc, coverage), 1.0);
        return;
    }

    // ---- normal (glow) path --------------------------------------------------
    // fade toward vFadeRGB as the particle dies
    vec3 rgb = mix(vFadeRGB, vColor.rgb, vFade);
    float a = vFade * texel.a * vColor.a;

    // Soft particles: fade out when the particle is close in front of a scene
    // surface. Sample the scene depth at the FRAGMENT's screen position — vUV
    // is the quad-local UV, which only matches screen space for tiny centered
    // quads and breaks for off-center / large / streaked particles.
    if (uUseSoft != 0) {
        const vec2 screenUV = gl_FragCoord.xy / uViewportSize;
        const float sd = texture(uSceneDepth, screenUV).r;
        if (sd < 1.0) {
            const vec4 clip = vec4(screenUV * 2.0 - 1.0, EMBER_DEPTH_TO_NDC(sd), 1.0);
            const vec4 sv = uInvProj * clip;
            const float sceneZ = sv.z / sv.w;      // view-space z of the surface
            a *= smoothstep(0.0, uSoftRadius, vViewZ - sceneZ);
        }
    }
    if (a <= 0.0) discard; // invisible fragments must not occlude later draws
    frag = vec4(rgb * texel.rgb, a);
}
)GLSL";

// GPU bitonic sort of alive particle indices (see shaders/sort.comp).
const char* const kSortComp = R"GLSL(#version 430 core
// Exact back-to-front bitonic sort with cached depth keys and 256-entry tiles.
// uMode=0 + uTileMode=1: fill and sort alternating tiles in shared memory.
// uMode=1 + uTileMode=0: one global compare-exchange step (uJ >= 256).
// uMode=1 + uTileMode=2: finish uJ=128..1 in shared memory for stage uK.
// All local invocations participate in barriers, including padding lanes.
// uGpuDriven=1 reads alive from binding 4 and padded N from binding 13 word 1.
// schedule.comp provides indirect commands; inactive merge stages have X=0.
layout(local_size_x = 256) in;

struct Particle { vec4 pos; vec4 vel; vec4 life; vec4 color; };
layout(binding = 0, std430) readonly buffer BufCur { Particle cur[]; };
layout(binding = 8, std430) buffer BufSorted { uint sorted[]; };
layout(binding = 11, std430) readonly buffer BufLive { uint liveIndices[]; };
// Binding 12 is scratch: next live indices during sim, depth keys during sort.
layout(binding = 12, std430) buffer BufKeys { float keys[]; };
// Scalar state in one std140 block (Vulkan-compilable; CPU mirror: SortParams
// in src/backends/opengl/params.hpp).
layout(binding = 15, std140) uniform SortParams {
    mat4 uView;
    uint uCapacity; uint uAlive; uint uPaddedN; uint uK;
    uint uJ; uint uMode; uint uTileMode; int uGpuDriven;
};
layout(binding=4,std430) readonly buffer Counters { uint gpuAlive; };
layout(binding=13,std430) readonly buffer Schedule { uint scheduleWords[]; };
shared uint tileIndices[256];
shared float tileKeys[256];

bool greaterKey(float a, uint ai, float b, uint bi) {
    return a > b || (a == b && ai > bi); // reproducible tie-break by stable slot
}
float depthKey(uint idx) {
    if (idx >= uCapacity) return uintBitsToFloat(0x7f800000u);
    float z = (uView * vec4(cur[idx].pos.xyz, 1.0)).z;
    return isnan(z) ? uintBitsToFloat(0x7f800000u) : z;
}
void tileStep(uint lane, uint globalIndex, uint k, uint j) {
    uint other = lane ^ j;
    if (lane < other) {
        float a = tileKeys[lane], b = tileKeys[other];
        uint ai = tileIndices[lane], bi = tileIndices[other];
        bool ascending = (globalIndex & k) == 0u;
        if (ascending ? greaterKey(a, ai, b, bi) : greaterKey(b, bi, a, ai)) {
            tileKeys[lane] = b; tileKeys[other] = a;
            tileIndices[lane] = bi; tileIndices[other] = ai;
        }
    }
    barrier();
}
void main() {
    uint count = uGpuDriven != 0 ? gpuAlive : uAlive;
    uint paddedN = uGpuDriven != 0 ? scheduleWords[1] : uPaddedN;
    uint i = gl_GlobalInvocationID.x;
    uint lane = gl_LocalInvocationID.x;
    if (uTileMode == 0u) {
        uint other = i ^ uJ;
        if (i >= paddedN || other <= i) return;
        float a = keys[i], b = keys[other];
        uint ai = sorted[i], bi = sorted[other];
        bool ascending = (i & uK) == 0u;
        if (ascending ? greaterKey(a, ai, b, bi) : greaterKey(b, bi, a, ai)) {
            keys[i] = b; keys[other] = a;
            sorted[i] = bi; sorted[other] = ai;
        }
        return;
    }
    uint idx = uCapacity;
    float key = uintBitsToFloat(0x7f800000u);
    if (uMode == 0u) {
        idx = i < count ? liveIndices[i] : uCapacity;
        key = depthKey(idx);
    } else if (i < paddedN) {
        idx = sorted[i]; key = keys[i];
    }
    tileIndices[lane] = idx;
    tileKeys[lane] = key;
    barrier();
    if (uMode == 0u) {
        for (uint k = 2u; k <= 256u; k <<= 1u)
            for (uint j = k >> 1u; j > 0u; j >>= 1u) tileStep(lane, i, k, j);
    } else {
        for (uint j = 128u; j > 0u; j >>= 1u) tileStep(lane, i, uK, j);
    }
    if (i < paddedN) {
        sorted[i] = tileIndices[lane];
        keys[i] = tileKeys[lane];
    }
}
)GLSL";


}
