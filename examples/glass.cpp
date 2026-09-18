// ember example: glass shards — screen-space refraction.
//
// The scene background (an image by default, see kBackgroundPath) is rendered
// into a texture each frame; refractive particles sample it with per-particle
// facet offsets, so every shard bends the background like a small prism.
//
//   left-drag  orbit camera      scroll  zoom
//   G          cycle refraction preset (glass / heat / water / prism)
//   SPACE      burst of 2000 refractive shards
//   B          toggle bloom      R  hot-reload config/glass.ini
//   ESC        quit
//
// Usage: example_glass [config.ini] [background.jpg]

#include "ember/camera.hpp"
#include "ember/config.hpp"
#include "ember/glfw_window.hpp"
#include "ember/particle_system.hpp"

#include "stb_image.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace ember;

namespace {

const char* kDefaultBackground = "Image184.jpg";

// Renders a background (image, aspect-fitted; fallback: gradient + bright
// props) into a color texture that the refractive particles sample. This is
// the minimal "host scene" contract: render your own scene to a texture, pass
// its id to setRefraction().
struct ScenePass {
    Shader prog;            // background geometry (fallback gradient + props)
    Shader bgProg;          // fullscreen image sampler (aspect fit)
    Buffer vbo{GL_ARRAY_BUFFER};
    VertexArray vao;
    Texture colorTex;       // the scene texture (refraction source + visible bg)
    Texture bgTex;          // loaded background image
    GLuint fbo = 0;
    ~ScenePass() { if (fbo) glDeleteFramebuffers(1, &fbo); }
    int w = 0, h = 0;
    int bgW = 0, bgH = 0;
    bool bgLoaded = false;

    void init() {
        prog = Shader::fromSources({
            {GL_VERTEX_SHADER,
             "#version 430 core\nlayout(location=0) in vec2 aPos; layout(location=1) in vec3 aCol;"
             " out vec3 vCol; void main(){ vCol = aCol; gl_Position = vec4(aPos, 0.0, 1.0); }"},
            {GL_FRAGMENT_SHADER, "#version 430 core\nin vec3 vCol; out vec4 frag; void main(){ frag = vec4(vCol, 1.0); }"},
        });
        bgProg = Shader::fromSources({
            {GL_VERTEX_SHADER,
             "#version 430 core\n"
             "out vec2 vUV; void main(){"
             " vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));"
             " vUV = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }"},
            {GL_FRAGMENT_SHADER,
             "#version 430 core\n"
             "in vec2 vUV; out vec4 frag; uniform sampler2D uTex; uniform vec4 uRect; uniform int uFlip;"
             " void main(){"
             "  vec2 ndc = vUV * 2.0 - 1.0;"
             "  vec2 uv = (ndc - uRect.xy) / uRect.zw + 0.5;"   // letterbox rect in NDC
             "  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) discard;"
             "  float fy = uFlip != 0 ? 1.0 - uv.y : uv.y;"
             "  frag = vec4(texture(uTex, vec2(uv.x, fy)).rgb, 1.0); }"}, // uFlip: stbi images are top-down
        });
        // Fallback scene: gradient background + bright props (the glass bends these).
        const float v[] = {
            // fullscreen gradient: dark blue top -> near-black bottom
            -1.f,  1.f, 0.06f, 0.10f, 0.22f,   -1.f, -1.f, 0.02f, 0.02f, 0.05f,   1.f,  1.f, 0.06f, 0.10f, 0.22f,
             1.f,  1.f, 0.06f, 0.10f, 0.22f,   -1.f, -1.f, 0.02f, 0.02f, 0.05f,   1.f, -1.f, 0.02f, 0.02f, 0.05f,
            // prop 1: red bar (upper left)
            -0.70f,  0.70f, 1.f, 0.20f, 0.10f,   -0.50f,  0.70f, 1.f, 0.20f, 0.10f,   -0.70f,  0.55f, 1.f, 0.20f, 0.10f,
            -0.50f,  0.70f, 1.f, 0.20f, 0.10f,   -0.50f,  0.55f, 1.f, 0.20f, 0.10f,   -0.70f,  0.55f, 1.f, 0.20f, 0.10f,
            // prop 2: green bar (upper right)
             0.50f,  0.60f, 0.10f, 1.f, 0.30f,    0.70f,  0.60f, 0.10f, 1.f, 0.30f,    0.50f,  0.45f, 0.10f, 1.f, 0.30f,
             0.70f,  0.60f, 0.10f, 1.f, 0.30f,    0.70f,  0.45f, 0.10f, 1.f, 0.30f,    0.50f,  0.45f, 0.10f, 1.f, 0.30f,
            // prop 3: cyan bar (center-right)
             0.60f, -0.20f, 0.10f, 0.80f, 1.f,    0.80f, -0.20f, 0.10f, 0.80f, 1.f,    0.60f, -0.35f, 0.10f, 0.80f, 1.f,
             0.80f, -0.20f, 0.10f, 0.80f, 1.f,    0.80f, -0.35f, 0.10f, 0.80f, 1.f,    0.60f, -0.35f, 0.10f, 0.80f, 1.f,
        };
        vbo.data(v, sizeof(v), GL_STATIC_DRAW);
        vao.bind();
        vbo.bind();
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 5, nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 5, (void*)(2 * sizeof(float)));
        vao.unbind();
        glGenFramebuffers(1, &fbo);
    }

    // Load the background image (RGBA via stb_image). Returns false on failure
    // (the demo then falls back to the gradient + props scene).
    bool loadBackground(const char* path) {
        int ch = 0;
        unsigned char* data = stbi_load(path, &bgW, &bgH, &ch, 4);
        if (!data) {
            std::fprintf(stderr, "[glass] warning: cannot load background '%s' (%s); using fallback scene\n",
                         path, stbi_failure_reason());
            bgLoaded = false;
            return false;
        }
        bgTex.uploadRGBA8(bgW, bgH, data);
        stbi_image_free(data);
        bgLoaded = true;
        std::printf("[glass] background '%s' loaded (%dx%d)\n", path, bgW, bgH);
        return true;
    }

    void resize(int ww, int hh) {
        if (ww == w && hh == h) return;
        if (ww <= 0 || hh <= 0) return; // minimized / hidden
        w = ww;
        h = hh;
        colorTex.uploadRGBA8(w, h, nullptr); // CLAMP_TO_EDGE (set by the wrapper)
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex.id(), 0);
        const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            throw std::runtime_error("ember: scene framebuffer incomplete");
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // Render the background into the scene texture (the refraction source).
    void draw() {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, w, h);
        glDisable(GL_BLEND);
        glClearColor(0.f, 0.f, 0.f, 1.f); // letterbox strips stay black, not stale garbage
        glClear(GL_COLOR_BUFFER_BIT);
        if (bgLoaded) {
            // Aspect-fit the image into the scene texture (letterboxed).
            const float scale = std::min((float)w / (float)bgW, (float)h / (float)bgH);
            const float rw = bgW * scale, rh = bgH * scale; // fitted size in pixels
            const glm::vec4 rect(0.f, 0.f, 2.f * rw / (float)w, 2.f * rh / (float)h); // NDC center+size
            glUseProgram(bgProg.id());
            bgProg.setInt("uTex", 0);
            bgProg.setVec4("uRect", rect);
            bgProg.setInt("uFlip", 1); // stbi image rows are top-down
            glActiveTexture(GL_TEXTURE0);
            bgTex.bind();
            vao.bind();
            glDrawArrays(GL_TRIANGLES, 0, 3);
            vao.unbind();
        } else {
            glUseProgram(prog.id());
            vao.bind();
            glDrawArrays(GL_TRIANGLES, 0, 24);
            vao.unbind();
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // Draw the scene texture onto the default framebuffer (visible background).
    void drawBackground() {
        glViewport(0, 0, w, h);
        glDisable(GL_BLEND);
        glUseProgram(bgProg.id());
        bgProg.setInt("uTex", 0);
        bgProg.setVec4("uRect", {0.f, 0.f, 2.f, 2.f}); // full screen, full UV
        bgProg.setInt("uFlip", 0); // FBO textures are already bottom-up: no flip,
                                   // or the visible background would not match what
                                   // the refraction pass samples
        glActiveTexture(GL_TEXTURE0);
        colorTex.bind();
        vao.bind();
        glDrawArrays(GL_TRIANGLES, 0, 3);
        vao.unbind();
    }
};

std::string pickConfig(int argc, char** argv) {
    if (argc > 1) return argv[1];
    for (const char* p : {"config/glass.ini", "../config/glass.ini", "glass.ini"}) {
        std::ifstream f(p);
        if (f) return p;
    }
    return "config/glass.ini";
}

} // namespace

int main(int argc, char** argv) {
    try {
        Window win(1280, 720, "ember — glass shards (drag: orbit, G: preset, SPACE: burst)");
        OrbitCamera cam;
        cam.target = {0.f, 1.5f, 0.f};
        cam.distance = 11.f;
        cam.pitch = 0.5f;

        ParticleSystem sys({200000, 8000});
        sys.setGravity({0.f, -9.8f, 0.f});
        sys.setDrag(0.02f);
        sys.setBoundary(BoundaryMode::Bounce, 0.f, 0.25f);
        sys.setSpin(1.5f); // shards tumble

        std::string cfgPath = pickConfig(argc, argv);
        Config cfg;
        try {
            Config candidate = Config::fromFile(cfgPath.c_str());
            sys.apply(candidate);
            cfg = std::move(candidate);
            std::printf("[glass] loaded config: %s\n", cfgPath.c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[glass] warning: %s (running with defaults)\n", e.what());
        }

        ScenePass scene;
        scene.init();
        const char* bgPath = argc > 2 ? argv[2] : kDefaultBackground;
        scene.loadBackground(bgPath);

        auto applyPreset = [&](int idx, bool useConfigMode = false) {
            RefractionSettings s;
            switch (idx % 4) {
                case 0: s = Refraction::glass(); break;
                case 1: s = Refraction::heat();  break;
                case 2: s = Refraction::water(); break;
                case 3: s = Refraction::prism(); break;
            }
            s.enabled = cfg.has("system.refraction") ? cfg.system.refraction : true;
            s.sceneColorTex = scene.colorTex.id();
            // File load/reload honors the explicit mode; G deliberately
            // selects the preset's own mode (so the heat preset stays heat).
            if (useConfigMode && cfg.has("system.refraction_mode"))
                s.mode = cfg.system.refractionMode == "depth" ? 1 : cfg.system.refractionMode == "noise" ? 2 : 0;
            s.strength = cfg.has("system.refraction_strength") ? cfg.system.refractionStrength : s.strength; // the ini knob stays live (preset default otherwise)
            setOpenGLRefraction(sys,s);
            const char* name = idx % 4 == 0 ? "glass" : idx % 4 == 1 ? "heat" : idx % 4 == 2 ? "water" : "prism";
            std::printf("[glass] refraction preset: %s\n", name);
        };
        int preset = 0;
        applyPreset(preset, true);

        Rng rng;
        bool reload = false;
        win.onKey = [&](int key, int, int action, int) {
            if (key == GLFW_KEY_R && action == GLFW_PRESS) reload = true;
            if (key == GLFW_KEY_G && action == GLFW_PRESS) { applyPreset(++preset); }
            if (key == GLFW_KEY_B && action == GLFW_PRESS) {
                sys.setBloom(!sys.bloom());
                std::printf("[glass] bloom %s\n", sys.bloom() ? "ON" : "OFF");
            }
            if (key == GLFW_KEY_SPACE && action == GLFW_PRESS) {
                BurstParams b;
                b.count = 2000;
                b.position = {rng.range(-6.f, 6.f), rng.range(6.f, 10.f), rng.range(-6.f, 6.f)};
                b.speedMin = 2.f; b.speedMax = 7.f;
                b.spread = 1.f;
                b.lifeMin = 2.f; b.lifeMax = 4.f;
                b.sizeMin = 0.03f; b.sizeMax = 0.09f;
                b.refractive = true;
                sys.burst(b);
                std::printf("[glass] burst %u shards\n", b.count);
            }
        };

        glm::vec2 lastCursor = win.cursor();
        double fpsAccum = 0.0;
        int fpsFrames = 0;

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
                    applyPreset(preset, true);
                    std::printf("[glass] config reloaded\n");
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "[glass] reload failed, keeping previous config: %s\n", e.what());
                }
            }

            const glm::vec2 cur = win.cursor();
            if (win.mouseButton(GLFW_MOUSE_BUTTON_LEFT)) cam.orbit(cur.x - lastCursor.x, cur.y - lastCursor.y);
            cam.zoom(win.scrollDelta().y);
            lastCursor = cur;

            sys.update(dt);

            // Host scene: render the background into the refraction texture,
            // show it, then draw the particles (refractive pass replaces pixels).
            scene.resize(vp.x, vp.y);
            scene.draw();
            glViewport(0, 0, vp.x, vp.y);
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            scene.drawBackground();
            sys.render(cam.view(), cam.projection(vp.y > 0 ? (float)vp.x / (float)vp.y : 1.f),
                       (float)vp.x, (float)vp.y, cam.fovY);

            win.swapBuffers();

            fpsAccum += elapsed;
            ++fpsFrames;
            if (fpsAccum > 0.5) {
                char title[160];
                std::snprintf(title, sizeof(title), "ember — glass shards | %u/%u | %.0f fps",
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
