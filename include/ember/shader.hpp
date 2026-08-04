#pragma once

// RAII GL program with cached uniform lookup.

#include "ember/gl.hpp"

#include <glm/glm.hpp>

#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ember {

class Shader {
public:
    Shader() = default;
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&&) noexcept;
    Shader& operator=(Shader&&) noexcept;

    // Compile + link from source strings, e.g.
    //   Shader::fromSources({ {GL_VERTEX_SHADER, vsSrc}, {GL_FRAGMENT_SHADER, fsSrc} });
    static Shader fromSources(std::initializer_list<std::pair<GLenum, const char*>> stages);
    static Shader fromSources(const std::vector<std::pair<GLenum, const char*>>& stages);
    static Shader fromFiles(std::initializer_list<std::pair<GLenum, const char*>> paths);
    // Convenience wrappers: vertex+fragment program / compute program from files.
    static Shader fromVertFrag(const char* vertPath, const char* fragPath);
    static Shader fromCompute(const char* compPath);

    void use() const { glUseProgram(id_); }
    GLuint id() const { return id_; }
    explicit operator bool() const { return id_ != 0; }

    // Cached uniform location (returns -1 if the uniform does not exist).
    GLint location(const char* name) const;

    void setInt(const char* n, int v);
    void setUint(const char* n, unsigned v);
    void setFloat(const char* n, float v);
    void setVec2(const char* n, const glm::vec2& v);
    void setVec3(const char* n, const glm::vec3& v);
    void setVec4(const char* n, const glm::vec4& v);
    void setMat4(const char* n, const glm::mat4& v);

private:
    explicit Shader(GLuint id) : id_(id) {}

    GLuint id_ = 0;
    mutable std::unordered_map<std::string, GLint> locs_;
};

} // namespace ember
