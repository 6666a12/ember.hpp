#include "ember/backends/opengl/shader.hpp"

#include <glm/gtc/type_ptr.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ember {

namespace {

std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error(std::string("ember: cannot open shader file: ") + path);
    std::ostringstream ss;
    char buffer[4096];
    while (f.read(buffer, sizeof(buffer))) ss.write(buffer, f.gcount());
    ss.write(buffer, f.gcount());
    if (f.bad() || !f.eof() || !ss)
        throw std::runtime_error(std::string("ember: cannot read shader file: ") + path);
    return ss.str();
}

struct StageOwner {
    GLuint id;
    ~StageOwner() { if (id) glDeleteShader(id); }
    GLuint release() { GLuint result = id; id = 0; return result; }
};

GLuint compileStage(GLenum stage, const char* src) {
    StageOwner owner{glCreateShader(stage)};
    GLuint s = owner.id;
    if (!s) throw std::runtime_error("ember: cannot create shader");
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[8192];
        GLsizei len = 0;
        glGetShaderInfoLog(s, sizeof(log), &len, log);
        std::string msg(log, len);
        throw std::runtime_error("ember: shader compile error: " + msg);
    }
    return owner.release();
}

} // namespace

Shader::~Shader() {
    if (id_) glDeleteProgram(id_);
}

Shader::Shader(Shader&& o) noexcept : id_(o.id_), locs_(std::move(o.locs_)),
                                      uniforms_(std::move(o.uniforms_)) { o.id_ = 0; }

Shader& Shader::operator=(Shader&& o) noexcept {
    if (this != &o) {
        if (id_) glDeleteProgram(id_);
        id_ = o.id_;
        o.id_ = 0;
        locs_ = std::move(o.locs_);
        uniforms_ = std::move(o.uniforms_);
    }
    return *this;
}

Shader Shader::fromSources(const std::vector<std::pair<GLenum, const char*>>& stages) {
    GLuint prog = glCreateProgram();
    std::vector<GLuint> shaders;
    try {
        for (const auto& [stage, src] : stages) {
            StageOwner shader{compileStage(stage, src)};
            shaders.push_back(shader.id);
            glAttachShader(prog, shader.release());
        }
        glLinkProgram(prog);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[8192];
            GLsizei len = 0;
            glGetProgramInfoLog(prog, sizeof(log), &len, log);
            throw std::runtime_error(std::string("ember: program link error: ") + std::string(log, len));
        }
    } catch (...) {
        for (GLuint s : shaders) glDeleteShader(s);
        glDeleteProgram(prog);
        throw;
    }
    for (GLuint s : shaders) glDeleteShader(s);
    return Shader(prog);
}

Shader Shader::fromSources(std::initializer_list<std::pair<GLenum, const char*>> stages) {
    return fromSources(std::vector<std::pair<GLenum, const char*>>(stages));
}

Shader Shader::fromFiles(std::initializer_list<std::pair<GLenum, const char*>> paths) {
    std::vector<std::pair<GLenum, std::string>> srcs;
    srcs.reserve(paths.size());
    for (const auto& [stage, path] : paths) srcs.emplace_back(stage, readFile(path));
    std::vector<std::pair<GLenum, const char*>> views;
    views.reserve(srcs.size());
    for (const auto& [stage, src] : srcs) views.emplace_back(stage, src.c_str());
    return fromSources(views);
}

Shader Shader::fromVertFrag(const char* vertPath, const char* fragPath) {
    return fromFiles({{GL_VERTEX_SHADER, vertPath}, {GL_FRAGMENT_SHADER, fragPath}});
}

Shader Shader::fromCompute(const char* compPath) {
    return fromFiles({{GL_COMPUTE_SHADER, compPath}});
}

GLint Shader::location(const char* name) const {
    std::string key(name);
    auto it = locs_.find(key);
    if (it != locs_.end()) return it->second;
    GLint l = glGetUniformLocation(id_, name);
    locs_.emplace(std::move(key), l);
    return l;
}

bool Shader::hasUniform(const char* name) const {
    std::string key(name);
    auto it = uniforms_.find(key);
    if (it != uniforms_.end()) return it->second;
    // Program-interface query sees std140 block members too (blocks declared
    // without an instance name expose plain member names).
    const bool found = glGetProgramResourceIndex(id_, GL_UNIFORM, name) != GL_INVALID_INDEX;
    uniforms_.emplace(std::move(key), found);
    return found;
}

void Shader::setInt(const char* n, int v)   { glUniform1i(location(n), v); }
void Shader::setUint(const char* n, unsigned v) { glUniform1ui(location(n), v); }
void Shader::setFloat(const char* n, float v)   { glUniform1f(location(n), v); }
void Shader::setVec2(const char* n, const glm::vec2& v) { glUniform2fv(location(n), 1, glm::value_ptr(v)); }
void Shader::setVec3(const char* n, const glm::vec3& v) { glUniform3fv(location(n), 1, glm::value_ptr(v)); }
void Shader::setVec4(const char* n, const glm::vec4& v) { glUniform4fv(location(n), 1, glm::value_ptr(v)); }
void Shader::setMat4(const char* n, const glm::mat4& v) { glUniformMatrix4fv(location(n), 1, GL_FALSE, glm::value_ptr(v)); }

} // namespace ember
