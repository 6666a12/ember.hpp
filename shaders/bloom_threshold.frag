#version 430 core
// Bright pass + 2x2 downsample to half resolution.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 frag;

layout(binding = 0) uniform sampler2D uTex;
// CPU mirror: BloomParams in src/backends/opengl/params.hpp.
layout(binding = 19, std140) uniform BloomParams {
    vec2 uTexel; float uThreshold; float uBloomPad0; // uTexel = 1/source size
    vec2 uDir;   vec2  uBloomPad1;
};

void main() {
    vec2 t = uTexel;
    vec4 col = texture(uTex, vUV + vec2(-0.25, -0.25) * t) +
               texture(uTex, vUV + vec2(0.25, -0.25) * t) +
               texture(uTex, vUV + vec2(-0.25, 0.25) * t) +
               texture(uTex, vUV + vec2(0.25, 0.25) * t);
    frag = max(col * 0.25 - vec4(uThreshold), vec4(0.0));
}
