#version 430 core
// Instanced billboard quad: 4 vertices per particle (GL_TRIANGLE_STRIP),
// the quad is expanded in view space from gl_VertexID — no vertex attributes.

struct Particle {
    vec4 pos;
    vec4 vel;
    vec4 life;
    vec4 color;
};
layout(binding = 0, std430) readonly buffer BufCur    { Particle particles[]; };
layout(binding = 8, std430) readonly buffer BufSorted { uint sorted[]; };

uniform mat4  uView;
uniform mat4  uProj;
uniform float uSizeScale;
uniform float uStreak;    // >0: stretch quads along lateral velocity (spark trails)
uniform int   uUseSorted; // 1 = fetch particle via sorted[gl_InstanceID] (GPU sort)

out vec2 vUV;
out vec4 vColor;
out float vFade;
out vec3 vFadeRGB;
out float vViewZ;         // view-space z, for soft particles in the FS

void main() {
    const uint idx = (uUseSorted != 0) ? sorted[gl_InstanceID] : gl_InstanceID;
    const Particle p = particles[idx];

    // Assign outputs unconditionally (strict drivers reject varyings that are
    // only written on some paths).
    vColor = p.color;
    vFade = 1.0 - clamp(p.vel.w / max(p.life.x, 1e-4), 0.0, 1.0);
    vFadeRGB = p.life.yzw;

    const vec3 viewPos = (uView * vec4(p.pos.xyz, 1.0)).xyz;
    if (viewPos.z > -0.01) {                 // behind the near plane: clip out
        gl_Position = vec4(0.0, 0.0, 3.0, 1.0);
        vUV = vec2(0.0);
        vViewZ = 0.0;
        return;
    }
    vViewZ = viewPos.z;

    const float hs = 0.5 * p.pos.w * uSizeScale;
    // Quad corner in [-1,1]^2, triangle-strip order.
    const vec2 c = vec2(float(gl_VertexID & 1) * 2.0 - 1.0,
                        float((gl_VertexID >> 1) & 1) * 2.0 - 1.0);
    vUV = c * 0.5 + 0.5;

    vec2 offset;
    if (uStreak > 0.0) {
        // Stretch along the particle's velocity projected on the view plane.
        const vec3 vv = (uView * vec4(p.vel.xyz, 0.0)).xyz;
        const vec2 dir2 = normalize(vv.xy + vec2(1e-6));
        const vec2 perp = vec2(-dir2.y, dir2.x);
        const float elong = 1.0 + uStreak * length(vv.xy);
        offset = dir2 * (c.x * hs * elong) + perp * (c.y * hs);
    } else {
        offset = c * hs;
    }
    gl_Position = uProj * vec4(viewPos + vec3(offset, 0.0), 1.0);
}
