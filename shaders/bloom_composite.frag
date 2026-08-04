#version 430 core
// Final additive composite of the blurred bloom onto the scene.
in vec2 vUV;
out vec4 frag;

uniform sampler2D uTex;

void main() {
    frag = texture(uTex, vUV);
}
