// GL-backed smoke test for ParticleSystem (aliveCount / capacity / clear /
// emitters / sizeScale through the real GPU pipeline).
//
// Requires a working OpenGL 4.3 context. When none can be created the test
// prints SKIP and exits with 77 (ctest SKIP_RETURN_CODE), so it can run in
// headless CI without a GPU.

#include "ember/glfw_window.hpp"
#include "ember/particle_system.hpp"

#include <glm/glm.hpp>

#include <cstdio>
#include <memory>
#include <cmath>
#include "ember/config.hpp"
#include "regressions.hpp"
#include "behavior.hpp"

namespace {
int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
            throw std::runtime_error(#cond);                                                 \
        }                                                                 \
    } while (0)
} // namespace

int main() {
    // ---- context setup (skip cleanly when no GL is available) ------------
    std::unique_ptr<ember::Window> win;
    try {
        win = std::make_unique<ember::Window>(64, 64, "ember system test", 0, /*vsync=*/false, /*visible=*/false);
    } catch (const ember::ContextUnavailable& e) {
        std::printf("SKIP: cannot create GL 4.3 context: %s\n", e.what());
        return 77;
    } catch (const std::exception& e) {
        std::printf("FAIL: GL initialization: %s\n", e.what());
        return 1;
    }

    try {
        // ---- capacity / initial state --------------------------------------
        ember::ParticleSystem sys({5000, 2000});
        CHECK(sys.capacity() == 5000);
        CHECK(sys.aliveCount() == 0);
        CHECK(sys.emitterCount() == 0);
        CHECK(sys.maxSpawnPerFrame() == 2000);

        // ---- emitters ---------------------------------------------------------
        ember::Emitter e;
        e.shape = ember::Emitter::Shape::Sphere;
        e.rate = 2000.f;
        e.lifeMin = e.lifeMax = 2.f;
        e.speedMin = 1.f;
        e.speedMax = 3.f;
        e.sizeMin = e.sizeMax = 0.1f;
        sys.addEmitter(e);
        CHECK(sys.emitterCount() == 1);

        // ---- run the pipeline: alive must grow but never exceed capacity ------
        for (int i = 0; i < 30; ++i) sys.update(1.f / 60.f);
        const std::uint32_t alive = sys.aliveCount();
        CHECK(alive > 0);
        CHECK(alive <= sys.capacity());
        std::printf("OK: alive=%u capacity=%u\n", alive, sys.capacity());

        // render() must not throw (exercises the draw path)
        sys.render(glm::mat4(1.f), glm::mat4(1.f), 64.f, 64.f, 50.f);

        // ---- GPU spawn assertions (read back live particles) -------------------
        sys.setForceMask(0); // no gravity/drag: fresh velocities stay at spawn values
        sys.setGravity({0.f, 0.f, 0.f});
        sys.clear();
        for (int i = 0; i < 5; ++i) sys.update(1.f / 60.f);
        {
            const std::vector<ember::Particle> ps = regression::particles(sys);
            CHECK(ps.size() == sys.aliveCount());
            bool speedOk = true, sizeOk = true, lifeOk = true;
            int fresh = 0;
            for (const auto& p : ps) {
                if (p.vel.w < 0.1f) { // born after forces were disabled
                    ++fresh;
                    const float sp = glm::length(glm::vec3(p.vel));
                    if (sp < 1.f - 1e-2f || sp > 3.f + 1e-2f) speedOk = false;
                }
                if (p.pos.w < 0.1f - 1e-3f || p.pos.w > 0.1f + 1e-3f) sizeOk = false;
                if (p.life.x < 2.f - 1e-2f || p.life.x > 2.f + 1e-2f) lifeOk = false;
            }
            CHECK(fresh > 0); // GPU spawn actually produced new particles
            CHECK(speedOk);
            CHECK(sizeOk);
            CHECK(lifeOk);
        }

        // ---- GPU spawn: palette pick + identity fade ----------------------------
        {
            ember::ParticleSystem sys2({2000, 1000});
            sys2.setForceMask(0);
            ember::Emitter e2;
            e2.shape = ember::Emitter::Shape::Point;
            e2.rate = 500.f;
            e2.speedMin = e2.speedMax = 2.f;
            e2.lifeMin = e2.lifeMax = 1.f;
            e2.sizeMin = e2.sizeMax = 0.05f;
            e2.palette = {{1.f, 0.f, 0.f, 1.f}, {0.f, 1.f, 0.f, 1.f}};
            sys2.addEmitter(e2);
            for (int i = 0; i < 3; ++i) sys2.update(1.f / 60.f);
            bool allPal = true, allFade = true;
            for (const auto& p : regression::particles(sys2)) {
                if (p.color != e2.palette[0] && p.color != e2.palette[1]) allPal = false;
                if (glm::length(glm::vec3(p.life.y, p.life.z, p.life.w) - glm::vec3(p.color)) > 1e-5f)
                    allFade = false;
            }
            CHECK(allPal);
            CHECK(allFade);
        }

        // ---- GPU spawn: cone emission stays inside the cone ---------------------
        {
            ember::ParticleSystem sys3({2000, 1000});
            sys3.setForceMask(0);
            ember::Emitter e3;
            e3.shape = ember::Emitter::Shape::Point;
            e3.rate = 500.f;
            e3.speedMin = e3.speedMax = 2.f;
            e3.lifeMin = e3.lifeMax = 1.f;
            e3.sizeMin = e3.sizeMax = 0.05f;
            e3.axis = {0.f, 1.f, 0.f};
            e3.coneAngle = 30.f;
            sys3.addEmitter(e3);
            for (int i = 0; i < 3; ++i) sys3.update(1.f / 60.f);
            const float cosHalf = std::cos(glm::radians(30.f));
            bool allCone = true;
            for (const auto& p : regression::particles(sys3)) {
                const float speed = glm::length(glm::vec3(p.vel));
                CHECK(std::isfinite(speed) && speed > 0.f);
                const glm::vec3 dir = glm::normalize(glm::vec3(p.vel));
                if (!std::isfinite(dir.y) || dir.y < cosHalf - 1e-3f) allCone = false;
            }
            CHECK(allCone);
        }

        // ---- GPU spawn: burst() applies on the next update() --------------------
        {
            ember::ParticleSystem sys4({5000, 2000});
            sys4.setForceMask(0);
            ember::BurstParams b;
            b.count = 500;
            b.speedMin = b.speedMax = 5.f;
            b.lifeMin = b.lifeMax = 2.f;
            b.sizeMin = b.sizeMax = 0.1f;
            b.colorMin = b.colorMax = {1.f, 1.f, 1.f, 1.f};
            sys4.burst(b);
            CHECK(sys4.aliveCount() == 0); // pending until update()
            sys4.update(1.f / 60.f);
            CHECK(sys4.aliveCount() == 500);
            bool burstOk = true;
            for (const auto& p : regression::particles(sys4)) {
                if (std::abs(glm::length(glm::vec3(p.vel)) - 5.f) > 1e-2f) burstOk = false;
            }
            CHECK(burstOk);
        }

        // ---- refraction: sign encoding + presets + no-throw renders --------------
        {
            ember::ParticleSystem sys5({2000, 1000});
            sys5.setForceMask(0);
            ember::Emitter e5;
            e5.shape = ember::Emitter::Shape::Point;
            e5.rate = 500.f;
            e5.speedMin = e5.speedMax = 2.f;
            e5.lifeMin = e5.lifeMax = 1.f;
            e5.sizeMin = e5.sizeMax = 0.05f;
            e5.refractive = true; // glass shards
            sys5.addEmitter(e5);

            ember::RefractionSettings r = ember::Refraction::glass();
            CHECK(!r.enabled); // presets leave enabled to the caller
            CHECK(r.mode == 0);
            CHECK(r.chroma > 0.f && r.specular > 0.f && r.fresnel > 0.f);
            r.enabled = true;
            setOpenGLRefraction(sys5,r);
            sys5.setSpin(1.5f);
            for (int i = 0; i < 3; ++i) sys5.update(1.f / 60.f);

            // refractive particles are sign-encoded in pos.w
            bool allNeg = true;
            for (const auto& p : regression::particles(sys5))
                if (p.pos.w >= 0.f) allNeg = false;
            CHECK(allNeg);

            // no scene texture -> refraction pass skipped; must not throw
            sys5.render(glm::mat4(1.f), glm::mat4(1.f), 64.f, 64.f, 50.f);

            // noise mode (heat) renders without a scene texture too
            ember::RefractionSettings h = ember::Refraction::heat();
            CHECK(h.mode == 2);
            h.enabled = true;
            setOpenGLRefraction(sys5,h);
            sys5.render(glm::mat4(1.f), glm::mat4(1.f), 64.f, 64.f, 50.f);

            // depth-aware mode without a depth texture falls back to simple
            ember::RefractionSettings d = ember::Refraction::water();
            d.enabled = true;
            d.mode = 1;
            setOpenGLRefraction(sys5,d);
            sys5.render(glm::mat4(1.f), glm::mat4(1.f), 64.f, 64.f, 50.f);

            // refractive burst
            ember::BurstParams b;
            sys5.clearEmitters();
            const auto beforeBurst = sys5.aliveCount();
            b.count = 100;
            b.refractive = true;
            sys5.burst(b);
            sys5.update(1.f / 60.f);
            CHECK(sys5.aliveCount() == beforeBurst + 100);
            allNeg = true;
            for (const auto& p : regression::particles(sys5))
                if (p.pos.w >= 0.f) allNeg = false;
            CHECK(allNeg);
        }

        // ---- clear --------------------------------------------------------------
        sys.clear();
        CHECK(sys.aliveCount() == 0);

        // ---- size scale ----------------------------------------------------------
        CHECK(sys.sizeScale() == 1.f);
        sys.setSizeScale(2.f);
        CHECK(sys.sizeScale() == 2.f);
        sys.setSizeScale(1.f, false); // no speed link
        CHECK(sys.sizeScale() == 1.f);

        // ---- sprite / sheet API must not throw ----------------------------------
        sys.setSpriteSheet(2, 2);
        sys.setUseSprite(false);
        sys.setUseSprite(true);

        // ---- rendering extras: streak / soft / sort / bloom must not throw -----
        sys.setStreak(0.5f);
        CHECK(sys.sortEnabled() == false);
        sys.setSortEnabled(true);   // GPU bitonic depth sort (runs inside render())
        CHECK(sys.sortEnabled());
        setOpenGLSoftParticles(sys,true); // no host depth texture -> soft path resolves off
        setOpenGLSoftParticles(sys,false);
        sys.setBloom(true);         // HDR chain: bright pass + blur + composite
        CHECK(sys.bloom());
        for (int i = 0; i < 3; ++i) sys.update(1.f / 60.f);
        CHECK(sys.aliveCount() > 0);
        sys.render(glm::mat4(1.f), glm::mat4(1.f), 64.f, 64.f, 50.f);
        sys.setBloom(false);
        sys.setSortEnabled(false);
        CHECK(sys.bloom() == false);
        CHECK(sys.sortEnabled() == false);

        regression::glClean("legacy smoke tests");
        regression::run(*win);
        behavior::run([](const ember::ParticleSettings& s) { return ember::ParticleSystem(s); });

        if (g_failures == 0) {
            std::printf("system test: ALL PASSED\n");
            return 0;
        }
        std::printf("system test: %d FAILURES\n", g_failures);
        return 1;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
}
