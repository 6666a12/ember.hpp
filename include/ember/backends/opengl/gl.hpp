#pragma once

// GL function loading + error helpers.
// The core library never creates a GL context — the host application does
// (GLFW / SDL2 / Qt / Win32 / EGL / your engine), then hands its loader here:
//
//     ember::gl::init(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress));
//
// Works with any loader that resolves GL function names.

#include <glad/gl.h>

#include <cstdio>

namespace ember {
namespace gl {

// Initialize GL function pointers. Must be called after a context is current.
inline int init(GLADloadfunc loader) {
    return gladLoadGL(loader);
}

inline const char* errorString(GLenum e) {
    switch (e) {
        case GL_NO_ERROR: return "GL_NO_ERROR";
        case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
        case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
        case GL_STACK_UNDERFLOW: return "GL_STACK_UNDERFLOW";
        case GL_STACK_OVERFLOW: return "GL_STACK_OVERFLOW";
        default: return "unknown GL error";
    }
}

// Pop and print all pending GL errors; returns how many were found.
inline int popErrors(const char* where = nullptr) {
    int n = 0;
    while (GLenum e = glGetError()) {
        if (where) std::fprintf(stderr, "[gl] %s: %s (0x%04x)\n", where, errorString(e), e);
        else std::fprintf(stderr, "[gl] %s (0x%04x)\n", errorString(e), e);
        ++n;
    }
    return n;
}

} // namespace gl
} // namespace ember
