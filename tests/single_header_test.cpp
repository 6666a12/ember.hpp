// Single-header consumption test: the WHOLE ember library comes from
// single_header/ember.hpp (EMBER_IMPLEMENTATION in this TU). No ember library
// is linked — only the documented external deps (glad for GL entry points,
// GLFW for the context, glm for math; stb_image for PNG sprites).
//
// Exercises: GPU spawn, GPU spawn request pipeline, indirect draw, depth sort,
// bloom — plus a pixel-level check that particles actually render.

#define EMBER_IMPLEMENTATION
#define EMBER_USE_GLFW
#define EMBER_USE_STB
#define STB_IMAGE_IMPLEMENTATION // compiles stb_image into this TU (PNG sprites)
#include "ember.hpp"

#include <glm/glm.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)
} // namespace

int main() {
    ember::Window* win = nullptr;
    try {
        win = new ember::Window(128, 128, "ember single-header test", 0, /*vsync=*/false, /*visible=*/false);
    } catch (const std::exception& e) {
        std::printf("SKIP: cannot create GL 4.3 context: %s\n", e.what());
        return 77;
    }

    try {
        // ---- capacity / GPU spawn through the single header -------------------
        ember::ParticleSystem sys({3000, 1500});
        CHECK(sys.capacity() == 3000);
        CHECK(sys.aliveCount() == 0);

        ember::Emitter e;
        e.shape = ember::Emitter::Shape::Sphere;
        e.rate = 1500.f;
        e.speedMin = 1.f; e.speedMax = 3.f;
        e.lifeMin = e.lifeMax = 2.f;
        e.sizeMin = e.sizeMax = 0.1f;
        e.colorMin = e.colorMax = {1.f, 0.5f, 0.1f, 1.f};
        sys.addEmitter(e);

        for (int i = 0; i < 10; ++i) sys.update(1.f / 60.f);
        CHECK(sys.aliveCount() > 0);
        CHECK(sys.aliveCount() <= sys.capacity());

        // GPU-spawned particles must lie inside the configured ranges
        sys.setForceMask(0);
        sys.setGravity({0.f, 0.f, 0.f});
        for (int i = 0; i < 5; ++i) sys.update(1.f / 60.f);
        bool speedOk = true, sizeOk = true, lifeOk = true;
        int fresh = 0;
        for (const auto& p : sys.readParticles()) {
            if (p.vel.w < 0.1f) {
                ++fresh;
                const float sp = glm::length(glm::vec3(p.vel));
                if (sp < 1.f - 1e-2f || sp > 3.f + 1e-2f) speedOk = false;
            }
            if (p.pos.w < 0.1f - 1e-3f || p.pos.w > 0.1f + 1e-3f) sizeOk = false;
            if (p.life.x < 2.f - 1e-2f || p.life.x > 2.f + 1e-2f) lifeOk = false;
        }
        CHECK(fresh > 0);
        CHECK(speedOk && sizeOk && lifeOk);

        // ---- palette pick -----------------------------------------------------
        {
            ember::ParticleSystem sys2({2000, 1000});
            sys2.setForceMask(0);
            ember::Emitter e2;
            e2.rate = 500.f;
            e2.speedMin = e2.speedMax = 2.f;
            e2.lifeMin = e2.lifeMax = 1.f;
            e2.sizeMin = e2.sizeMax = 0.05f;
            e2.palette = {{1.f, 0.f, 0.f, 1.f}, {0.f, 1.f, 0.f, 1.f}};
            sys2.addEmitter(e2);
            for (int i = 0; i < 3; ++i) sys2.update(1.f / 60.f);
            bool allPal = true;
            for (const auto& p : sys2.readParticles()) {
                if (p.color != e2.palette[0] && p.color != e2.palette[1]) allPal = false;
            }
            CHECK(allPal);
        }

        // ---- burst ------------------------------------------------------------
        {
            ember::ParticleSystem sys3({2000, 1000});
            sys3.setForceMask(0);
            ember::BurstParams b;
            b.count = 300;
            b.speedMin = b.speedMax = 5.f;
            b.lifeMin = b.lifeMax = 2.f;
            sys3.burst(b);
            CHECK(sys3.aliveCount() == 0);
            sys3.update(1.f / 60.f);
            CHECK(sys3.aliveCount() == 300);
        }

        // ---- pixel check: plain / sort / bloom paths must draw particles ------
        const glm::mat4 view = glm::lookAt(glm::vec3(0.f, 3.f, 8.f), glm::vec3(0.f), glm::vec3(0.f, 1.f, 0.f));
        const glm::mat4 proj = glm::perspective(glm::radians(50.f), 1.f, 0.1f, 100.f);
        auto countLit = [&]() {
            std::vector<unsigned char> px(128 * 128 * 4);
            glReadPixels(0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            int lit = 0;
            for (std::size_t i = 0; i < px.size(); i += 4)
                if (px[i] > 8 || px[i + 1] > 8 || px[i + 2] > 8) ++lit;
            return lit;
        };
        auto renderOnce = [&]() {
            glViewport(0, 0, 128, 128);
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            sys.render(view, proj, 128.f, 128.f, 50.f);
            glFinish();
            return countLit();
        };
        CHECK(renderOnce() > 0);
        sys.setSortEnabled(true);
        CHECK(renderOnce() > 0);
        sys.setSortEnabled(false);
        sys.setBloom(true);
        CHECK(renderOnce() > 0);
        sys.setBloom(false);

        delete win;
        win = nullptr;

        if (g_failures == 0) {
            std::printf("single-header test: ALL PASSED\n");
            return 0;
        }
        std::printf("single-header test: %d FAILURES\n", g_failures);
        return 1;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        delete win;
        return 1;
    }
}
