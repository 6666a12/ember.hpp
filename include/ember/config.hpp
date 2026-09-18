#pragma once

// INI-style configuration with a self-contained parser (no third-party JSON).
//
// Users can edit gravity, wind, turbulence, emitters, palettes and attractors
// without touching code; ParticleSystem::loadConfig() / apply() consume this.
//
// Format (see config/example.ini):
//   # comment
//   [system]                 gravity = 0, -7, 0        drag = 0.05   ...
//   [palette "fire"]         colors = 1,0.9,0.4,1 | 1,0.15,0.05,1
//   [emitter "fountain"]     shape = cone    position = 0, 0, 0   ...
//   [attractor]              position = 0, 0.6, 0    strength = 60
//
// Errors throw std::runtime_error with a line number.

#include "ember/emitters.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <limits>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ember {

struct Palette {
    std::string name;
    std::vector<glm::vec4> colors;
};

// Mirrors Emitter + configuration metadata (section name / referenced palette).
// Explicit-time lifecycle-curve key (WO-09): t in [0,1], value = rgba (color)
// or .x = size. The facade resamples these to uniform 64-entry LUTs.
struct CurveKey {
    float t = 0.f;
    glm::vec4 value{1.f};
};

struct EmitterConfig : public Emitter {
    std::string name;
    std::string paletteName;  // optional: [palette "..."] to take colors from
    std::string onDeathName;  // [emitter]/[event] on_death = <event name>
    std::string onBounceName; // [emitter]/[event] on_bounce = <event name>
};

struct Config {
    struct System {
        glm::vec3 gravity{0.f, -9.81f, 0.f};
        float drag = 0.f;
        glm::vec3 wind{0.f};
        float turbulence = 0.f;
        std::string blend = "additive";   // additive | normal
        std::uint32_t capacity = 0;       // 0 = keep current
        std::uint32_t maxSpawnPerFrame = 0;

        // ---- force switches & new force fields ----
        std::vector<std::string> forces;  // enabled forces; empty = auto (setters decide)
        std::string dragMode = "linear";  // linear | quadratic
        glm::vec3 noiseWindDir{1.f, 0.f, 0.f};
        float noiseWindAmp = 0.f;         // 0 = off
        float noiseWindScale = 0.4f;
        float noiseWindSpeed = 0.8f;
        glm::vec3 waveDir{1.f, 0.f, 0.f};
        glm::vec3 waveK{1.f, 0.f, 0.f};   // wave vector (radians/unit)
        float waveAmp = 0.f;              // 0 = off
        float waveOmega = 1.f;
        std::string boundaryMode = "none"; // none | kill | bounce
        float boundaryY = 0.f;
        float restitution = 0.5f;
        bool debug = false;               // per-frame diagnostics to stderr

        // ---- size / texture / editor ----
        float sizeScale = 1.f;            // global particle size multiplier
        bool scaleSpeedWithSize = true;   // keep spawn speed proportional to size_scale
        std::string texture;              // optional sprite PNG path ("" = built-in gradient)
        bool editor = true;               // examples: interactive emitter editor (runtime toggle)

        // ---- rendering extras ----
        float streak = 0.f;               // >0: centered stretch along projected particle motion
        bool softParticles = false;       // fade particles near host scene depth
        float softRadius = 0.5f;
        bool bloom = false;               // additive HDR bloom post-process
        float bloomThreshold = 1.f;
        bool sort = false;                // GPU depth sort (OIT: correct alpha blending)
        float spin = 0.f;                 // billboard spin speed in rad/s (0 = off)
        bool refraction = false;          // refractive particles (glass; host supplies scene texture)
        std::string refractionMode = "simple"; // simple | depth | noise
        float refractionStrength = 0.02f; // refraction offset strength
    };
    System system;
    std::vector<Palette> palettes;
    std::vector<EmitterConfig> emitters;
    std::vector<EmitterConfig> events;
    std::vector<CurveKey> colorKeys; // [curves] color: value = rgba
    std::vector<CurveKey> sizeKeys;  // [curves] size:  value.x = size
    std::vector<Attractor> attractors;
    std::vector<Vortex> vortexes;
    std::vector<Spring> springs;

    // Keys/sections that actually appeared in the parsed text (e.g.
    // "system.gravity", "vortex", "emitter"). Lets apply() keep programmatic
    // state for anything the config does not mention.
    std::vector<std::string> specified;

    bool has(const char* key) const {
        return std::find(specified.begin(), specified.end(), key) != specified.end();
    }

    const Palette* findPalette(const char* name) const {
        for (const auto& p : palettes)
            if (p.name == name) return &p;
        return nullptr;
    }

    // Event template index by [event] name, or -1 when absent.
    int findEvent(const char* name) const {
        for (std::size_t i = 0; i < events.size(); ++i)
            if (events[i].name == name) return (int)i;
        return -1;
    }

    static Config fromFile(const char* path) {
        std::ifstream f(path);
        if (!f) throw std::runtime_error(std::string("ember config: cannot open '") + path + "'");
        std::ostringstream ss;
        char buffer[4096];
        while (f.read(buffer, sizeof(buffer))) ss.write(buffer, f.gcount());
        ss.write(buffer, f.gcount());
        if (f.bad() || !f.eof() || !ss)
            throw std::runtime_error(std::string("ember config: read failed: ") + path);
        return fromString(ss.str());
    }

    static Config fromString(const std::string& text) {
        Config cfg;
        std::istringstream in(text);
        std::string line;
        int lineNo = 0;

        enum class Section { None, System, Palette, Emitter, Event, Curves, Attractor, Vortex, Spring };
        Section section = Section::None;
        Palette* pal = nullptr;
        EmitterConfig* em = nullptr;

        auto fail = [&](const std::string& msg) -> void {
            throw std::runtime_error("ember config: " + msg + " (line " + std::to_string(lineNo) + ")");
        };

        while (std::getline(in, line)) {
            ++lineNo;
            // Strip a UTF-8 BOM (common when files are saved by Windows Notepad).
            if (lineNo == 1 && line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
                line.erase(0, 3);
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;

            if (line[0] == '[') { // new section
                if (line.back() != ']') fail("malformed section header '" + line + "'");
                const std::string body = trim(line.substr(1, line.size() - 2));
                const std::string lower = toLower(body);
                const std::string sec = lower.substr(0, lower.find_first_of(" \t")); // section keyword
                if (sec == "system") {
                    section = Section::System;
                } else if (sec == "curves") {
                    section = Section::Curves;
                } else if (sec == "attractor") {
                    cfg.attractors.push_back(Attractor{});
                    cfg.specified.push_back("attractor");
                    section = Section::Attractor;
                } else if (sec == "vortex" || sec == "spring") {
                    if (sec == "vortex") {
                        cfg.vortexes.push_back(Vortex{});
                        cfg.specified.push_back("vortex");
                        section = Section::Vortex;
                    } else {
                        cfg.springs.push_back(Spring{});
                        cfg.specified.push_back("spring");
                        section = Section::Spring;
                    }
                } else if (sec == "palette" || sec == "emitter" || sec == "event") {
                    std::string arg;
                    try { arg = sectionArg(body); }
                    catch (const std::invalid_argument&) { fail("malformed named section: " + body); }
                    if (sec == "palette") {
                        for (auto& p : cfg.palettes)
                            if (p.name == arg) fail("duplicate palette '" + arg + "'");
                        cfg.palettes.push_back(Palette{arg, {}});
                        pal = &cfg.palettes.back();
                        section = Section::Palette;
                    } else if (sec == "event") {
                        cfg.events.push_back(EmitterConfig{});
                        cfg.specified.push_back("event");
                        em = &cfg.events.back();
                        em->name = arg;
                        section = Section::Event;
                    } else {
                        cfg.emitters.push_back(EmitterConfig{});
                        cfg.specified.push_back("emitter");
                        em = &cfg.emitters.back();
                        em->name = arg;
                        section = Section::Emitter;
                    }
                } else {
                    fail("unknown section '" + body + "'");
                }
                continue;
            }

            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) fail("expected 'key = value', got '" + line + "'");
            const std::string key = toLower(trim(line.substr(0, eq)));
            const std::string value = trim(line.substr(eq + 1));
            // Explicitly empty values are allowed for texture (built-in sprite)
            // and the curves channels (disable the curve).
            const bool emptyAllowed = (section == Section::System && key == "texture") ||
                                      (section == Section::Curves && (key == "size" || key == "color"));
            if (key.empty() || (value.empty() && !emptyAllowed))
                fail("empty key or value in '" + line + "'");

            // Record key presence so apply() can keep untouched state.
            const char* secName = section == Section::System ? "system"
                : section == Section::Palette ? "palette"
                : section == Section::Emitter ? "emitter"
                : section == Section::Event ? "event"
                : section == Section::Curves ? "curves"
                : section == Section::Attractor ? "attractor"
                : section == Section::Vortex ? "vortex"
                : section == Section::Spring ? "spring"
                : "none";
            if (section != Section::None) cfg.specified.push_back(std::string(secName) + "." + key);

            switch (section) {
                case Section::System: {
                    if (key == "gravity") cfg.system.gravity = parseVec3(value, lineNo);
                    else if (key == "drag") cfg.system.drag = parseFloat(value, lineNo);
                    else if (key == "wind") cfg.system.wind = parseVec3(value, lineNo);
                    else if (key == "turbulence") cfg.system.turbulence = parseFloat(value, lineNo);
                    else if (key == "blend") cfg.system.blend = toLower(value);
                    else if (key == "capacity") cfg.system.capacity = parseUInt(value, lineNo);
                    else if (key == "max_spawn_per_frame") cfg.system.maxSpawnPerFrame = parseUInt(value, lineNo);
                    else if (key == "forces") {
                        for (auto& tok : split(value, ',')) {
                            const std::string f = toLower(trim(tok));
                            if (!f.empty()) cfg.system.forces.push_back(f);
                        }
                    }
                    else if (key == "drag_mode") cfg.system.dragMode = toLower(value);
                    else if (key == "noise_wind_direction") cfg.system.noiseWindDir = parseVec3(value, lineNo);
                    else if (key == "noise_wind_amplitude") cfg.system.noiseWindAmp = parseFloat(value, lineNo);
                    else if (key == "noise_wind_scale") cfg.system.noiseWindScale = parseFloat(value, lineNo);
                    else if (key == "noise_wind_speed") cfg.system.noiseWindSpeed = parseFloat(value, lineNo);
                    else if (key == "wave_direction") cfg.system.waveDir = parseVec3(value, lineNo);
                    else if (key == "wave_vector") cfg.system.waveK = parseVec3(value, lineNo);
                    else if (key == "wave_amplitude") cfg.system.waveAmp = parseFloat(value, lineNo);
                    else if (key == "wave_omega") cfg.system.waveOmega = parseFloat(value, lineNo);
                    else if (key == "boundary_mode") cfg.system.boundaryMode = toLower(value);
                    else if (key == "boundary_y") cfg.system.boundaryY = parseFloat(value, lineNo);
                    else if (key == "restitution") cfg.system.restitution = parseFloat(value, lineNo);
                    else if (key == "debug") cfg.system.debug = parseBool(value, lineNo);
                    else if (key == "size_scale") cfg.system.sizeScale = parseFloat(value, lineNo);
                    else if (key == "scale_speed_with_size") cfg.system.scaleSpeedWithSize = parseBool(value, lineNo);
                    else if (key == "texture") cfg.system.texture = value;
                    else if (key == "editor") cfg.system.editor = parseBool(value, lineNo);
                    else if (key == "streak") cfg.system.streak = parseFloat(value, lineNo);
                    else if (key == "soft_particles") cfg.system.softParticles = parseBool(value, lineNo);
                    else if (key == "soft_radius") cfg.system.softRadius = parseFloat(value, lineNo);
                    else if (key == "bloom") cfg.system.bloom = parseBool(value, lineNo);
                    else if (key == "bloom_threshold") cfg.system.bloomThreshold = parseFloat(value, lineNo);
                    else if (key == "sort") cfg.system.sort = parseBool(value, lineNo);
                    else if (key == "spin") cfg.system.spin = parseFloat(value, lineNo);
                    else if (key == "refraction") cfg.system.refraction = parseBool(value, lineNo);
                    else if (key == "refraction_mode") {
                        cfg.system.refractionMode = toLower(value);
                        if (cfg.system.refractionMode != "simple" && cfg.system.refractionMode != "depth" && cfg.system.refractionMode != "noise")
                            fail("unknown refraction_mode: " + value);
                    }
                    else if (key == "refraction_strength") cfg.system.refractionStrength = parseFloat(value, lineNo);
                    else fail("unknown key '" + key + "' in [system]");
                    break;
                }
                case Section::Palette: {
                    if (key == "colors") {
                        for (auto& tok : split(value, '|'))
                            pal->colors.push_back(parseVec4(trim(tok), lineNo));
                        if (pal->colors.empty()) fail("palette '" + pal->name + "' has no colors");
                    } else fail("unknown key '" + key + "' in [palette]");
                    break;
                }
                case Section::Emitter:
                case Section::Event: {
                    if (key == "shape") em->shape = parseShape(value, lineNo);
                    else if (key == "position") em->position = parseVec3(value, lineNo);
                    else if (key == "base_velocity") em->baseVelocity = parseVec3(value, lineNo);
                    else if (key == "extents") em->extents = parseVec3(value, lineNo);
                    else if (key == "axis") em->axis = parseVec3(value, lineNo);
                    else if (key == "radius") em->radius = parseFloat(value, lineNo);
                    else if (key == "rate") em->rate = parseFloat(value, lineNo);
                    else if (key == "speed_min") em->speedMin = parseFloat(value, lineNo);
                    else if (key == "speed_max") em->speedMax = parseFloat(value, lineNo);
                    else if (key == "spread") em->spread = parseFloat(value, lineNo);
                    else if (key == "life_min") em->lifeMin = parseFloat(value, lineNo);
                    else if (key == "life_max") em->lifeMax = parseFloat(value, lineNo);
                    else if (key == "size_min") em->sizeMin = parseFloat(value, lineNo);
                    else if (key == "size_max") em->sizeMax = parseFloat(value, lineNo);
                    else if (key == "color_min") em->colorMin = parseVec4(value, lineNo);
                    else if (key == "color_max") em->colorMax = parseVec4(value, lineNo);
                    else if (key == "palette") em->paletteName = value;
                    else if (key == "active") em->active = parseBool(value, lineNo);
                    else if (key == "refractive") em->refractive = parseBool(value, lineNo);
                    else if (key == "cone_angle") em->coneAngle = parseFloat(value, lineNo);
                    else if (key == "speed_size_link") em->speedSizeLink = parseFloat(value, lineNo);
                    else if (key == "speed_scale") em->speedScale = parseFloat(value, lineNo);
                    else if (key == "fade_color_min") {
                        em->fadeColorMin = parseVec3(value, lineNo);
                        em->hasFadeColor = true;
                    }
                    else if (key == "fade_color_max") {
                        em->fadeColorMax = parseVec3(value, lineNo);
                        em->hasFadeColor = true;
                    }
                    else if (key == "on_death") em->onDeathName = value;
                    else if (key == "on_bounce") em->onBounceName = value;
                    else if (key == "count") {
                        if (section != Section::Event) fail("unknown key '" + key + "' in [emitter]");
                        em->eventCount = parseUInt(value, lineNo);
                    }
                    else if (key == "inherit") {
                        if (section != Section::Event) fail("unknown key '" + key + "' in [emitter]");
                        em->inheritVelocity = parseFloat(value, lineNo);
                    }
                    else fail("unknown key '" + key + "' in [" +
                              std::string(section == Section::Event ? "event" : "emitter") + "]");
                    break;
                }
                case Section::Curves: {
                    if (value.empty()) { // explicit empty = close the channel
                        if (key == "color") cfg.colorKeys.clear();
                        else if (key == "size") cfg.sizeKeys.clear();
                        else fail("unknown key '" + key + "' in [curves]");
                    } else if (key == "color") {
                        cfg.colorKeys = parseCurveKeys(value, lineNo, 4);
                    } else if (key == "size") {
                        cfg.sizeKeys = parseCurveKeys(value, lineNo, 1);
                    } else fail("unknown key '" + key + "' in [curves]");
                    break;
                }
                case Section::Attractor: {
                    if (key == "position") cfg.attractors.back().position = parseVec3(value, lineNo);
                    else if (key == "strength") cfg.attractors.back().strength = parseFloat(value, lineNo);
                    else fail("unknown key '" + key + "' in [attractor]");
                    break;
                }
                case Section::Vortex: {
                    if (key == "position")
                        cfg.vortexes.back().center =
                            glm::vec4(parseVec3(value, lineNo), cfg.vortexes.back().center.w);
                    else if (key == "radius") cfg.vortexes.back().center.w = parseFloat(value, lineNo);
                    else if (key == "axis")
                        cfg.vortexes.back().axisStrength =
                            glm::vec4(parseVec3(value, lineNo), cfg.vortexes.back().axisStrength.w);
                    else if (key == "strength") cfg.vortexes.back().axisStrength.w = parseFloat(value, lineNo);
                    else fail("unknown key '" + key + "' in [vortex]");
                    break;
                }
                case Section::Spring: {
                    if (key == "position") cfg.springs.back().anchor = parseVec3(value, lineNo);
                    else if (key == "stiffness") cfg.springs.back().stiffness = parseFloat(value, lineNo);
                    else if (key == "damping") cfg.springs.back().damping = parseFloat(value, lineNo);
                    else fail("unknown key '" + key + "' in [spring]");
                    break;
                }
                case Section::None:
                    fail("key '" + key + "' outside any section");
            }
        }
        return cfg;
    }

private:
    static std::string trim(const std::string& s) {
        std::size_t b = 0, e = s.size();
        while (b < e && std::isspace((unsigned char)s[b])) ++b;
        while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
        return s.substr(b, e - b);
    }
    static std::string toLower(std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }
    static std::vector<std::string> split(const std::string& s, char sep) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (c == sep) { out.push_back(cur); cur.clear(); }
            else cur += c;
        }
        out.push_back(cur);
        return out;
    }
    // "[section \"arg\"]" -> "arg"; "[section]" -> section name
    static std::string sectionArg(const std::string& body) {
        const auto split = body.find_first_of(" \t");
        if (split == std::string::npos) return body; // unnamed emitter
        const std::string arg = trim(body.substr(split));
        if (arg.size() < 2 || arg.front() != '"' || arg.back() != '"' ||
            arg.find('"', 1) != arg.size() - 1)
            throw std::invalid_argument("malformed section");
        return arg.substr(1, arg.size() - 2);
    }
    static float parseFloat(const std::string& v, int line) {
        try {
            std::size_t pos = 0;
            const float f = std::stof(v, &pos);
            if (pos != v.size() || !std::isfinite(f)) throw std::invalid_argument("invalid number");
            return f;
        } catch (...) {
            throw std::runtime_error("ember config: bad number '" + v + "' (line " + std::to_string(line) + ")");
        }
    }
    static std::uint32_t parseUInt(const std::string& v, int line) {
        try {
            std::size_t pos = 0;
            if (v.empty() || v.front() == '-') throw std::invalid_argument("negative");
            const unsigned long long u = std::stoull(v, &pos);
            if (pos != v.size() || u > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("trailing");
            return (std::uint32_t)u;
        } catch (...) {
            throw std::runtime_error("ember config: bad integer '" + v + "' (line " + std::to_string(line) + ")");
        }
    }
    static bool parseBool(const std::string& v, int line) {
        const std::string l = toLower(v);
        if (l == "true" || l == "1" || l == "yes" || l == "on") return true;
        if (l == "false" || l == "0" || l == "no" || l == "off") return false;
        throw std::runtime_error("ember config: bad bool '" + v + "' (line " + std::to_string(line) + ")");
    }
    static glm::vec3 parseVec3(const std::string& v, int line) {
        const auto t = split(v, ',');
        if (t.size() != 3) throw std::runtime_error("ember config: expected 3 numbers (line " + std::to_string(line) + ")");
        return glm::vec3(parseFloat(trim(t[0]), line), parseFloat(trim(t[1]), line), parseFloat(trim(t[2]), line));
    }
    static glm::vec4 parseVec4(const std::string& v, int line) {
        const auto t = split(v, ',');
        if (t.size() != 4) throw std::runtime_error("ember config: expected 4 numbers (line " + std::to_string(line) + ")");
        return glm::vec4(parseFloat(trim(t[0]), line), parseFloat(trim(t[1]), line),
                         parseFloat(trim(t[2]), line), parseFloat(trim(t[3]), line));
    }
    // Comma-separated "t:c0,c1,..." keys. Each key carries exactly `components`
    // values; t must lie in [0,1] and strictly increase.
    static std::vector<CurveKey> parseCurveKeys(const std::string& v, int line, int components) {
        std::vector<CurveKey> out;
        const auto toks = split(v, ',');
        std::size_t i = 0;
        while (i < toks.size()) {
            const std::string tok = trim(toks[i]);
            const std::size_t colon = tok.find(':');
            if (colon == std::string::npos)
                throw std::runtime_error("ember config: curves key must be 't:value' (line " + std::to_string(line) + ")");
            CurveKey k;
            k.t = parseFloat(tok.substr(0, colon), line);
            if (k.t < 0.f || k.t > 1.f)
                throw std::runtime_error("ember config: curves t must be in [0,1] (line " + std::to_string(line) + ")");
            if (!out.empty() && k.t <= out.back().t)
                throw std::runtime_error("ember config: curves t must strictly increase (line " + std::to_string(line) + ")");
            std::vector<float> vals;
            vals.push_back(parseFloat(tok.substr(colon + 1), line));
            ++i;
            while (i < toks.size() && toks[i].find(':') == std::string::npos) {
                vals.push_back(parseFloat(trim(toks[i]), line));
                ++i;
            }
            if ((int)vals.size() != components)
                throw std::runtime_error("ember config: curves key needs " + std::to_string(components) +
                                         " component(s) (line " + std::to_string(line) + ")");
            for (int c = 0; c < components; ++c) k.value[c] = vals[c];
            out.push_back(k);
        }
        if (out.empty())
            throw std::runtime_error("ember config: empty curves list (line " + std::to_string(line) + ")");
        return out;
    }
    static Emitter::Shape parseShape(const std::string& v, int line) {
        const std::string l = toLower(v);
        if (l == "point") return Emitter::Shape::Point;
        if (l == "box") return Emitter::Shape::Box;
        if (l == "sphere") return Emitter::Shape::Sphere;
        if (l == "cone") return Emitter::Shape::Cone;
        throw std::runtime_error("ember config: unknown shape '" + v + "' (line " + std::to_string(line) + ")");
    }
};

} // namespace ember
