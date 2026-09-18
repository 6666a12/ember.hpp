#include "ember/system.hpp"
#include "particle_backend.hpp"
#include <utility>

namespace ember {
namespace {
detail::opengl::OpenGLBackend& requireOpenGL(ParticleBackend& backend) {
    auto* gl=dynamic_cast<detail::opengl::OpenGLBackend*>(&backend);
    if(!gl) throw std::invalid_argument("ember: this operation requires the OpenGL backend");
    return *gl;
}
}
std::unique_ptr<ParticleBackend> makeOpenGLBackend() {
    return std::make_unique<detail::opengl::OpenGLBackend>();
}
ParticleSystem::ParticleSystem(const Settings& s) : ParticleSystem(s,makeOpenGLBackend()) {}
void setOpenGLPrograms(ParticleBackend& backend,Shader render,Shader simulate) {
    requireOpenGL(backend).setPrograms(std::move(render),std::move(simulate));
}
void setOpenGLRefractionInputs(ParticleBackend& backend,GLuint color,GLuint depth) {
    requireOpenGL(backend).setRefractionInputs(color,depth);
}
void setOpenGLSoftDepth(ParticleBackend& backend,GLuint depth) {
    requireOpenGL(backend).setSoftDepth(depth);
}
void setOpenGLRefraction(ParticleSystem& sys,const RefractionSettings& settings) {
    auto& gl=requireOpenGL(sys.backend());
    RefractionParameters p;
    p.enabled=settings.enabled;
    p.mode=settings.mode;
    p.strength=settings.strength;
    p.ior=settings.ior;
    p.tint=settings.tint;
    p.absorption=settings.absorption;
    p.fresnel=settings.fresnel;
    p.chroma=settings.chroma;
    p.specular=settings.specular;
    p.lightDir=settings.lightDir;
    p.shape=settings.shape;
    p.dome=settings.dome;
    sys.setRefractionParameters(p);
    gl.setRefractionInputs(settings.sceneColorTex,settings.sceneDepthTex);
}
void setOpenGLSoftParticles(ParticleSystem& sys,bool on,std::uint32_t sceneDepthTex,float radius) {
    if(on || sceneDepthTex!=0) {
        sys.setSoftParticleParameters(on,radius);
        requireOpenGL(sys.backend()).setSoftDepth(sceneDepthTex);
    } else sys.setSoftParticleParameters(false,radius);
}
namespace Refraction {
RefractionSettings glass() {
    RefractionSettings s;
    s.tint = {0.95f, 0.97f, 1.f};
    s.absorption = 0.35f;
    s.fresnel = 2.f;
    s.chroma = 0.15f;
    s.specular = 0.4f;
    s.shape = 1;   // polygon shards (3-5 sides, jittered)
    s.dome = 1.3f; // curved-facet lighting: visible rim, glint spots
    return s;
}
RefractionSettings heat() {
    RefractionSettings s;
    s.mode = 2; // noise offset: invisible lens
    s.strength = 0.015f;
    s.absorption = 1.f; // full displaced scene: mix(original, refr, 0) would be invisible
    s.fresnel = 0.f;
    return s;
}
RefractionSettings water() {
    RefractionSettings s;
    s.tint = {0.8f, 0.9f, 1.f};
    s.absorption = 0.5f;
    s.fresnel = 4.f;
    s.dome = 2.2f; // droplet-like: tight bright rim
    return s;
}
RefractionSettings prism() {
    RefractionSettings s;
    s.chroma = 1.0f;
    s.absorption = 0.5f;
    s.specular = 0.2f;
    s.shape = 1;   // polygon shards
    s.dome = 0.8f;
    return s;
}
} // namespace Refraction


} // namespace ember
