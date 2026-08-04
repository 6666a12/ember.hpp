#version 430 core
// 9-tap Gaussian blur along uDir (ping-pong passes).
in vec2 vUV;
out vec4 frag;

uniform sampler2D uTex;
uniform vec2  uTexel;
uniform vec2  uDir;

void main() {
    vec2 off = uDir * uTexel;
    vec4 col = texture(uTex, vUV) * 0.227027;
    col += texture(uTex, vUV + off * 1.384615) * 0.316216;
    col += texture(uTex, vUV - off * 1.384615) * 0.316216;
    col += texture(uTex, vUV + off * 3.230769) * 0.070270;
    col += texture(uTex, vUV - off * 3.230769) * 0.070270;
    frag = col;
}
