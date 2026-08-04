#version 430 core
in vec2 vUV;
in vec4 vColor;
in float vFade;
in vec3 vFadeRGB;
in float vViewZ;
out vec4 frag;

uniform sampler2D uSprite;
uniform sampler2D uSceneDepth;
uniform mat4  uInvViewProj;
uniform float uSoftRadius;
uniform vec2  uViewportSize; // screen size in pixels (soft-particle depth lookup)
uniform int   uUseSprite;  // 1 = texture path, 0 = procedural glow
uniform int   uUseSoft;    // 1 = soft particles (fade near scene depth)
uniform int   uSheetCols;  // sprite-sheet grid (1x1 = single frame)
uniform int   uSheetRows;

void main() {
    vec2 uv = vUV;
    vec4 texel = vec4(1.0);
    if (uUseSprite != 0) {
        if (uSheetCols > 1 || uSheetRows > 1) {
            // age-based frame animation (vFade = 1 - age/life)
            float t = 1.0 - vFade;
            int total = uSheetCols * uSheetRows;
            int frame = int(clamp(t, 0.0, 0.999) * float(total));
            uv = (vUV + vec2(float(frame % uSheetCols), float(frame / uSheetCols)))
                 / vec2(uSheetCols, uSheetRows);
        }
        texel = texture(uSprite, uv);
        if (texel.a < 0.02) discard;
    } else {
        // procedural soft disc + halo
        vec2 c = vUV * 2.0 - 1.0;
        float r = length(c);
        if (r > 1.0) discard;
        texel.a = smoothstep(1.0, 0.0, r) + smoothstep(1.0, 0.0, r * r) * 0.5;
    }
    // fade toward vFadeRGB as the particle dies
    vec3 rgb = mix(vFadeRGB, vColor.rgb, vFade);
    float a = vFade * texel.a * vColor.a;

    // Soft particles: fade out when the particle is close behind a scene
    // surface. Sample the scene depth at the FRAGMENT's screen position — vUV
    // is the quad-local UV, which only matches screen space for tiny centered
    // quads and breaks for off-center / large / streaked particles.
    if (uUseSoft != 0) {
        const vec2 screenUV = gl_FragCoord.xy / uViewportSize;
        const float sd = texture(uSceneDepth, screenUV).r;
        const vec4 clip = vec4(screenUV * 2.0 - 1.0, sd * 2.0 - 1.0, 1.0);
        const vec4 sv = uInvViewProj * clip;
        const float sceneZ = sv.z / sv.w;          // view-space z of the surface
        a *= smoothstep(0.0, uSoftRadius, sceneZ - vViewZ);
    }
    frag = vec4(rgb * texel.rgb, a);
}
