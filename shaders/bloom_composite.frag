#version 430 core
// Final additive composite of the blurred bloom onto the scene.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 frag;

layout(binding = 0) uniform sampler2D uTex;

void main() {
    frag = texture(uTex, vUV);
}
