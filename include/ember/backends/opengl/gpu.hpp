#pragma once

// RAII wrappers for GL buffer objects and vertex arrays.

#include "ember/backends/opengl/gl.hpp"

namespace ember {

// RAII wrapper for a GL buffer object (VBO / SSBO / ...).
class Buffer {
public:
    explicit Buffer(GLenum target) : target_(target) { glGenBuffers(1, &id_); }
    ~Buffer() {
        if (id_) glDeleteBuffers(1, &id_);
    }

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    Buffer(Buffer&& o) noexcept : target_(o.target_), id_(o.id_) { o.id_ = 0; }
    Buffer& operator=(Buffer&& o) noexcept {
        if (this != &o) {
            if (id_) glDeleteBuffers(1, &id_);
            target_ = o.target_;
            id_ = o.id_;
            o.id_ = 0;
        }
        return *this;
    }

    void bind() const { glBindBuffer(target_, id_); }
    void data(const void* data, GLsizeiptr bytes, GLenum usage) {
        bind();
        glBufferData(target_, bytes, data, usage);
    }
    void subData(GLintptr offset, GLsizeiptr bytes, const void* data) {
        bind();
        glBufferSubData(target_, offset, bytes, data);
    }
    void getSubData(GLintptr offset, GLsizeiptr bytes, void* out) {
        bind();
        glGetBufferSubData(target_, offset, bytes, out);
    }
    GLuint id() const { return id_; }
    GLenum target() const { return target_; }

private:
    GLenum target_ = GL_ARRAY_BUFFER;
    GLuint id_ = 0;
};

// RAII wrapper for a 2D texture (sprite atlases etc.).
class Texture {
public:
    Texture() { glGenTextures(1, &id_); }
    ~Texture() {
        if (id_) glDeleteTextures(1, &id_);
    }

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    Texture(Texture&& o) noexcept : id_(o.id_) { o.id_ = 0; }
    Texture& operator=(Texture&& o) noexcept {
        if (this != &o) {
            if (id_) glDeleteTextures(1, &id_);
            id_ = o.id_;
            o.id_ = 0;
        }
        return *this;
    }

    void bind() const { glBindTexture(GL_TEXTURE_2D, id_); }
    GLuint id() const { return id_; }

    // Depth-only attachment (soft particles sample this).
    void uploadDepth(int w, int h) {
        bind(); // other upload*() bind first; without it the image lands in whatever
                // texture is bound on the active unit (e.g. the particle sprite).
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // Upload RGBA8 and set linear filtering + clamp-to-edge (point sprites
    // want no mipmaps).
    void uploadRGBA8(int w, int h, const void* pixels) {
        bind();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // Float RGBA16F texture (bloom passes).
    void uploadRGBA16F(int w, int h) {
        bind();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

private:
    GLuint id_ = 0;
};

// RAII wrapper for a vertex array object.
class VertexArray {
public:
    VertexArray() { glGenVertexArrays(1, &id_); }
    ~VertexArray() {
        if (id_) glDeleteVertexArrays(1, &id_);
    }

    VertexArray(const VertexArray&) = delete;
    VertexArray& operator=(const VertexArray&) = delete;

    VertexArray(VertexArray&& o) noexcept : id_(o.id_) { o.id_ = 0; }
    VertexArray& operator=(VertexArray&& o) noexcept {
        if (this != &o) {
            if (id_) glDeleteVertexArrays(1, &id_);
            id_ = o.id_;
            o.id_ = 0;
        }
        return *this;
    }

    void bind() const { glBindVertexArray(id_); }
    void unbind() const { glBindVertexArray(0); }
    GLuint id() const { return id_; }

private:
    GLuint id_ = 0;
};

} // namespace ember
