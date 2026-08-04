// Standalone tests for the config parser + emitter spawn logic.
// No GL context needed — runs anywhere. Usage: ember_config_test [config_dir]

#include "ember/config.hpp"
#include "ember/emitters.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_THROWS(expr)                                                     \
    do {                                                                       \
        bool threw = false;                                                    \
        try {                                                                  \
            (void)(expr);                                                      \
        } catch (const std::exception&) {                                      \
            threw = true;                                                      \
        }                                                                      \
        CHECK(threw);                                                          \
    } while (0)

bool vecEq(const glm::vec3& a, const glm::vec3& b, float eps = 1e-5f) {
    return glm::length(a - b) < eps;
}
bool vecEq(const glm::vec4& a, const glm::vec4& b, float eps = 1e-5f) {
    return glm::length(a - b) < eps;
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";

    // ---- parse config/example.ini ------------------------------------------
    {
        ember::Config cfg = ember::Config::fromFile((dir + "/config/example.ini").c_str());
        CHECK(vecEq(cfg.system.gravity, {0.f, -7.f, 0.f}));
        CHECK(cfg.system.drag == 0.05f);
        CHECK(vecEq(cfg.system.wind, {0.f, 0.f, 0.f}));
        CHECK(cfg.system.turbulence == 0.4f);
        CHECK(cfg.system.blend == "additive");
        CHECK(cfg.system.capacity == 150000);
        CHECK(cfg.system.maxSpawnPerFrame == 40000);
        CHECK(cfg.system.sizeScale == 1.f);
        CHECK(cfg.system.editor);
        CHECK(cfg.system.streak == 0.12f);
        CHECK(cfg.system.softParticles);
        CHECK(cfg.system.softRadius == 0.6f);

        // force switches / new fields
        CHECK(cfg.system.forces.size() == 7);
        CHECK(cfg.system.forces[0] == "gravity" && cfg.system.forces[5] == "vortex" &&
              cfg.system.forces[6] == "noise_wind");
        CHECK(cfg.system.dragMode == "linear");
        CHECK(vecEq(cfg.system.noiseWindDir, {0.f, 0.f, 1.f}));
        CHECK(cfg.system.noiseWindAmp == 2.5f);
        CHECK(cfg.system.noiseWindScale == 0.4f);
        CHECK(cfg.system.noiseWindSpeed == 0.8f);
        CHECK(cfg.vortexes.size() == 1);
        CHECK(vecEq(glm::vec3(cfg.vortexes[0].center), {0.f, 3.f, 0.f}));
        CHECK(cfg.vortexes[0].center.w == 6.f);
        CHECK(vecEq(glm::vec3(cfg.vortexes[0].axisStrength), {0.f, 1.f, 0.f}));
        CHECK(cfg.vortexes[0].axisStrength.w == 8.f);
        CHECK(cfg.springs.empty());

        CHECK(cfg.palettes.size() == 3);
        CHECK(cfg.palettes[0].name == "fire");
        CHECK(cfg.palettes[0].colors.size() == 3);
        CHECK(vecEq(cfg.palettes[1].colors[0], {0.75f, 0.95f, 1.f, 0.9f}));
        CHECK(cfg.palettes[2].colors.size() == 4);

        CHECK(cfg.emitters.size() == 2);
        const auto& fountain = cfg.emitters[0];
        CHECK(fountain.name == "fountain");
        CHECK(fountain.shape == ember::Emitter::Shape::Cone);
        CHECK(vecEq(fountain.axis, {0.f, 1.f, 0.f}));
        CHECK(fountain.radius == 0.25f);
        CHECK(fountain.rate == 7000.f);
        CHECK(vecEq(fountain.baseVelocity, {0.f, 10.f, 0.f}));
        CHECK(fountain.speedMin == 7.f && fountain.speedMax == 11.f);
        CHECK(fountain.spread == 0.25f);
        CHECK(fountain.lifeMin == 2.f && fountain.lifeMax == 3.5f);
        CHECK(fountain.sizeMin == 0.05f && fountain.sizeMax == 0.11f);
        CHECK(fountain.paletteName == "fire");
        CHECK(fountain.coneAngle == 22.f);
        CHECK(fountain.hasFadeColor);
        CHECK(vecEq(fountain.fadeColorMin, {1.f, 0.35f, 0.1f}));
        CHECK(vecEq(fountain.fadeColorMax, {1.f, 0.05f, 0.02f}));
        CHECK(fountain.speedScale == 1.f); // no speed_scale key in example.ini
        CHECK(fountain.active);

        const auto& trail = cfg.emitters[1];
        CHECK(trail.name == "spark trail");
        CHECK(trail.shape == ember::Emitter::Shape::Sphere);
        CHECK(trail.radius == 0.08f);
        CHECK(trail.paletteName == "aurora");

        CHECK(cfg.attractors.size() == 1);
        CHECK(vecEq(cfg.attractors[0].position, {0.f, 0.6f, 0.f}));
        CHECK(cfg.attractors[0].strength == 60.f);

        CHECK(cfg.findPalette("fire") != nullptr);
        CHECK(cfg.findPalette("nope") == nullptr);

        // presence tracking: keys/sections that appeared vs. those absent
        CHECK(cfg.has("system.gravity") && cfg.has("system.forces") &&
              cfg.has("system.noise_wind_amplitude"));
        CHECK(cfg.has("vortex") && cfg.has("attractor") && cfg.has("emitter"));
        CHECK(!cfg.has("spring"));
        CHECK(!cfg.has("system.wave_amplitude"));
        CHECK(!cfg.has("system.boundary_mode"));
    }

    // ---- parse config/fireworks.ini -----------------------------------------
    {
        ember::Config cfg = ember::Config::fromFile((dir + "/config/fireworks.ini").c_str());
        CHECK(cfg.palettes.size() == 6);
        CHECK(cfg.system.capacity == 250000);
        CHECK(cfg.system.gravity.y == -4.f);
        CHECK(cfg.system.drag == 0.06f);
        CHECK(cfg.system.forces.size() == 4);
        CHECK(cfg.system.forces[0] == "gravity" && cfg.system.forces[3] == "boundary");
        CHECK(cfg.system.boundaryMode == "kill");
        CHECK(cfg.system.boundaryY == 0.2f);
        CHECK(cfg.system.restitution == 0.5f);
    }

    // ---- parse from string ---------------------------------------------------
    {
        const std::string text =
            "# comment\n"
            "[system]\n"
            "gravity = 1, 2, 3\n"
            "blend = Normal\n"
            "forces = Gravity, drag, VORTEX, boundary\n"
            "drag_mode = quadratic\n"
            "boundary_mode = bounce\n"
            "boundary_y = -1.5\n"
            "restitution = 0.3\n"
            "size_scale = 2.5\n"
            "scale_speed_with_size = false\n"
            "texture = sprites/glow.png\n"
            "editor = false\n"
            "[palette \"x\"]\n"
            "colors = 1,0,0,1 | 0,1,0,0.5\n"
            "[emitter \"e\"]\n"
            "shape = box\n"
            "position = 4, 5, 6\n"
            "extents = 1, 2, 3\n"
            "rate = 10\n"
            "palette = x\n"
            "cone_angle = 30\n"
            "speed_scale = 1.5\n"
            "speed_size_link = 0.7\n"
            "fade_color_min = 1, 0.2, 0.05\n"
            "fade_color_max = 1, 0.05, 0.02\n"
            "[attractor]\n"
            "position = 0, 0, 0\n"
            "strength = -5\n"
            "[vortex]\n"
            "position = 1, 2, 3\n"
            "radius = 4\n"
            "axis = 0, 0, 1\n"
            "strength = 7\n"
            "[spring]\n"
            "position = 9, 9, 9\n"
            "stiffness = 1.5\n"
            "damping = 0.6\n";
        ember::Config cfg = ember::Config::fromString(text);
        CHECK(vecEq(cfg.system.gravity, {1.f, 2.f, 3.f}));
        CHECK(cfg.system.blend == "normal");
        CHECK(cfg.system.forces.size() == 4);
        CHECK(cfg.system.forces[0] == "gravity" && cfg.system.forces[1] == "drag" &&
              cfg.system.forces[2] == "vortex" && cfg.system.forces[3] == "boundary");
        CHECK(cfg.system.dragMode == "quadratic");
        CHECK(cfg.system.boundaryMode == "bounce");
        CHECK(cfg.system.boundaryY == -1.5f);
        CHECK(cfg.system.restitution == 0.3f);
        CHECK(cfg.system.sizeScale == 2.5f);
        CHECK(!cfg.system.scaleSpeedWithSize);
        CHECK(cfg.system.texture == "sprites/glow.png");
        CHECK(!cfg.system.editor);
        CHECK(cfg.palettes.size() == 1 && cfg.palettes[0].colors.size() == 2);
        CHECK(vecEq(cfg.palettes[0].colors[1], {0.f, 1.f, 0.f, 0.5f}));
        CHECK(cfg.emitters.size() == 1);
        CHECK(cfg.emitters[0].shape == ember::Emitter::Shape::Box);
        CHECK(vecEq(cfg.emitters[0].extents, {1.f, 2.f, 3.f}));
        CHECK(cfg.emitters[0].coneAngle == 30.f);
        CHECK(cfg.emitters[0].speedScale == 1.5f);
        CHECK(cfg.emitters[0].speedSizeLink == 0.7f);
        CHECK(cfg.emitters[0].hasFadeColor);
        CHECK(vecEq(cfg.emitters[0].fadeColorMin, {1.f, 0.2f, 0.05f}));
        CHECK(vecEq(cfg.emitters[0].fadeColorMax, {1.f, 0.05f, 0.02f}));
        CHECK(cfg.attractors.size() == 1 && cfg.attractors[0].strength == -5.f);
        CHECK(cfg.vortexes.size() == 1);
        CHECK(vecEq(glm::vec3(cfg.vortexes[0].center), {1.f, 2.f, 3.f}));
        CHECK(cfg.vortexes[0].center.w == 4.f);
        CHECK(vecEq(glm::vec3(cfg.vortexes[0].axisStrength), {0.f, 0.f, 1.f}));
        CHECK(cfg.vortexes[0].axisStrength.w == 7.f);
        CHECK(cfg.springs.size() == 1);
        CHECK(vecEq(cfg.springs[0].anchor, {9.f, 9.f, 9.f}));
        CHECK(cfg.springs[0].stiffness == 1.5f);
        CHECK(cfg.springs[0].damping == 0.6f);
        CHECK(cfg.has("system.drag_mode") && cfg.has("system.boundary_mode") &&
              cfg.has("vortex") && cfg.has("spring") && cfg.has("emitter"));
        CHECK(!cfg.has("system.turbulence")); // not in this text
    }

    // ---- error handling --------------------------------------------------------
    {
        CHECK_THROWS(ember::Config::fromString("[system]\nunknown_key = 1\n"));
        CHECK_THROWS(ember::Config::fromString("[system]\ndrag = notanumber\n"));
        CHECK_THROWS(ember::Config::fromString("[system]\nwind = 1, 2\n"));       // 2 != 3 components
        CHECK_THROWS(ember::Config::fromString("[bogus]\nx = 1\n"));
        CHECK_THROWS(ember::Config::fromString("key = value\n"));                 // outside any section
        CHECK_THROWS(ember::Config::fromString("[emitter \"e\"]\nshape = octagon\n"));
        CHECK_THROWS(ember::Config::fromString("[palette \"p\"]\ncolors = 1, 2\n")); // bad vec4
        CHECK_THROWS(ember::Config::fromString("[vortex]\nbogus_key = 1\n"));
        CHECK_THROWS(ember::Config::fromString("[spring]\nstiffness = x\n"));
        CHECK_THROWS(ember::Config::fromFile("definitely_missing.ini"));
    }

    // ---- UTF-8 BOM tolerance (Windows Notepad saves) --------------------------
    {
        ember::Config cfg =
            ember::Config::fromString("\xEF\xBB\xBF[system]\ngravity = 1, 2, 3\n");
        CHECK(vecEq(cfg.system.gravity, {1.f, 2.f, 3.f}));
        CHECK(cfg.has("system.gravity"));
    }

    // ---- emitter spawn logic -----------------------------------------------------
    {
        ember::Rng rng(12345);
        ember::Emitter cone;
        cone.shape = ember::Emitter::Shape::Cone;
        cone.rate = 1000.f;
        cone.speedMin = 5.f;
        cone.speedMax = 7.f;
        cone.lifeMin = 1.f;
        cone.lifeMax = 2.f;
        cone.sizeMin = 0.1f;
        cone.sizeMax = 0.2f;
        cone.colorMin = {1.f, 0.f, 0.f, 1.f};
        cone.colorMax = {1.f, 0.f, 0.f, 1.f};

        std::vector<ember::Particle> out;
        std::uint32_t n = cone.spawn(out, 10000, 0.1f, rng); // expect ~100
        CHECK(n > 50 && n <= 200);
        CHECK(out.size() == n);
        for (const auto& p : out) {
            CHECK(p.vel.w == 0.f);                                    // age starts at 0
            CHECK(p.life.x >= 1.f && p.life.x <= 2.f);
            CHECK(p.pos.w >= 0.1f && p.pos.w <= 0.2f);
            CHECK(p.color == glm::vec4(1.f, 0.f, 0.f, 1.f));
            const float speed = glm::length(glm::vec3(p.vel));
            CHECK(speed >= 5.f - 1e-3f && speed <= 7.f + 1e-3f);
        }

        // palette path: every emitted color must come from the palette
        ember::Emitter pal;
        pal.rate = 500.f;
        pal.palette = {{1.f, 0.f, 0.f, 1.f}, {0.f, 1.f, 0.f, 1.f}};
        out.clear();
        pal.spawn(out, 10000, 0.01f, rng);
        CHECK(!out.empty());
        for (const auto& p : out) {
            const bool inPalette = p.color == pal.palette[0] || p.color == pal.palette[1];
            CHECK(inPalette);
        }

        // inactive emitter spawns nothing
        cone.active = false;
        out.clear();
        CHECK(cone.spawn(out, 10000, 0.1f, rng) == 0);
        CHECK(out.empty());
    }

    // ---- pure logic: Rng ---------------------------------------------------------
    {
        ember::Rng rng(7);
        for (int i = 0; i < 1000; ++i) {
            const float u = rng.unit();
            CHECK(u >= 0.f && u < 1.f);
            const float r = rng.range(3.f, 9.f);
            CHECK(r >= 3.f && r <= 9.f);
            const glm::vec3 d = rng.dirOnSphere();
            CHECK(std::abs(glm::length(d) - 1.f) < 1e-3f);
        }
    }

    // ---- pure logic: cone angle + speed scale + fade writing ----------------------
    {
        ember::Rng rng(99);
        ember::Emitter em;
        em.shape = ember::Emitter::Shape::Point;
        em.rate = 1000.f;
        em.axis = {0.f, 1.f, 0.f};
        em.coneAngle = 30.f;
        em.speedMin = 2.f;
        em.speedMax = 2.f;          // exact speed for deterministic asserts
        em.speedScale = 1.5f;
        em.sizeMin = 0.1f;
        em.sizeMax = 0.1f;
        em.lifeMin = em.lifeMax = 1.f;
        em.fadeColorMin = {1.f, 0.5f, 0.25f};
        em.fadeColorMax = {1.f, 0.5f, 0.25f};
        em.hasFadeColor = true;

        std::vector<ember::Particle> out;
        em.spawn(out, 10000, 0.1f, rng);
        CHECK(!out.empty());
        const float cosHalf = std::cos(glm::radians(30.f));
        for (const auto& p : out) {
            // cone emission: direction must stay inside the cone around +Y
            const glm::vec3 dir = glm::normalize(glm::vec3(p.vel));
            CHECK(dir.y >= cosHalf - 1e-3f);
            // speed = base (2) * speedScale (1.5) = 3
            CHECK(std::abs(glm::length(glm::vec3(p.vel)) - 3.f) < 1e-3f);
            // fade color written into life.yzw
            CHECK(std::abs(p.life.y - 1.f) < 1e-5f);
            CHECK(std::abs(p.life.z - 0.5f) < 1e-5f);
            CHECK(std::abs(p.life.w - 0.25f) < 1e-5f);
        }

        // without fade config, life.yzw mirrors the spawn color (identity fade)
        em.hasFadeColor = false;
        out.clear();
        em.spawn(out, 10000, 0.1f, rng);
        for (const auto& p : out) {
            CHECK(std::abs(p.life.y - p.color.r) < 1e-5f);
            CHECK(std::abs(p.life.z - p.color.g) < 1e-5f);
            CHECK(std::abs(p.life.w - p.color.b) < 1e-5f);
        }
    }

    // ---- pure logic: BurstParams defaults -------------------------------------------
    {
        ember::BurstParams b;
        CHECK(b.count == 100);
        CHECK(b.speedMin == 5.f && b.speedMax == 10.f);
        CHECK(b.lifeMin == 1.f && b.lifeMax == 2.f);
        CHECK(b.spread == 1.f);
    }

    if (g_failures == 0) {
        std::printf("config test: ALL PASSED\n");
        return 0;
    }
    std::printf("config test: %d FAILURES\n", g_failures);
    return 1;
}
