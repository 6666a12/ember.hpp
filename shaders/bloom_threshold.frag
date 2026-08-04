#version 430 core
// Bright pass + 2x2 downsample to half resolution.
in vec2 vUV;
out vec4 frag;

uniform sampler2D uTex;
uniform float uThreshold;
uniform vec2  uTexel; // 1/source size

void main() {
    vec2 t = uTexel;
    vec4 col = texture(uTex, vUV + vec2(-0.25, -0.25) * t) +
               texture(uTex, vUV + vec2(0.25, -0.25) * t) +
               texture(uTex, vUV + vec2(-0.25, 0.25) * t) +
               texture(uTex, vUV + vec2(0.25, 0.25) * t);
    frag = max(col * 0.25 - vec4(uThreshold), vec4(0.0));
}
