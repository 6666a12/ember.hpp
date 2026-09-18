#include "ember/system.hpp"
#include "ember/config.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace ember {
namespace {
std::uint32_t forceBit(const std::string& name) {
    struct Entry { const char* name; std::uint32_t bit; };
    static const Entry kForces[] = {
        {"gravity", 1u << 0}, {"drag", 1u << 1}, {"wind", 1u << 2},
        {"turbulence", 1u << 3}, {"attractors", 1u << 4}, {"vortex", 1u << 5},
        {"spring", 1u << 6}, {"noise_wind", 1u << 7}, {"wave", 1u << 8},
        {"boundary", 1u << 9},
    };
    for (const auto& e : kForces)
        if (name == e.name) return e.bit;
    throw std::runtime_error("ember: unknown force '" + name + "' in forces list");
}

// Uniformly spaced keys: t_i = i/(n-1); n == 1 is constant; endpoints clamp.
// Both backends bake with this exact function so pixel comparisons line up.
glm::vec4 evalUniformColor(const std::vector<glm::vec4>& keys, float t) {
    if (keys.empty()) return glm::vec4(1.f);
    if (keys.size() == 1) return keys[0];
    const float u = std::max(0.f, std::min(1.f, t)) * float(keys.size() - 1);
    const std::size_t i = std::min<std::size_t>((std::size_t)u, keys.size() - 2);
    return glm::mix(keys[i], keys[i + 1], u - float(i));
}
float evalUniformSize(const std::vector<float>& keys, float t) {
    if (keys.empty()) return 1.f;
    if (keys.size() == 1) return keys[0];
    const float u = std::max(0.f, std::min(1.f, t)) * float(keys.size() - 1);
    const std::size_t i = std::min<std::size_t>((std::size_t)u, keys.size() - 2);
    return keys[i] + (keys[i + 1] - keys[i]) * (u - float(i));
}

}
ParticleSystem::ParticleSystem(const Settings& s, std::unique_ptr<ParticleBackend> backend)
    : backend_(std::move(backend)), capacity_(s.capacity),
      maxSpawnPerFrame_(s.maxSpawnPerFrame),
      frameSeed_(s.seed ? s.seed : std::random_device{}()),
      debug_(std::getenv("EMBER_DEBUG") != nullptr) {
    if(!backend_) throw std::invalid_argument("ember: null particle backend");
    if(capacity_==0 || capacity_>(1u<<30))
        throw std::invalid_argument("ember: capacity must be in [1, 2^30]");
    backend_->initialize(capacity_,debug_);
}
ParticleSystem::~ParticleSystem() = default;
ParticleSystem::ParticleSystem(ParticleSystem&&) noexcept = default;
ParticleSystem& ParticleSystem::operator=(ParticleSystem&&) noexcept = default;

void ParticleSystem::setGravity(glm::vec3 g) { simulation_.gravity = g; }

void ParticleSystem::setDrag(float k) { simulation_.drag = k; }

void ParticleSystem::setWind(glm::vec3 w) { simulation_.wind = w; }

void ParticleSystem::setTurbulence(float k) { simulation_.turbulence = k; }

void ParticleSystem::setBlendMode(BlendMode m) { rendering_.blend = m; }

void ParticleSystem::setDepthTest(bool on) { rendering_.depthTest = on; }

void ParticleSystem::setDepthWrite(bool on) { rendering_.depthWrite = on; }

void ParticleSystem::setMaxSpawnPerFrame(std::uint32_t n) {
    maxSpawnPerFrame_ = n; // spawn volume is budgeted in update(); no staging buffer to size
}

void ParticleSystem::setForceEnabled(Force f, bool on) {
    const auto index = static_cast<std::uint32_t>(f);
    if (index > static_cast<std::uint32_t>(Force::Boundary))
        throw std::invalid_argument("ember: invalid force");
    const std::uint32_t bit = 1u << index;
    if (on) simulation_.forceMask |= bit;
    else simulation_.forceMask &= ~bit;
}

void ParticleSystem::setForceMask(std::uint32_t mask) { simulation_.forceMask = mask; }

void ParticleSystem::setDragMode(DragMode m) { simulation_.dragMode = m; }

void ParticleSystem::setNoiseWind(glm::vec3 dir, float amplitude, float scale, float speed) {
    simulation_.noiseWindDir = dir;
    simulation_.noiseWindAmp = amplitude;
    simulation_.noiseWindScale = scale;
    simulation_.noiseWindSpeed = speed;
    setForceEnabled(Force::NoiseWind, amplitude != 0.f);
}

void ParticleSystem::setWave(glm::vec3 dir, glm::vec3 waveVector, float amplitude, float omega) {
    simulation_.waveDir = dir;
    simulation_.waveK = waveVector;
    simulation_.waveAmp = amplitude;
    simulation_.waveOmega = omega;
    setForceEnabled(Force::Wave, amplitude != 0.f);
}

void ParticleSystem::setBoundary(BoundaryMode mode, float planeY, float restitution) {
    simulation_.boundaryMode = mode;
    simulation_.boundaryY = planeY;
    simulation_.restitution = restitution;
    setForceEnabled(Force::Boundary, true);
}

void ParticleSystem::setSizeScale(float s, bool linkSpeed) {
    const float factor = (rendering_.sizeScale > 0.f && s > 0.f) ? s / rendering_.sizeScale : 1.f; // guard div-by-zero
    rendering_.sizeScale = s;
    if (linkSpeed && factor != 1.f) {
        speedScaleBase_ *= factor;
        for (auto& e : emitters_) e.speedScale *= factor;
    }
}

void ParticleSystem::setUseSprite(bool on) { rendering_.useSprite = on; }

void ParticleSystem::setSpriteSheet(int cols, int rows) {
    rendering_.sheetCols = std::max(1, cols);
    rendering_.sheetRows = std::max(1, rows);
}

void ParticleSystem::loadConfig(const char* path) {
    apply(Config::fromFile(path));
}

void ParticleSystem::apply(const Config& cfg) {
    // ------------------------------------------------------------------
    // Phase 1 — validate everything first, so a bad config throws before
    // any state is mutated (loadConfig stays atomic).
    // ------------------------------------------------------------------
    if (cfg.system.capacity > (1u << 30)) throw std::invalid_argument("ember: capacity exceeds supported range");
    std::uint32_t mask = 0;
    if (cfg.has("system.forces"))
        for (const auto& name : cfg.system.forces) mask |= forceBit(name);

    const DragMode dragMode = cfg.has("system.drag_mode")
        ? (cfg.system.dragMode == "quadratic" ? DragMode::Quadratic
           : cfg.system.dragMode == "linear"   ? DragMode::Linear
           : throw std::runtime_error("ember: unknown drag_mode '" + cfg.system.dragMode + "'"))
        : simulation_.dragMode;

    enum class BC { None, Kill, Bounce };
    const BC boundary = cfg.has("system.boundary_mode")
        ? (cfg.system.boundaryMode == "kill"    ? BC::Kill
           : cfg.system.boundaryMode == "bounce" ? BC::Bounce
           : cfg.system.boundaryMode == "none"   ? BC::None
           : throw std::runtime_error("ember: unknown boundary_mode '" + cfg.system.boundaryMode + "'"))
        : BC::None;

    const BlendMode blend = cfg.has("system.blend")
        ? (cfg.system.blend == "additive" ? BlendMode::Additive
           : cfg.system.blend == "normal" ? BlendMode::Normal
           : throw std::runtime_error("ember: unknown blend mode '" + cfg.system.blend + "'"))
        : rendering_.blend;

    if (cfg.has("system.refraction_mode") && cfg.system.refractionMode != "simple" &&
        cfg.system.refractionMode != "depth" && cfg.system.refractionMode != "noise")
        throw std::invalid_argument("ember: invalid refraction_mode");
    for (const auto& ec : cfg.emitters)
        if (!ec.paletteName.empty() && !cfg.findPalette(ec.paletteName.c_str()))
            throw std::runtime_error("ember: unknown palette '" + ec.paletteName +
                                     "' referenced by emitter '" + ec.name + "'");
    for (const auto& ec : cfg.events)
        if (!ec.paletteName.empty() && !cfg.findPalette(ec.paletteName.c_str()))
            throw std::runtime_error("ember: unknown palette '" + ec.paletteName +
                                     "' referenced by event '" + ec.name + "'");
    if (cfg.has("event") && cfg.events.size() > maxEventTemplates)
        throw std::invalid_argument("ember: too many event templates");
    // Event references resolve in two phases: collect every [event] name first,
    // then bind on_death/on_bounce (templates may chain to other templates).
    for (const auto& ec : cfg.events) {
        if (!ec.onDeathName.empty() && cfg.findEvent(ec.onDeathName.c_str()) < 0)
            throw std::runtime_error("ember: unknown event '" + ec.onDeathName +
                                     "' referenced by event '" + ec.name + "'");
        if (!ec.onBounceName.empty() && cfg.findEvent(ec.onBounceName.c_str()) < 0)
            throw std::runtime_error("ember: unknown event '" + ec.onBounceName +
                                     "' referenced by event '" + ec.name + "'");
    }
    for (const auto& ec : cfg.emitters) {
        if (!ec.onDeathName.empty() && cfg.findEvent(ec.onDeathName.c_str()) < 0)
            throw std::runtime_error("ember: unknown event '" + ec.onDeathName +
                                     "' referenced by emitter '" + ec.name + "'");
        if (!ec.onBounceName.empty() && cfg.findEvent(ec.onBounceName.c_str()) < 0)
            throw std::runtime_error("ember: unknown event '" + ec.onBounceName +
                                     "' referenced by emitter '" + ec.name + "'");
    }
    if (cfg.has("curves.color") && !cfg.colorKeys.empty() && !backend_->capabilities().lifeCurves)
        throw std::invalid_argument("ember: backend does not support life curves");
    if (cfg.has("curves.size") && !cfg.sizeKeys.empty() && !backend_->capabilities().lifeCurves)
        throw std::invalid_argument("ember: backend does not support life curves");
    backend_->validateConfiguration(cfg.system.capacity ? cfg.system.capacity : capacity_,
                                    cfg.has("system.sort") ? cfg.system.sort : sortEnabled());
    if (cfg.has("system.bloom") && cfg.system.bloom && !backend_->capabilities().bloom)
        throw std::invalid_argument("ember: backend does not support bloom");
    if (cfg.has("system.refraction") && cfg.system.refraction && !backend_->capabilities().refraction)
        throw std::invalid_argument("ember: backend does not support refraction");
    if (cfg.has("system.soft_particles") && cfg.system.softParticles && !backend_->capabilities().softParticles)
        throw std::invalid_argument("ember: backend does not support soft particles");
    if (cfg.has("system.texture") && !backend_->capabilities().spriteTextures)
        throw std::invalid_argument("ember: backend does not support sprite textures");
    if (cfg.has("system.soft_radius") && (!std::isfinite(cfg.system.softRadius) || cfg.system.softRadius <= 0.f))
        throw std::invalid_argument("ember: soft particle radius must be finite and positive");

    // ------------------------------------------------------------------
    // Phase 2 — apply. Only keys/sections actually present in the config
    // are touched; anything the config does not mention keeps its current
    // (programmatic) value.
    // ------------------------------------------------------------------
    if (cfg.system.capacity != 0 && cfg.system.capacity != capacity_) {
        backend_->resize(cfg.system.capacity);
        capacity_ = cfg.system.capacity;
    }
    if (cfg.system.maxSpawnPerFrame != 0) setMaxSpawnPerFrame(cfg.system.maxSpawnPerFrame);

    if (cfg.has("system.gravity")) setGravity(cfg.system.gravity);
    if (cfg.has("system.drag")) setDrag(cfg.system.drag);
    if (cfg.has("system.wind")) setWind(cfg.system.wind);
    if (cfg.has("system.turbulence")) setTurbulence(cfg.system.turbulence);
    if (cfg.has("system.drag_mode")) setDragMode(dragMode);

    if (cfg.has("system.noise_wind_direction") || cfg.has("system.noise_wind_amplitude") ||
        cfg.has("system.noise_wind_scale") || cfg.has("system.noise_wind_speed"))
        setNoiseWind(cfg.has("system.noise_wind_direction") ? cfg.system.noiseWindDir : simulation_.noiseWindDir,
                     cfg.has("system.noise_wind_amplitude") ? cfg.system.noiseWindAmp : simulation_.noiseWindAmp,
                     cfg.has("system.noise_wind_scale") ? cfg.system.noiseWindScale : simulation_.noiseWindScale,
                     cfg.has("system.noise_wind_speed") ? cfg.system.noiseWindSpeed : simulation_.noiseWindSpeed);
    if (cfg.has("system.wave_direction") || cfg.has("system.wave_vector") ||
        cfg.has("system.wave_amplitude") || cfg.has("system.wave_omega"))
        setWave(cfg.has("system.wave_direction") ? cfg.system.waveDir : simulation_.waveDir,
                cfg.has("system.wave_vector") ? cfg.system.waveK : simulation_.waveK,
                cfg.has("system.wave_amplitude") ? cfg.system.waveAmp : simulation_.waveAmp,
                cfg.has("system.wave_omega") ? cfg.system.waveOmega : simulation_.waveOmega);

    const float boundaryY = cfg.has("system.boundary_y") ? cfg.system.boundaryY : simulation_.boundaryY;
    const float restitution = cfg.has("system.restitution") ? cfg.system.restitution : simulation_.restitution;
    if (cfg.has("system.boundary_mode")) {
        if (boundary == BC::Kill) setBoundary(BoundaryMode::Kill, boundaryY, restitution);
        else if (boundary == BC::Bounce) setBoundary(BoundaryMode::Bounce, boundaryY, restitution);
        else disableForce(Force::Boundary);
    } else if (cfg.has("system.boundary_y") || cfg.has("system.restitution")) {
        simulation_.boundaryY = boundaryY; // param-only update, mask untouched
        simulation_.restitution = restitution;
    }

    if (cfg.has("system.blend")) setBlendMode(blend);
    if (cfg.has("system.debug")) { debug_ = cfg.system.debug; backend_->setDebug(debug_); }
    if (cfg.has("system.size_scale")) setSizeScale(cfg.system.sizeScale, cfg.system.scaleSpeedWithSize);
    if (cfg.has("system.texture"))
        setSpriteTexture(cfg.system.texture.c_str());
    if (cfg.has("system.streak")) setStreak(cfg.system.streak);
    if (cfg.has("system.soft_particles")) {
        rendering_.softParticles = cfg.system.softParticles; // scene depth stays host-provided
    }
    if (cfg.has("system.soft_radius")) rendering_.softRadius = cfg.system.softRadius;
    if (cfg.has("system.bloom")) setBloom(cfg.system.bloom);
    if (cfg.has("system.bloom_threshold")) setBloomThreshold(cfg.system.bloomThreshold);
    if (cfg.has("system.sort")) setSortEnabled(cfg.system.sort);
    if (cfg.has("system.spin")) setSpin(cfg.system.spin);
    if (cfg.has("system.refraction")) rendering_.refraction.enabled = cfg.system.refraction;
    if (cfg.has("system.refraction_mode"))
        rendering_.refraction.mode = cfg.system.refractionMode == "depth" ? 1
                         : cfg.system.refractionMode == "noise" ? 2 : 0;
    if (cfg.has("system.refraction_strength")) rendering_.refraction.strength = cfg.system.refractionStrength;

    // Lifecycle curves: resample explicit-t INI keys into 64 uniform values,
    // then hand them to the same setter the programmatic API uses.
    if (cfg.has("curves.color")) {
        std::vector<glm::vec4> uniform;
        if (!cfg.colorKeys.empty()) {
            uniform.resize(kLifeCurveLutSize);
            const auto& keys = cfg.colorKeys;
            for (std::uint32_t j = 0; j < kLifeCurveLutSize; ++j) {
                const float t = float(j) / float(kLifeCurveLutSize - 1);
                if (t <= keys.front().t) uniform[j] = keys.front().value;
                else if (t >= keys.back().t) uniform[j] = keys.back().value;
                else {
                    std::size_t i = 0;
                    while (i + 1 < keys.size() && keys[i + 1].t < t) ++i;
                    const float f = (t - keys[i].t) / (keys[i + 1].t - keys[i].t);
                    uniform[j] = glm::mix(keys[i].value, keys[i + 1].value, f);
                }
            }
        }
        setColorOverLife(std::move(uniform));
    }
    if (cfg.has("curves.size")) {
        std::vector<float> uniform;
        if (!cfg.sizeKeys.empty()) {
            uniform.resize(kLifeCurveLutSize);
            const auto& keys = cfg.sizeKeys;
            for (std::uint32_t j = 0; j < kLifeCurveLutSize; ++j) {
                const float t = float(j) / float(kLifeCurveLutSize - 1);
                if (t <= keys.front().t) uniform[j] = keys.front().value.x;
                else if (t >= keys.back().t) uniform[j] = keys.back().value.x;
                else {
                    std::size_t i = 0;
                    while (i + 1 < keys.size() && keys[i + 1].t < t) ++i;
                    const float f = (t - keys[i].t) / (keys[i + 1].t - keys[i].t);
                    uniform[j] = keys[i].value.x + (keys[i + 1].value.x - keys[i].value.x) * f;
                }
            }
        }
        setSizeOverLife(std::move(uniform));
    }

    if (cfg.has("event")) {
        clearEventEmitters();
        for (const auto& ec : cfg.events) {
            Emitter e = ec;
            if (!ec.paletteName.empty()) // validated in phase 1, so non-null
                e.palette = cfg.findPalette(ec.paletteName.c_str())->colors;
            if (!ec.onDeathName.empty()) e.onDeath = cfg.findEvent(ec.onDeathName.c_str());
            if (!ec.onBounceName.empty()) e.onBounce = cfg.findEvent(ec.onBounceName.c_str());
            addEventEmitter(e);
        }
    }
    if (cfg.has("emitter")) {
        clearEmitters();
        for (const auto& ec : cfg.emitters) {
            Emitter e = ec; // slices EmitterConfig -> Emitter (drops name/paletteName)
            if (!ec.paletteName.empty()) // validated in phase 1, so non-null
                e.palette = cfg.findPalette(ec.paletteName.c_str())->colors;
            if (!ec.onDeathName.empty()) e.onDeath = cfg.findEvent(ec.onDeathName.c_str());
            if (!ec.onBounceName.empty()) e.onBounce = cfg.findEvent(ec.onBounceName.c_str());
            addEmitter(e);
        }
    }
    if (cfg.has("attractor")) setAttractors(cfg.attractors);
    if (cfg.has("vortex")) setVortexes(cfg.vortexes);
    if (cfg.has("spring")) setSprings(cfg.springs);

    // An explicit forces list is the final word on the mask (overrides the
    // auto-enables from the parameter setters above).
    if (cfg.has("system.forces")) setForceMask(mask);
}

Emitter& ParticleSystem::addEmitter(const Emitter& e) {
    if (e.onDeath < -1 || e.onDeath >= (int)eventEmitters_.size() ||
        e.onBounce < -1 || e.onBounce >= (int)eventEmitters_.size())
        throw std::invalid_argument("ember: event reference out of range");
    emitters_.push_back(e);
    // Inherit the accumulated size<->speed link factor (setSizeScale(.., true)).
    emitters_.back().speedScale *= speedScaleBase_;
    registerEmitterPalette(emitters_.back());
    return emitters_.back();
}

std::size_t ParticleSystem::addEventEmitter(const Emitter& e) {
    if (eventEmitters_.size() >= maxEventTemplates)
        throw std::invalid_argument("ember: too many event templates");
    if (!std::isfinite(e.inheritVelocity))
        throw std::invalid_argument("ember: event inheritVelocity must be finite");
    Emitter ev = e;
    ev.eventCount = std::max(1u, std::min(ev.eventCount, 64u));
    eventEmitters_.push_back(ev);
    registerEmitterPalette(eventEmitters_.back());
    eventTemplatesDirty_ = true;
    return eventEmitters_.size() - 1;
}

void ParticleSystem::clearEventEmitters() {
    eventEmitters_.clear();
    for (auto& e : emitters_) { e.onDeath = -1; e.onBounce = -1; }
    eventTemplatesDirty_ = false;
    rebuildPaletteBuffer();
    backend_->uploadEventTemplates({}); // release backend resources; empty never throws
}

void ParticleSystem::removeEmitter(std::size_t index) {
    if (index < emitters_.size()) emitters_.erase(emitters_.begin() + (std::ptrdiff_t)index);
    rebuildPaletteBuffer(); // palette ranges shift; re-upload from the live emitters
}

void ParticleSystem::clearEmitters() {
    emitters_.clear();
    rebuildPaletteBuffer();
}

void ParticleSystem::registerEmitterPalette(Emitter& e) {
    e.paletteIdx = -1;
    e.paletteCount = 0;
    if (!e.palette.empty()) {
        e.paletteIdx = (int)paletteData_.size();
        e.paletteCount = (int)e.palette.size();
        paletteData_.insert(paletteData_.end(), e.palette.begin(), e.palette.end());
        backend_->uploadPalette(paletteData_);
    }
}

void ParticleSystem::rebuildPaletteBuffer() {
    paletteData_.clear();
    auto append = [&](std::vector<Emitter>& list) {
        for (auto& e : list) {
            e.paletteIdx = e.palette.empty() ? -1 : (int)paletteData_.size();
            e.paletteCount = (int)e.palette.size();
            paletteData_.insert(paletteData_.end(), e.palette.begin(), e.palette.end());
        }
    };
    append(emitters_);
    append(eventEmitters_);
    backend_->uploadPalette(paletteData_);
    // Template requests bake paletteIdx; a rebuilt layout invalidates the
    // encoded copies the backend holds, so re-upload on the next update.
    eventTemplatesDirty_ = true;
}

void ParticleSystem::synchronizePalettes() {
    std::size_t offset = 0;
    auto matches = [&](const std::vector<Emitter>& list) {
        for (const auto& e : list) {
            if (e.paletteIdx != (e.palette.empty() ? -1 : (int)offset) ||
                e.paletteCount != (int)e.palette.size() || offset + e.palette.size() > paletteData_.size() ||
                !std::equal(e.palette.begin(), e.palette.end(), paletteData_.begin() + offset))
                return false;
            offset += e.palette.size();
        }
        return true;
    };
    if (!matches(emitters_) || !matches(eventEmitters_) || offset != paletteData_.size())
        rebuildPaletteBuffer();
}

void ParticleSystem::synchronizeEventTemplates() {
    if (!eventTemplatesDirty_) return;
    std::vector<SpawnRequest> templates;
    templates.reserve(eventEmitters_.size());
    for (const auto& e : eventEmitters_) templates.push_back(e.makeRequest(0, e.eventCount));
    backend_->uploadEventTemplates(templates);
    eventTemplatesDirty_ = false;
}

void ParticleSystem::burst(const BurstParams& b) {
    // Encode as a single GPU spawn request (the shader samples the particles).
    SpawnRequest r{};
    r.colorMin = b.colorMin;
    r.colorMax = b.colorMax;
    r.position = b.position;
    r.dir = glm::vec3(0.f, 1.f, 0.f); // spread mixes away from +Y (matches the old CPU path)
    r.spread = b.spread;
    r.speedMin = b.speedMin;
    r.speedMax = b.speedMax;
    r.lifeMin = b.lifeMin;
    r.lifeMax = b.lifeMax;
    r.sizeMin = b.sizeMin;
    r.sizeMax = b.sizeMax;
    r.shape = (int)Emitter::Shape::Point;
    r.paletteIdx = -1;
    r.flags = b.refractive ? 2 : 0; // bit1 = refractive (sign-encoded in pos.w)
    r.count = std::min(b.count, maxSpawnPerFrame_);
    pendingRequests_.push_back(r);
    // Keep the pending queue bounded; drop the oldest overflow.
    if (pendingRequests_.size() > maxSpawnRequests) {
        pendingRequests_.erase(pendingRequests_.begin(),
                               pendingRequests_.begin() +
                                   (std::ptrdiff_t)(pendingRequests_.size() - maxSpawnRequests));
    }
}

void ParticleSystem::setStreak(float k) { rendering_.streak = k; }

void ParticleSystem::setSpin(float speed) { rendering_.spin = speed; }

void ParticleSystem::setBloomThreshold(float t) { rendering_.bloomThreshold = t; }

void ParticleSystem::uploadLifeCurves() {
    LifeCurvesLut lut{};
    lut.mask = (colorOverLifeKeys_.empty() ? 0u : 1u) | (sizeOverLifeKeys_.empty() ? 0u : 2u);
    for (std::uint32_t i = 0; i < kLifeCurveLutSize; ++i) {
        const float t = float(i) / float(kLifeCurveLutSize - 1);
        lut.color[i] = evalUniformColor(colorOverLifeKeys_, t);
        lut.size[i] = evalUniformSize(sizeOverLifeKeys_, t);
    }
    backend_->uploadLifeCurves(lut);
}

void ParticleSystem::setColorOverLife(std::vector<glm::vec4> keys) {
    if (!keys.empty() && !backend_->capabilities().lifeCurves)
        throw std::invalid_argument("ember: backend does not support life curves");
    for (const auto& k : keys)
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(k[c])) throw std::invalid_argument("ember: life curve keys must be finite");
    colorOverLifeKeys_ = std::move(keys);
    uploadLifeCurves();
}

void ParticleSystem::setSizeOverLife(std::vector<float> keys) {
    if (!keys.empty() && !backend_->capabilities().lifeCurves)
        throw std::invalid_argument("ember: backend does not support life curves");
    for (float k : keys)
        if (!std::isfinite(k)) throw std::invalid_argument("ember: life curve keys must be finite");
    sizeOverLifeKeys_ = std::move(keys);
    uploadLifeCurves();
}

void ParticleSystem::setAttractors(const std::vector<Attractor>& a) {
    simulation_.attractors = a;
    backend_->uploadAttractors(a);
}

void ParticleSystem::setVortexes(const std::vector<Vortex>& v) {
    simulation_.vortexes = v;
    backend_->uploadVortexes(v);
    setForceEnabled(Force::Vortex, !v.empty());
}

void ParticleSystem::setSprings(const std::vector<Spring>& s) {
    simulation_.springs = s;
    backend_->uploadSprings(s);
    setForceEnabled(Force::Spring, !s.empty());
}

void ParticleSystem::update(float dt) {
    if (!std::isfinite(dt) || dt < 0.f) throw std::invalid_argument("ember: dt must be finite and nonnegative");
    synchronizePalettes();
    synchronizeEventTemplates();
    dt = std::min(dt, 0.1f); // clamp for stability; variable dt is fine below this
    time_ += dt;

    // 1) Encode spawn requests: pending bursts first, then emitters (capped
    //    total). The GPU samples the particles; the CPU only accumulates the
    //    per-emitter counts (rate*dt) and assigns prefix offsets.
    std::vector<SpawnRequest> reqs;
    reqs.reserve(pendingRequests_.size() + emitters_.size());
    std::uint32_t total = 0;
    const std::uint32_t budget = maxSpawnPerFrame_;
    for (const auto& r : pendingRequests_) {
        if (reqs.size() == maxSpawnRequests) break;
        const std::uint32_t cnt = std::min(r.count, budget - total);
        if (cnt > 0) {
            SpawnRequest rr = r;
            rr.base = total;
            rr.count = cnt;
            reqs.push_back(rr);
            total += cnt;
        }
    }
    pendingRequests_.clear();
    for (auto& e : emitters_) {
        if (reqs.size() == maxSpawnRequests) break;
        const std::uint32_t cnt = e.takeCount(dt, budget - total);
        if (cnt > 0) {
            reqs.push_back(e.makeRequest(total, cnt));
            total += cnt;
        }
    }

    backend_->update(simulation_, UpdateBatch{reqs,total,dt,time_,frameSeed_++});
}

void ParticleSystem::setRefractionParameters(const RefractionParameters& s) {
    if (!std::isfinite(s.ior) || s.ior <= 0.f || s.mode < 0 || s.mode > 2 ||
        !std::isfinite(s.strength))
        throw std::invalid_argument("ember: invalid refraction index, mode or strength");
    if(s.enabled && !backend_->capabilities().refraction)
        throw std::invalid_argument("ember: backend does not support refraction");
    rendering_.refraction = s;
    // Keep lightDir unit-length: pow(dot, 24) explodes for |L| > 1 and turns
    // whole shards into blown-out white blobs.
    const float len = glm::length(rendering_.refraction.lightDir);
    if (len > 1e-6f) rendering_.refraction.lightDir /= len;
}

void ParticleSystem::setSoftParticleParameters(bool on, float radius) {
    if (!std::isfinite(radius) || radius <= 0.f)
        throw std::invalid_argument("ember: soft particle radius must be finite and positive");
    if(on && !backend_->capabilities().softParticles)
        throw std::invalid_argument("ember: backend does not support soft particles");
    rendering_.softParticles=on;
    rendering_.softRadius=radius;
}
void ParticleSystem::clear() { backend_->clear(); pendingRequests_.clear(); }
void ParticleSystem::setGpuDriven(bool on) { backend_->setGpuDriven(on); }
void ParticleSystem::setSortEnabled(bool on) { backend_->setSortEnabled(on); }
void ParticleSystem::setBloom(bool on) { backend_->setBloom(on); }
void ParticleSystem::setSpriteTexture(const char* path) { backend_->setSpriteTexture(path); }
void ParticleSystem::setShaderDirectory(const char* path) { backend_->setShaderDirectory(path); }
std::uint32_t ParticleSystem::aliveCount() const { return backend_->synchronizeStatistics().alive; }
ParticleSystem::Statistics ParticleSystem::pollStatistics() { return backend_->pollStatistics(); }
ParticleSystem::Statistics ParticleSystem::synchronizeStatistics() const { return backend_->synchronizeStatistics(); }
std::vector<Particle> ParticleSystem::readParticles(std::uint32_t max) const { return backend_->readParticles(max); }
void ParticleSystem::render(const glm::mat4& view, const glm::mat4& proj, float w, float h, float fov) {
    backend_->render(rendering_, RenderView{view,proj,w,h,fov});
}
} // namespace ember
