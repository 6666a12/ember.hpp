#pragma once
// OpenGL-specific creation and native-resource customization. Current GL 4.3
// context required; external textures are borrowed, never deleted by ember.
#include "ember/backend.hpp"
#include "ember/backends/opengl/gpu.hpp"
#include "ember/backends/opengl/shader.hpp"

namespace ember {
struct RefractionSettings {
    bool enabled = false;
    GLuint sceneColorTex = 0;   // host scene color (RGBA, CLAMP_TO_EDGE)
    int mode = 0;               // 0 = simple offset, 1 = depth-aware, 2 = noise (heat)
    GLuint sceneDepthTex = 0;   // needed by mode 1; 0 falls back to mode 0
    float strength = 0.02f;     // simple/noise: UV offset; depth: projected displacement scale
    float ior = 1.5f;           // refractive index (mode 1)
    glm::vec3 tint{1.f};        // glass color absorption
    float absorption = 0.35f;   // mix(original, refracted, absorption)
    float fresnel = 2.f;        // rim brightening exponent (0 = off)
    float chroma = 0.f;         // chromatic dispersion (0 = off)
    float specular = 0.f;       // specular glint (0 = off)
    glm::vec3 lightDir{0.5f, 1.f, 0.3f}; // glint light direction (world space, normalized on set)
    int shape = 0;              // 0 = sprite/disc mask, 1 = procedural polygon shard (3-5 sides)
    float dome = 0.f;           // dome normal strength: silhouette rim + glint spots (0 = flat facet)
};

// Built-in presets — fine params live here (in code), not in the config file.
namespace Refraction {
    RefractionSettings glass(); // slightly blue tint, mild chroma + glints
    RefractionSettings heat();  // noise mode, invisible lens
    RefractionSettings water(); // strong fresnel, blue tint
    RefractionSettings prism(); // full chromatic dispersion
}


class ParticleSystem;

std::unique_ptr<ParticleBackend> makeOpenGLBackend();
// Reject non-OpenGL backends before changing any state.
void setOpenGLPrograms(ParticleBackend&, Shader render, Shader simulate);
void setOpenGLRefractionInputs(ParticleBackend&, GLuint color, GLuint depth);
void setOpenGLSoftDepth(ParticleBackend&, GLuint depth);
// Whole-feature adapters: backend-agnostic parameters + native GL textures.
// Replacement programs must follow the uniform/binding contract documented in
// shaders/simulate.comp and shaders/particle.vert.
void setOpenGLRefraction(ParticleSystem&, const RefractionSettings&);
// A supplied depth texture is retained even when initially disabled.
void setOpenGLSoftParticles(ParticleSystem&, bool on, std::uint32_t sceneDepthTex = 0, float radius = 0.5f);
} // namespace ember
