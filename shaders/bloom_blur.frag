#version 430 core
// 9-tap Gaussian blur along uDir (ping-pong passes).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 frag;

layout(binding = 0) uniform sampler2D uTex;
// CPU mirror: BloomParams in src/backends/opengl/params.hpp.
layout(binding = 19, std140) uniform BloomParams {
    vec2 uTexel; float uThreshold; float uBloomPad0;
    vec2 uDir;   vec2  uBloomPad1;
};

void main() {
    vec2 off = uDir * uTexel;
    vec4 col = texture(uTex, vUV) * 0.227027;
    col += texture(uTex, vUV + off * 1.384615) * 0.316216;
    col += texture(uTex, vUV - off * 1.384615) * 0.316216;
    col += texture(uTex, vUV + off * 3.230769) * 0.070270;
    col += texture(uTex, vUV - off * 3.230769) * 0.070270;
    frag = col;
}
