// ember example: fireworks.
//
// Random bursts in the sky; colors are drawn from the palettes in
// config/fireworks.ini (edit it, press R to reload live).
//
//   left-drag  orbit camera      scroll  zoom
//   R          hot-reload config/fireworks.ini
//   ESC        quit
//
// Usage: example_fireworks [path/to/config.ini]

#include "ember/camera.hpp"
#include "ember/config.hpp"
#include "ember/glfw_window.hpp"
#include "ember/particle_system.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace ember;

namespace {

std::string pickConfig(int argc, char** argv) {
    if (argc > 1) return argv[1];
    for (const char* p : {"config/fireworks.ini", "../config/fireworks.ini", "fireworks.ini"}) {
        std::ifstream f(p);
        if (f) return p;
    }
    return "config/fireworks.ini";
}

} // namespace

int main(int argc, char** argv) {
    try {
        Window win(1280, 720, "ember — fireworks (drag: orbit, R: reload config)");
        OrbitCamera cam;
        cam.target = {0.f, 12.f, 0.f};
        cam.distance = 42.f;
        cam.pitch = 0.35f;

        ParticleSystem sys({250000, 32768});

        std::string cfgPath = pickConfig(argc, argv);
        Config cfg;
        try {
            Config candidate = Config::fromFile(cfgPath.c_str());
            sys.apply(candidate);
            cfg = std::move(candidate);
            std::printf("[fireworks] loaded config: %s (%zu palettes)\n", cfgPath.c_str(), cfg.palettes.size());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[fireworks] warning: %s (running with defaults)\n", e.what());
        }

        Rng rng;
        bool reload = false;
        win.onKey = [&](int key, int, int action, int) {
            if (key == GLFW_KEY_R && action == GLFW_PRESS) reload = true;
        };

        glm::vec2 lastCursor = win.cursor();
        double fpsAccum = 0.0;
        int fpsFrames = 0;
        float nextBurst = 0.3f;

        while (!win.shouldClose()) {
            win.pollEvents();
            const float elapsed = win.deltaTime();
            const float dt = std::min(elapsed, 0.05f);
            const glm::ivec2 vp = win.framebufferSize();
            const glm::ivec2 windowSize = win.size();
            if (vp.x <= 0 || vp.y <= 0 || windowSize.x <= 0 || windowSize.y <= 0) continue;
            if (win.key(GLFW_KEY_ESCAPE)) break;

            if (reload) {
                reload = false;
                try {
                    Config candidate = Config::fromFile(cfgPath.c_str());
                    sys.apply(candidate);
                    cfg = std::move(candidate);
                    std::printf("[fireworks] config reloaded (%zu palettes)\n", cfg.palettes.size());
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "[fireworks] reload failed, keeping previous config: %s\n", e.what());
                }
            }

            const glm::vec2 cur = win.cursor();
            if (win.mouseButton(GLFW_MOUSE_BUTTON_LEFT)) cam.orbit(cur.x - lastCursor.x, cur.y - lastCursor.y);
            cam.zoom(win.scrollDelta().y);
            lastCursor = cur;
            cam.yaw += dt * 0.08f; // slow drift around the sky

            nextBurst -= dt;
            if (nextBurst <= 0.f) {
                BurstParams b;
                b.count = (std::uint32_t)rng.range(1200.f, 2600.f);
                b.position = {rng.range(-16.f, 16.f), rng.range(4.f, 22.f), rng.range(-16.f, 16.f)};
                b.speedMin = 6.f;
                b.speedMax = 16.f;
                b.spread = 1.f; // uniform sphere
                b.lifeMin = 1.6f;
                b.lifeMax = 3.2f;
                b.sizeMin = 0.05f;
                b.sizeMax = 0.15f;
                if (!cfg.palettes.empty()) {
                    const auto& pal = cfg.palettes[(std::size_t)rng.range(0.f, (float)cfg.palettes.size() - 0.001f)];
                    if (!pal.colors.empty()) {
                        const glm::vec4 c =
                            pal.colors[(std::size_t)rng.range(0.f, (float)pal.colors.size() - 0.001f)];
                        b.colorMin = c;
                        b.colorMax = c;
                    }
                }
                sys.burst(b);
                nextBurst = rng.range(0.3f, 1.1f);
            }

            sys.update(dt);

            const float aspect = vp.y > 0 ? (float)vp.x / (float)vp.y : 1.f;
            glViewport(0, 0, vp.x, vp.y);
            glClearColor(0.008f, 0.01f, 0.03f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            sys.render(cam.view(), cam.projection(aspect), (float)vp.x, (float)vp.y, cam.fovY);

            win.swapBuffers();

            fpsAccum += elapsed;
            ++fpsFrames;
            if (fpsAccum > 0.5) {
                char title[160];
                std::snprintf(title, sizeof(title),
                              "ember — fireworks | particles: %u/%u | %.0f fps",
                              sys.aliveCount(), sys.capacity(), fpsFrames / fpsAccum);
                win.setTitle(title);
                fpsAccum = 0.0;
                fpsFrames = 0;
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
