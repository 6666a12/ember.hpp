#version 430 core
// gl_VertexID/gl_InstanceID are gl_VertexIndex/gl_InstanceIndex in Vulkan GLSL
// (same base-offset semantics here: firstVertex/baseInstance are always 0).
#if defined(VULKAN)
#define EMBER_VERTEX_INDEX gl_VertexIndex
#define EMBER_INSTANCE_INDEX gl_InstanceIndex
#else
#define EMBER_VERTEX_INDEX gl_VertexID
#define EMBER_INSTANCE_INDEX gl_InstanceID
#endif

// Fullscreen triangle from gl_VertexID (no VBO/VAO attributes).
layout(location = 0) out vec2 vUV;

void main() {
    vec2 p = vec2(float((EMBER_VERTEX_INDEX << 1) & 2), float(EMBER_VERTEX_INDEX & 2));
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
