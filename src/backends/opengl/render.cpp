#include "common.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace ember::detail::opengl {
struct RenderState {
    GLint drawFbo, readFbo, viewport[4], depthFunc;
    GLboolean depthTest, depthWrite, cullFace, scissor;
    RenderState() {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
        depthTest = glIsEnabled(GL_DEPTH_TEST);
        cullFace = glIsEnabled(GL_CULL_FACE);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite);
    }
    ~RenderState() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        if (depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (cullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        restoreScissor();
        glDepthMask(depthWrite);
        glDepthFunc(depthFunc);
    }
    void restoreScissor() const {
        if (scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    }
};

void OpenGLBackend::render(const RenderParameters& p,const RenderView& camera) {
    const auto& view=camera.view;
    const auto& proj=camera.projection;
    const float viewportWidth=camera.width,viewportHeight=camera.height,fovYDeg=camera.fovYDegrees;

    if ((!gpuDriven_ && alive_ == 0) || viewportWidth <= 0 || viewportHeight <= 0 ||
        !std::isfinite(viewportWidth) || !std::isfinite(viewportHeight)) return;
    RenderState state;
    glDisable(GL_CULL_FACE); // billboards/fullscreen triangles are two-sided
    if (!renderProg_) ensurePrograms();
    if (sortEnabled_) sortParticles(view);
    (void)fovYDeg;
    const int w = (int)viewportWidth, h = (int)viewportHeight;
    glViewport(0, 0, w, h);
    if (bloom_) ensureBloom(w, h);
    const bool useBloom = bloom_ && bloomPass_ && bloomBlur_ && bloomComposite_;
    if (useBloom) {
        // Host scissor coordinates only apply to the final framebuffer. Half
        // resolution intermediates must be fully cleared and processed.
        glDisable(GL_SCISSOR_TEST);
        EMBER_BENCH_BEGIN(BloomPrepare);
        // Resolve the host depth into an identically formatted attachment.
        // This preserves scene occlusion for the offscreen particle pass,
        // including hosts whose depth buffer is multisampled.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, state.drawFbo);
        GLint bits = 0, component = GL_UNSIGNED_NORMALIZED, stencil = 0;
        if (p.depthTest) {
            const GLenum attachment = state.drawFbo ? GL_DEPTH_ATTACHMENT : GL_DEPTH;
            GLint object = GL_FRAMEBUFFER_DEFAULT;
            if (state.drawFbo)
                glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment,
                    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &object);
            if (object != GL_NONE) {
                glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment,
                    GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &bits);
                glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment,
                    GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &component);
                glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment,
                    GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencil);
                // Default framebuffer's stencil attachment is queried separately.
                if (!state.drawFbo)
                    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_STENCIL,
                        GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencil);
            }
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bloomFbo_);
        if (bits) {
            GLenum internal = stencil ? (component == GL_FLOAT ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8)
                : component == GL_FLOAT ? GL_DEPTH_COMPONENT32F
                : bits <= 16 ? GL_DEPTH_COMPONENT16 : bits <= 24 ? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT32;
            glActiveTexture(GL_TEXTURE0);
            bloomDepthTex_.bind();
            glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0,
                stencil ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT,
                stencil ? (component == GL_FLOAT ? GL_FLOAT_32_UNSIGNED_INT_24_8_REV : GL_UNSIGNED_INT_24_8) : GL_FLOAT, nullptr);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, bloomDepthTex_.id(), 0);
            glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        } else {
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        }
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        EMBER_BENCH_END(BloomPrepare);
        EMBER_BENCH_BEGIN(BloomDraw);
        drawParticles(p,view, proj, viewportWidth, viewportHeight);
        EMBER_BENCH_END(BloomDraw);
        // Draw the actual particles onto the host with their chosen blending
        // and depth policy. The HDR target contributes only the halo.
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state.drawFbo);
        state.restoreScissor();
        EMBER_BENCH_BEGIN(Draw);
        drawParticles(p,view, proj, viewportWidth, viewportHeight);
        EMBER_BENCH_END(Draw);
        EMBER_BENCH_BEGIN(BloomPost);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        const int hw = std::max(1, w / 2), hh = std::max(1, h / 2);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bloomHalfFbo_);
        glViewport(0, 0, hw, hh);
        glUseProgram(bloomPass_.id());
        bloomPass_.setInt("uTex", 0);
        bloomPass_.setFloat("uThreshold", p.bloomThreshold);
        bloomPass_.setVec2("uTexel", {1.f / w, 1.f / h});
        BloomParams bp{};
        bp.texelX = 1.f / w; bp.texelY = 1.f / h; bp.threshold = p.bloomThreshold;
        uboBloom_.subData(0, sizeof(bp), &bp);
        glBindBufferBase(GL_UNIFORM_BUFFER, kBindingUboBloom, uboBloom_.id());        glActiveTexture(GL_TEXTURE0);
        bloomTex_.bind();
        drawFullscreen();
        for (int i = 0; i < 2; ++i) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, i == 0 ? bloomBlurFbo_ : bloomHalfFbo_);
            glUseProgram(bloomBlur_.id());
            bloomBlur_.setInt("uTex", 0);
            bloomBlur_.setVec2("uTexel", {1.f / hw, 1.f / hh});
            bloomBlur_.setVec2("uDir", i == 0 ? glm::vec2(1, 0) : glm::vec2(0, 1));
            bp.texelX = 1.f / hw; bp.texelY = 1.f / hh;
            bp.dirX = i == 0 ? 1.f : 0.f; bp.dirY = i == 0 ? 0.f : 1.f;
            uboBloom_.subData(0, sizeof(bp), &bp);
            glBindBufferBase(GL_UNIFORM_BUFFER, kBindingUboBloom, uboBloom_.id());            (i == 0 ? bloomHalfTex_ : bloomBlurTex_).bind();
            drawFullscreen();
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state.drawFbo);
        glViewport(0, 0, w, h);
        state.restoreScissor();
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE);
        glUseProgram(bloomComposite_.id());
        bloomComposite_.setInt("uTex", 0);
        bloomHalfTex_.bind();
        drawFullscreen();
        glDisable(GL_BLEND);
        EMBER_BENCH_END(BloomPost);
    } else {
        EMBER_BENCH_BEGIN(Draw);
        drawParticles(p,view, proj, viewportWidth, viewportHeight);
        EMBER_BENCH_END(Draw);
    }
    if (p.refraction.enabled && sceneColor_) {
        EMBER_BENCH_BEGIN(Refraction);
        drawParticles(p,view, proj, viewportWidth, viewportHeight, true);
        EMBER_BENCH_END(Refraction);
    }
}

void OpenGLBackend::drawParticles(const RenderParameters& p,const glm::mat4& view, const glm::mat4& proj,
                                   float viewportWidth, float viewportHeight, bool refractionPass) {
    glUseProgram(renderProg_.id());
    renderProg_.setMat4("uView", view);
    renderProg_.setMat4("uProj", proj);
    renderProg_.setFloat("uSizeScale", p.sizeScale);
    renderProg_.setFloat("uStreak", p.streak);
    renderProg_.setInt("uUseSorted", sortEnabled_ ? 1 : 0);
    renderProg_.setInt("uUseSprite", p.useSprite ? 1 : 0);
    renderProg_.setInt("uSheetCols", p.sheetCols);
    renderProg_.setInt("uSheetRows", p.sheetRows);
    renderProg_.setVec2("uViewportSize", {viewportWidth, viewportHeight});
    renderProg_.setFloat("uSpinSpeed", p.spin);
    renderProg_.setInt("uRefraction", refractionPass ? 1 : 0);

    // Built-in shaders read std140 blocks (bindings 17/18); the loose
    // uniforms above serve custom shaders on the legacy contract.
    DrawParams dp{};
    dp.view = view; dp.proj = proj;
    dp.sizeScale = p.sizeScale; dp.streak = p.streak; dp.spinSpeed = p.spin;
    dp.useSorted = sortEnabled_ ? 1 : 0;
    dp.refraction = refractionPass ? 1 : 0;
    FragParams fp{};
    fp.invProj = glm::inverse(proj); fp.fragProj = proj;
    fp.viewportWidth = viewportWidth; fp.viewportHeight = viewportHeight;
    fp.softRadius = p.softRadius;
    fp.useSprite = p.useSprite ? 1 : 0;
    fp.sheetCols = p.sheetCols; fp.sheetRows = p.sheetRows;
    fp.fragRefraction = refractionPass ? 1 : 0;
    if (refractionPass) {
        // Refractive particles: replace pixels (blend off) by sampling the
        // host's scene color texture (unit 2) with a facet offset.
        int mode = p.refraction.mode;
        const bool hasDepth = refractionDepth_ != 0 || sceneDepth_ != 0;
        if (mode == 1 && !hasDepth) mode = 0; // depth-aware without a depth texture
        renderProg_.setInt("uRefrMode", mode);
        renderProg_.setInt("uRefrShape", p.refraction.shape);
        renderProg_.setFloat("uRefrDome", p.refraction.dome);
        renderProg_.setFloat("uRefrStrength", p.refraction.strength);
        renderProg_.setFloat("uRefrIor", p.refraction.ior);
        renderProg_.setVec3("uRefrTint", p.refraction.tint);
        renderProg_.setFloat("uRefrAbsorption", p.refraction.absorption);
        renderProg_.setFloat("uRefrFresnel", p.refraction.fresnel);
        renderProg_.setFloat("uRefrChroma", p.refraction.chroma);
        renderProg_.setFloat("uRefrSpecular", p.refraction.specular);
        renderProg_.setVec3("uRefrLightDir", glm::mat3(view) * p.refraction.lightDir);
        fp.refrMode = mode; fp.refrShape = p.refraction.shape;
        fp.refrDome = p.refraction.dome; fp.refrStrength = p.refraction.strength;
        fp.refrIor = p.refraction.ior; fp.refrTint = p.refraction.tint;
        fp.refrAbsorption = p.refraction.absorption; fp.refrFresnel = p.refraction.fresnel;
        fp.refrChroma = p.refraction.chroma; fp.refrSpecular = p.refraction.specular;
        fp.refrLightDir = glm::mat3(view) * p.refraction.lightDir;        // Use the system's explicit policy, not the host's previous state.
        // Pixel replacement still needs occlusion when depth testing is on.
        if (p.depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glDepthMask(p.depthWrite ? GL_TRUE : GL_FALSE);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, sceneColor_);
        renderProg_.setInt("uSceneColor", 2);
        if (mode == 1) {
            const GLuint depth = refractionDepth_ ? refractionDepth_ : sceneDepth_;
            renderProg_.setMat4("uInvProj", glm::inverse(proj)); // scene-point reconstruction
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, depth);
            renderProg_.setInt("uSceneDepth", 1);
        }
        glDisable(GL_BLEND);
    } else {
        const bool soft = p.softParticles && sceneDepth_ != 0;
        renderProg_.setInt("uUseSoft", soft ? 1 : 0);
        fp.useSoft = soft ? 1 : 0;        if (soft) {
            renderProg_.setMat4("uInvProj", glm::inverse(proj));
            renderProg_.setFloat("uSoftRadius", p.softRadius);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, sceneDepth_);
            renderProg_.setInt("uSceneDepth", 1);
        }
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        if (p.blend == BlendMode::Additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if (p.depthTest) glEnable(GL_DEPTH_TEST);
        else glDisable(GL_DEPTH_TEST);
        glDepthMask(p.depthWrite ? GL_TRUE : GL_FALSE);
    }

    uboDraw_.subData(0, sizeof(dp), &dp);
    uboFrag_.subData(0, sizeof(fp), &fp);
    glBindBufferBase(GL_UNIFORM_BUFFER, kBindingUboDraw, uboDraw_.id());
    glBindBufferBase(GL_UNIFORM_BUFFER, kBindingUboFrag, uboFrag_.id());

    glActiveTexture(GL_TEXTURE0);
    spriteTex_.bind();
    renderProg_.setInt("uSprite", 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingCur, cur_->id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingLive, liveBuf_.id());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingSorted, sortedBuf_.id());
    // Curves bind unconditionally: a custom render shader without BufCurves
    // simply ignores them (the render path stays tolerant).
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingCurves, curvesBuf_.id());
    vao_.bind();
    // Draw count comes from the GPU-written indirect args (phase 2 of update());
    // the CPU alive_ readback is only for aliveCount()/UI — not the render path.
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirectBuf_.id());
    glDrawArraysIndirect(GL_TRIANGLE_STRIP, nullptr);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    vao_.unbind();

    glDepthMask(GL_TRUE); // restore
}

void OpenGLBackend::drawFullscreen() {
    vao_.bind();
    glDrawArrays(GL_TRIANGLES, 0, 3);
    vao_.unbind();
}

void OpenGLBackend::ensureBloom(int w, int h) {
    RenderState state;
    try {
        if (!bloomPass_ || !bloomBlur_ || !bloomComposite_) {
            const std::string dir = shaderDir_.empty() ? "" : shaderDir_ + "/";
            auto pass = Shader::fromVertFrag((dir + "bloom.vert").c_str(), (dir + "bloom_threshold.frag").c_str());
            auto blur = Shader::fromVertFrag((dir + "bloom.vert").c_str(), (dir + "bloom_blur.frag").c_str());
            auto composite = Shader::fromVertFrag((dir + "bloom.vert").c_str(), (dir + "bloom_composite.frag").c_str());
            bloomPass_ = std::move(pass);
            bloomBlur_ = std::move(blur);
            bloomComposite_ = std::move(composite);
        }
        if (bloomFbo_ && w == bloomW_ && h == bloomH_) return;
        glActiveTexture(GL_TEXTURE0);
        bloomTex_.uploadRGBA16F(w, h);
        bloomHalfTex_.uploadRGBA16F(std::max(1, w / 2), std::max(1, h / 2));
        bloomBlurTex_.uploadRGBA16F(std::max(1, w / 2), std::max(1, h / 2));
        if (!bloomFbo_) {
            GLuint fbos[3]{};
            glGenFramebuffers(3, fbos);
            bloomFbo_ = fbos[0]; bloomHalfFbo_ = fbos[1]; bloomBlurFbo_ = fbos[2];
        }
        const GLuint fbos[] = {bloomFbo_, bloomHalfFbo_, bloomBlurFbo_};
        const GLuint textures[] = {bloomTex_.id(), bloomHalfTex_.id(), bloomBlurTex_.id()};
        for (int i = 0; i < 3; ++i) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbos[i]);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[i], 0);
            if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                throw std::runtime_error("incomplete bloom framebuffer");
        }
        bloomW_ = w; bloomH_ = h;
    } catch (const std::exception& e) {
        bloomPass_ = Shader(); bloomBlur_ = Shader(); bloomComposite_ = Shader();
        bloomW_ = bloomH_ = 0;
        bloom_ = false;
        std::fprintf(stderr, "[ember] bloom disabled: %s\n", e.what());
    }
}

} // namespace ember::detail::opengl
