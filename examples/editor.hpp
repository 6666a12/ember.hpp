#pragma once

// Interactive emitter ("particle source") editor for the example programs.
//
// Compiled in only when EMBER_EDITOR_ENABLED is defined (CMake option
// EMBER_BUILD_EDITOR, default ON, applies to the examples only). The library
// itself never contains the editor. When the macro is absent every function
// degrades to a no-op so example code links unchanged.
//
// Keys (window must have focus):
//   1-9        select emitter N (title bar shows name + params)
//   WASD       move on the camera-facing horizontal plane
//   Space/Shift  move up / down
//   Right-drag or right-click  place selected emitter at the cursor
//   [ ]        scale shape size (cone/sphere radius, box extents)
//   , .        cone angle +/- (cone emission)
//   - =        rate (spawns per second)
//   ; '        size_min/max (particle size range)
//   P          print the selected emitter as config lines
//   T          toggle sprite texture vs procedural glow
//   B          toggle bloom post-processing
//   O          toggle depth sort (OIT; correct alpha blending)
//
// Runtime toggle: [system] editor = false in the config file (hot-reloadable).

#include "ember/camera.hpp"
#include "ember/glfw_window.hpp"
#include "ember/particle_system.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ember_example {

// Unproject the cursor onto the horizontal plane y = py.
inline glm::vec3 rayPlanePoint(const ember::OrbitCamera& cam, glm::vec2 ndc, float aspect, float py) {
    const glm::mat4 invVP = glm::inverse(cam.projection(aspect) * cam.view());
    const glm::vec4 n = invVP * glm::vec4(ndc, -1.f, 1.f);
    const glm::vec4 f = invVP * glm::vec4(ndc, 1.f, 1.f);
    const glm::vec3 pn = glm::vec3(n) / n.w;
    const glm::vec3 pf = glm::vec3(f) / f.w;
    const glm::vec3 o = cam.position();
    const glm::vec3 dir = glm::normalize(pf - pn);
    const float t = std::abs(dir.y) > 1e-4f ? (py - o.y) / dir.y : 0.f;
    return o + dir * t;
}

#ifdef EMBER_EDITOR_ENABLED

struct EmitterEditor {
    int selected = 0;
    bool enabled = true;

    void update(ember::ParticleSystem& sys, ember::Window& win, ember::OrbitCamera& cam, float dt) {
        if (!enabled || sys.emitterCount() == 0) return;
        if (selected >= (int)sys.emitterCount()) selected = (int)sys.emitterCount() - 1;
        ember::Emitter* e = sys.emitter((std::size_t)selected);
        if (!e) return;

        // selection 1..9
        for (int k = 0; k < 9; ++k) {
            if (win.key(GLFW_KEY_1 + k) && k < (int)sys.emitterCount()) {
                if (selected != k) {
                    selected = k;
                    e = sys.emitter((std::size_t)selected);
                    std::printf("[editor] emitter %d selected\n", selected);
                }
                break;
            }
        }
        if (!e) return;

        // camera-facing movement basis (horizontal)
        glm::vec3 fwd = glm::normalize(glm::vec3(cam.target.x - cam.position().x, 0.f,
                                                 cam.target.z - cam.position().z));
        glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.f, 1.f, 0.f)));
        const float spd = dt * 8.f;
        glm::vec3 d{0.f};
        if (win.key(GLFW_KEY_W)) d += fwd;
        if (win.key(GLFW_KEY_S)) d -= fwd;
        if (win.key(GLFW_KEY_A)) d -= right;
        if (win.key(GLFW_KEY_D)) d += right;
        if (win.key(GLFW_KEY_SPACE)) d.y += 1.f;
        if (win.key(GLFW_KEY_LEFT_SHIFT)) d.y -= 1.f;
        if (d != glm::vec3(0.f)) {
            d = glm::normalize(d) * spd;
            e->moveBy(d);
        }

        // right-click: place on the plane at the emitter's current height
        if (win.mouseButton(GLFW_MOUSE_BUTTON_RIGHT) && !wasRight_) {
            const glm::ivec2 vp = win.framebufferSize();
            if (vp.x > 0 && vp.y > 0) {
                const glm::vec2 cur = win.cursor();
                const float aspect = (float)vp.x / (float)vp.y;
                const glm::vec2 ndc(cur.x / (float)vp.x * 2.f - 1.f, 1.f - cur.y / (float)vp.y * 2.f);
                e->position = rayPlanePoint(cam, ndc, aspect, e->position.y);
                std::printf("[editor] emitter %d placed at (%.2f, %.2f, %.2f)\n",
                            selected, e->position.x, e->position.y, e->position.z);
            }
        }
        wasRight_ = win.mouseButton(GLFW_MOUSE_BUTTON_RIGHT);

        // shape / cone / rate / size tweaks (hold to accelerate)
        if (win.key(GLFW_KEY_LEFT_BRACKET)) e->scaleShape(std::exp(dt * 1.5f));
        if (win.key(GLFW_KEY_RIGHT_BRACKET)) e->scaleShape(std::exp(-dt * 1.5f));
        if (win.key(GLFW_KEY_COMMA)) e->coneAngle = std::max(0.f, e->coneAngle - 45.f * dt);
        if (win.key(GLFW_KEY_PERIOD)) e->coneAngle = std::min(120.f, e->coneAngle + 45.f * dt);
        if (win.key(GLFW_KEY_MINUS)) e->rate = std::max(0.f, e->rate * (1.f - 2.f * dt));
        if (win.key(GLFW_KEY_EQUAL)) e->rate *= (1.f + 2.f * dt);
        if (win.key(GLFW_KEY_SEMICOLON)) e->setSizeRange(e->sizeMin * 0.995f, e->sizeMax * 0.995f);
        if (win.key(GLFW_KEY_APOSTROPHE)) e->setSizeRange(e->sizeMin * 1.005f, e->sizeMax * 1.005f);

        // P: print config lines (paste straight into the .ini)
        if (win.key(GLFW_KEY_P) && !wasP_) {
            const char* shape =
                e->shape == ember::Emitter::Shape::Point ? "point"
                : e->shape == ember::Emitter::Shape::Box ? "box"
                : e->shape == ember::Emitter::Shape::Sphere ? "sphere" : "cone";
            std::printf("\n[emitter \"emitter%d\"]\n", selected);
            std::printf("shape          = %s\n", shape);
            std::printf("position       = %.3f, %.3f, %.3f\n", e->position.x, e->position.y, e->position.z);
            std::printf("axis           = %.3f, %.3f, %.3f\n", e->axis.x, e->axis.y, e->axis.z);
            std::printf("radius         = %.3f\n", e->radius);
            std::printf("rate           = %.0f\n", e->rate);
            std::printf("base_velocity  = %.3f, %.3f, %.3f\n",
                        e->baseVelocity.x, e->baseVelocity.y, e->baseVelocity.z);
            std::printf("speed_min      = %.3f\nspeed_max      = %.3f\n", e->speedMin, e->speedMax);
            std::printf("spread         = %.2f\n", e->spread);
            std::printf("cone_angle     = %.1f\n", e->coneAngle);
            std::printf("life_min       = %.2f\nlife_max       = %.2f\n", e->lifeMin, e->lifeMax);
            std::printf("size_min       = %.3f\nsize_max       = %.3f\n", e->sizeMin, e->sizeMax);
            std::printf("speed_scale    = %.2f\n", e->speedScale);
            std::printf("speed_size_link= %.2f\n", e->speedSizeLink);
            std::printf("color_min      = %.2f, %.2f, %.2f, %.2f\n",
                        e->colorMin.r, e->colorMin.g, e->colorMin.b, e->colorMin.a);
            std::printf("color_max      = %.2f, %.2f, %.2f, %.2f\n",
                        e->colorMax.r, e->colorMax.g, e->colorMax.b, e->colorMax.a);
            std::printf("\n");
        }
        wasP_ = win.key(GLFW_KEY_P);

        // T: toggle sprite texture vs procedural glow
        if (win.key(GLFW_KEY_T) && !wasT_) {
            sys.setUseSprite(!sys.useSprite());
            std::printf("[editor] sprite %s\n", sys.useSprite() ? "texture" : "procedural");
        }
        wasT_ = win.key(GLFW_KEY_T);

        // B: toggle bloom
        if (win.key(GLFW_KEY_B) && !wasB_) {
            sys.setBloom(!sys.bloom());
            std::printf("[editor] bloom %s\n", sys.bloom() ? "ON" : "OFF");
        }
        wasB_ = win.key(GLFW_KEY_B);

        // O: toggle depth sort (correct alpha blending of overlapping quads)
        if (win.key(GLFW_KEY_O) && !wasO_) {
            sys.setSortEnabled(!sys.sortEnabled());
            std::printf("[editor] depth sort %s\n", sys.sortEnabled() ? "ON" : "OFF");
        }
        wasO_ = win.key(GLFW_KEY_O);
    }

private:
    bool wasRight_ = false;
    bool wasP_ = false;
    bool wasT_ = false;
    bool wasB_ = false;
    bool wasO_ = false;
};

#else // !EMBER_EDITOR_ENABLED

struct EmitterEditor {
    bool enabled = true; // kept for API compatibility; no-op when compiled out
    void update(ember::ParticleSystem&, ember::Window&, ember::OrbitCamera&, float) {}
};

#endif

} // namespace ember_example
