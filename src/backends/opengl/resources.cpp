#include "common.hpp"
#ifdef EMBER_USE_STB
#include "stb_image.h"
#endif
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace ember::detail::opengl {
OpenGLBackend::OpenGLBackend()
    : bufA_(GL_SHADER_STORAGE_BUFFER),
      bufB_(GL_SHADER_STORAGE_BUFFER),
      spawnBuf_(GL_SHADER_STORAGE_BUFFER),
      paletteBuf_(GL_SHADER_STORAGE_BUFFER),
      deadBuf_(GL_SHADER_STORAGE_BUFFER),
      counterBuf_(GL_SHADER_STORAGE_BUFFER),
      attractorBuf_(GL_SHADER_STORAGE_BUFFER),
      vortexBuf_(GL_SHADER_STORAGE_BUFFER),
      springBuf_(GL_SHADER_STORAGE_BUFFER),
      sortedBuf_(GL_SHADER_STORAGE_BUFFER),
      liveBuf_(GL_SHADER_STORAGE_BUFFER),
      nextLiveBuf_(GL_SHADER_STORAGE_BUFFER),
      sortKeyBuf_(GL_SHADER_STORAGE_BUFFER),
      indirectBuf_(GL_SHADER_STORAGE_BUFFER),
      dispatchBuf_(GL_SHADER_STORAGE_BUFFER),
      eventTagBuf_(GL_SHADER_STORAGE_BUFFER),
      eventTemplateBuf_(GL_SHADER_STORAGE_BUFFER),
      eventQueueBuf_(GL_SHADER_STORAGE_BUFFER),
      curvesBuf_(GL_SHADER_STORAGE_BUFFER),
      uboSim_(GL_UNIFORM_BUFFER),
      uboSort_(GL_UNIFORM_BUFFER),
      uboSchedule_(GL_UNIFORM_BUFFER),
      uboDraw_(GL_UNIFORM_BUFFER),
      uboFrag_(GL_UNIFORM_BUFFER),
      uboBloom_(GL_UNIFORM_BUFFER) {}

void OpenGLBackend::initialize(std::uint32_t capacity,bool debug) {
    capacity_=capacity;debug_=debug;
    if (capacity_ == 0 || capacity_ > (1u << 30))
        throw std::invalid_argument("ember: capacity must be in [1, 2^30]");
    cur_ = &bufA_;
    nxt_ = &bufB_;


    uploadBuiltinSprite();

    if (debug_) {
        std::fprintf(stderr, "[ember] GL_VERSION=%s GL_RENDERER=%s\n",
                     (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
    }

    const GLsizeiptr pbytes = (GLsizeiptr)capacity_ * (GLsizeiptr)sizeof(Particle);
    std::vector<std::byte> zeros(pbytes);
    bufA_.data(zeros.data(), pbytes, GL_DYNAMIC_COPY);
    bufB_.data(zeros.data(), pbytes, GL_DYNAMIC_COPY);
    spawnBuf_.data(nullptr, (GLsizeiptr)kMaxSpawnRequests * (GLsizeiptr)sizeof(SpawnRequest), GL_DYNAMIC_DRAW);
    paletteBuf_.data(nullptr, 0, GL_DYNAMIC_DRAW);
    deadBuf_.data(nullptr, (GLsizeiptr)capacity_ * (GLsizeiptr)sizeof(GLuint), GL_DYNAMIC_DRAW);
    const GLuint ctr[8] = {0, 0, 0, capacity_, 0, 0, 0, 0};
    counterBuf_.data(ctr, sizeof(ctr), GL_DYNAMIC_DRAW);
    attractorBuf_.data(nullptr, 0, GL_DYNAMIC_DRAW);
    vortexBuf_.data(nullptr, 0, GL_DYNAMIC_DRAW);
    springBuf_.data(nullptr, 0, GL_DYNAMIC_DRAW);
    liveBuf_.data(nullptr, (GLsizeiptr)capacity_ * sizeof(GLuint), GL_DYNAMIC_DRAW);
    nextLiveBuf_.data(nullptr, (GLsizeiptr)capacity_ * sizeof(GLuint), GL_DYNAMIC_DRAW);
    sortedBuf_.data(nullptr, (GLsizeiptr)nextPow2(capacity_) * (GLsizeiptr)sizeof(GLuint), GL_DYNAMIC_DRAW);
    const GLuint indirectInit[4] = {4, 0, 0, 0}; // {vertexCount, instanceCount, first, baseInstance}
    indirectBuf_.data(indirectInit, sizeof(indirectInit), GL_DYNAMIC_DRAW);
    eventTagBuf_.data(nullptr, (GLsizeiptr)capacity_ * 2 * sizeof(GLuint), GL_DYNAMIC_DRAW); // uvec2: tag + birth id
    eventTemplateBuf_.data(nullptr, (GLsizeiptr)maxEventTemplates * (GLsizeiptr)sizeof(SpawnRequest), GL_DYNAMIC_DRAW);
    eventQueueBuf_.data(nullptr, (GLsizeiptr)kEventQueueBytes, GL_DYNAMIC_DRAW);
    const GLuint queueZero[4] = {0, 0, 0, 0};
    eventQueueBuf_.subData(0, sizeof(queueZero), queueZero);
    // Lifecycle curves default to all-ones (disabled) so the shader multiply is
    // bit-identical to the plain path until the facade uploads real keys.
    CurvesParams curves;
    for (auto& c : curves.colorLut) c = glm::vec4(1.f);
    for (auto& s : curves.sizeLut) s = 1.f;
    curvesBuf_.data(&curves, sizeof(curves), GL_DYNAMIC_DRAW);

    // std140 parameter blocks for the built-in shaders (bindings 14-19).
    uboSim_.data(nullptr, sizeof(SimParams), GL_DYNAMIC_DRAW);
    uboSort_.data(nullptr, sizeof(SortParams), GL_DYNAMIC_DRAW);
    uboSchedule_.data(nullptr, sizeof(ScheduleParams), GL_DYNAMIC_DRAW);
    uboDraw_.data(nullptr, sizeof(DrawParams), GL_DYNAMIC_DRAW);
    uboFrag_.data(nullptr, sizeof(FragParams), GL_DYNAMIC_DRAW);
    uboBloom_.data(nullptr, sizeof(BloomParams), GL_DYNAMIC_DRAW);

    // Instanced quads: no vertex attributes — the VS expands gl_VertexID.
    vao_.bind();
    vao_.unbind();

    ensurePrograms();
}

OpenGLBackend::~OpenGLBackend() {
    for (auto fence : statisticsFences_) if (fence) glDeleteSync(fence);
    glDeleteBuffers((GLsizei)statisticsBuffers_.size(), statisticsBuffers_.data());
    if (bloomFbo_) {
        glDeleteFramebuffers(1, &bloomFbo_);
        glDeleteFramebuffers(1, &bloomHalfFbo_);
        glDeleteFramebuffers(1, &bloomBlurFbo_);
    }
}

void OpenGLBackend::setSpriteTexture(const char* pngPath) {
    if (!pngPath || pngPath[0] == '\0') {
        uploadBuiltinSprite();
        return;
    }
    int w = 0, h = 0, ch = 0;
    unsigned char* data = nullptr;
#ifdef EMBER_USE_STB
    data = stbi_load(pngPath, &w, &h, &ch, 4);
    if (!data) {
        std::fprintf(stderr, "[ember] warning: cannot load sprite '%s' (%s); using built-in gradient\n",
                     pngPath, stbi_failure_reason());
        uploadBuiltinSprite();
        return;
    }
#else
    (void)w; (void)h; (void)ch;
    std::fprintf(stderr,
                 "[ember] warning: cannot load sprite '%s' (EMBER_USE_STB not defined); using built-in gradient\n",
                 pngPath);
    uploadBuiltinSprite();
    return;
#endif
    spriteTex_.uploadRGBA8(w, h, data);
#ifdef EMBER_USE_STB
    stbi_image_free(data);
#endif
    if (debug_) std::fprintf(stderr, "[ember] sprite '%s' loaded (%dx%d)\n", pngPath, w, h);
}

void OpenGLBackend::uploadBuiltinSprite() {
    constexpr int kSize = 64;
    std::vector<unsigned char> px((std::size_t)kSize * kSize * 4, 0);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float dx = (x + 0.5f) / kSize * 2.f - 1.f;
            const float dy = (y + 0.5f) / kSize * 2.f - 1.f;
            const float r = std::sqrt(dx * dx + dy * dy);
            float a = 1.f - std::min(r, 1.f);                 // soft disc
            a += (1.f - std::min(r * r, 1.f)) * 0.35f;        // faint halo
            const std::size_t i = ((std::size_t)y * kSize + (std::size_t)x) * 4;
            px[i + 0] = 255;
            px[i + 1] = 255;
            px[i + 2] = 255;
            px[i + 3] = (unsigned char)(glm::clamp(a, 0.f, 1.f) * 255.f);
        }
    }
    spriteTex_.uploadRGBA8(kSize, kSize, px.data());
}

void OpenGLBackend::setShaderDirectory(const char* dir) { shaderDir_ = dir ? dir : ""; }

void OpenGLBackend::ensurePrograms() {
    if (renderProg_ || simProg_) return;
    // Prefer editable files in the shader directory (shaders/particle.vert,
    // .frag, simulate.comp); fall back to the embedded copies when the files
    // are missing or fail to compile, so embedding stays zero-file.
    const std::string dir = shaderDir_.empty() ? "" : shaderDir_ + "/";
    bool fromFiles = true;
    try {
        renderProg_ = Shader::fromVertFrag((dir + "particle.vert").c_str(), (dir + "particle.frag").c_str());
        simProg_ = Shader::fromCompute((dir + "simulate.comp").c_str());
    } catch (const std::exception& e) {
        if (debug_) std::fprintf(stderr, "[ember] shader files unavailable (%s); using embedded\n", e.what());
        renderProg_ = Shader();
        simProg_ = Shader();
        fromFiles = false;
    }
    if (!fromFiles) {
        renderProg_ = Shader::fromSources({
            {GL_VERTEX_SHADER, kParticleVert},
            {GL_FRAGMENT_SHADER, kParticleFrag},
        });
        simProg_ = Shader::fromSources({{GL_COMPUTE_SHADER, kSimulateComp}});
    } else if (debug_) {
        std::fprintf(stderr, "[ember] shaders loaded from %s/\n", dir.c_str());
    }
}

void OpenGLBackend::setPrograms(Shader render, Shader simulate) {
    if (gpuDriven_ && (!simulate.hasUniform("uGpuDriven") || !simulate.hasUniform("uInputAlive")))
        throw std::invalid_argument("ember: incompatible simulation shader in GPU-driven mode");
    if (allocated_ && simulate.hasUniform("uInputAlive") &&
        !simProg_.hasUniform("uInputAlive")) {
        // A legacy integrator may leave the previous frame's live contents
        // in inactive slots of nxt_. Live-only traversal will never touch
        // those slots again, so initialize its tombstones once on transition.
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_COPY_READ_BUFFER, cur_->id());
        glBindBuffer(GL_COPY_WRITE_BUFFER, nxt_->id());
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0,
                           (GLsizeiptr)allocated_ * sizeof(Particle));
    }
    renderProg_ = std::move(render);
    simProg_ = std::move(simulate);
}

void OpenGLBackend::uploadAttractors(const std::vector<Attractor>& a) {
    std::vector<glm::vec4> v;
    v.reserve(a.size());
    for (const auto& at : a) v.emplace_back(at.position, at.strength);
    attractorBuf_.data(v.empty() ? nullptr : v.data(),
                       (GLsizeiptr)(v.size() * sizeof(glm::vec4)), GL_DYNAMIC_DRAW);
}

void OpenGLBackend::uploadVortexes(const std::vector<Vortex>& v) {
    vortexBuf_.data(v.empty() ? nullptr : v.data(),
                    (GLsizeiptr)(v.size() * sizeof(Vortex)), GL_DYNAMIC_DRAW);
}

void OpenGLBackend::uploadSprings(const std::vector<Spring>& s) {
    springBuf_.data(s.empty() ? nullptr : s.data(),
                    (GLsizeiptr)(s.size() * sizeof(Spring)), GL_DYNAMIC_DRAW);
}

void OpenGLBackend::uploadPalette(const std::vector<glm::vec4>& values) {
    paletteBuf_.data(values.empty()?nullptr:values.data(),GLsizeiptr(values.size()*sizeof(glm::vec4)),GL_DYNAMIC_DRAW);
}

void OpenGLBackend::uploadEventTemplates(const std::vector<SpawnRequest>& templates) {
    if (templates.empty()) {
        eventTemplateCount_ = 0;
        eventTemplateBuf_.data(nullptr, 0, GL_DYNAMIC_DRAW); // release; empty never throws
        return;
    }
    // Legacy custom simulation shaders predate the event protocol; reject them
    // rather than feeding a buffer the shader cannot address.
    ensurePrograms();
    if (simProg_ && glGetProgramResourceIndex(simProg_.id(), GL_SHADER_STORAGE_BLOCK,
                                              "BufEventQueue") == GL_INVALID_INDEX)
        throw std::invalid_argument("ember: custom simulation shader lacks the event queue protocol");
    eventTemplateCount_ = (std::uint32_t)templates.size();
    eventTemplateBuf_.data(templates.data(),
                           (GLsizeiptr)templates.size() * (GLsizeiptr)sizeof(SpawnRequest),
                           GL_DYNAMIC_DRAW);
}

void OpenGLBackend::uploadLifeCurves(const LifeCurvesLut& lut) {
    CurvesParams curves;
    std::copy(lut.color, lut.color + kLifeCurveLutSize, curves.colorLut);
    std::copy(lut.size, lut.size + kLifeCurveLutSize, curves.sizeLut);
    curvesBuf_.subData(0, sizeof(curves), &curves);
}
void OpenGLBackend::validateConfiguration(std::uint32_t capacity,bool sorting) {
    if(capacity==0 || capacity>(1u<<30)) throw std::invalid_argument("ember: invalid capacity");
    if(gpuDriven_) {
        validateGpuCapacity(capacity);
        if(sorting) {
            ensureSortProgram();
            if(!sortProg_ || !sortProg_.hasUniform("uGpuDriven") || !sortProg_.hasUniform("uTileMode"))
                throw std::invalid_argument("ember: GPU-driven mode requires the GPU scheduling sort protocol");
        }
    }
}
void OpenGLBackend::resize(std::uint32_t capacity) {
    validateConfiguration(capacity,sortEnabled_);
    capacity_ = capacity;
    allocated_ = 0;
    alive_ = 0;
    const GLsizeiptr pbytes = (GLsizeiptr)capacity_ * (GLsizeiptr)sizeof(Particle);
    std::vector<std::byte> zeros(pbytes);
    bufA_.data(zeros.data(), pbytes, GL_DYNAMIC_COPY);
    bufB_.data(zeros.data(), pbytes, GL_DYNAMIC_COPY);
    deadBuf_.data(nullptr, (GLsizeiptr)capacity_ * (GLsizeiptr)sizeof(GLuint), GL_DYNAMIC_DRAW);
    liveBuf_.data(nullptr, (GLsizeiptr)capacity_ * sizeof(GLuint), GL_DYNAMIC_DRAW);
    nextLiveBuf_.data(nullptr, (GLsizeiptr)capacity_ * sizeof(GLuint), GL_DYNAMIC_DRAW);
    sortKeyBuf_.data(nullptr, 0, GL_DYNAMIC_DRAW); // lazy resize on next sort
    sortKeyCapacity_ = 0;
    sortedBuf_.data(nullptr, (GLsizeiptr)nextPow2(capacity_) * (GLsizeiptr)sizeof(GLuint), GL_DYNAMIC_DRAW);
    eventTagBuf_.data(nullptr, (GLsizeiptr)capacity_ * 2 * sizeof(GLuint), GL_DYNAMIC_DRAW); // uvec2: tag + birth id
    const GLuint ctr[8] = {0, 0, 0, capacity_, 0, 0, 0, 0};
    counterBuf_.data(ctr, sizeof(ctr), GL_DYNAMIC_DRAW);
    const GLuint args[4]={4,0,0,0};
    indirectBuf_.data(args,sizeof(args),GL_DYNAMIC_DRAW);
    const GLuint queueZero[4] = {0, 0, 0, 0};
    eventQueueBuf_.subData(0, sizeof(queueZero), queueZero);
    resetStatistics();
    if (gpuDriven_ && sortEnabled_) scheduleGpu(1);
}
void OpenGLBackend::clear() {
    alive_ = 0;
    allocated_ = 0;
    const GLsizeiptr pbytes = (GLsizeiptr)capacity_ * (GLsizeiptr)sizeof(Particle);
    std::vector<std::byte> zeros(pbytes);
    bufA_.data(zeros.data(), pbytes, GL_DYNAMIC_COPY);
    bufB_.data(zeros.data(), pbytes, GL_DYNAMIC_COPY);
    const GLuint ctr[8] = {0, 0, 0, capacity_, 0, 0, 0, 0};
    counterBuf_.data(ctr, sizeof(ctr), GL_DYNAMIC_DRAW);
    const GLuint args[4]={4,0,0,0};
    indirectBuf_.data(args,sizeof(args),GL_DYNAMIC_DRAW);
    eventTagBuf_.data(nullptr, (GLsizeiptr)capacity_ * 2 * sizeof(GLuint), GL_DYNAMIC_DRAW); // uvec2: tag + birth id
    const GLuint queueZero[4] = {0, 0, 0, 0};
    eventQueueBuf_.subData(0, sizeof(queueZero), queueZero);
    resetStatistics();
    if (gpuDriven_ && sortEnabled_) scheduleGpu(1);
}
} // namespace ember::detail::opengl
