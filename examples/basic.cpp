// ember example: interactive fountain.
//
//   left-drag  orbit camera          scroll  zoom
//   mouse      attractor + spark trail follow the cursor (plane y = 0.6)
//   R          hot-reload config/example.ini
//   V          toggle vortex         T  toggle sprite texture
//   1-9/WASD/[ ]/, ./ - =/; '/P  emitter editor (EMBER_BUILD_EDITOR)
//   ESC        quit
//
// Usage: example_basic [path/to/config.ini]

#include "ember/camera.hpp"
#include "ember/config.hpp"
#include "ember/glfw_window.hpp"
#include "ember/particle_system.hpp"

#include "editor.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace ember;

namespace {

// Depth-only ground pass: fills a depth texture so soft particles fade near
// the ground plane (config: soft_particles = true).
struct GroundDepth {
    Shader prog;
    Buffer vbo{GL_ARRAY_BUFFER};
    VertexArray vao;
    Texture depthTex;
    GLuint fbo = 0;
    GLuint colorRb = 0; // dummy color attachment: some drivers reject depth-only FBOs
    ~GroundDepth() {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (colorRb) glDeleteRenderbuffers(1, &colorRb);
    }
    int w = 0, h = 0;

    void init() {
        prog = Shader::fromSources({
            {GL_VERTEX_SHADER,
             "#version 430 core\nlayout(location=0) in vec3 aPos; uniform mat4 uVP;"
             " void main(){ gl_Position = uVP * vec4(aPos, 1.0); }"},
            {GL_FRAGMENT_SHADER, "#version 430 core\nvoid main(){}"},
        });
        const float g = 60.f;
        const float verts[] = {-g, 0.f, -g, g, 0.f, -g, -g, 0.f, g, g, 0.f, g};
        vbo.data(verts, sizeof(verts), GL_STATIC_DRAW);
        vao.bind();
        vbo.bind();
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, nullptr);
        vao.unbind();
        glGenFramebuffers(1, &fbo);
        glGenRenderbuffers(1, &colorRb);
        // Do NOT set glDrawBuffer/glReadBuffer here: no FBO is bound yet, so the
        // calls would hit the *default* framebuffer and permanently disable all
        // rendering to the window. draw() sets them on the ground FBO each frame.
    }

    void resize(int ww, int hh) {
        if (ww == w && hh == h) return;
        if (ww <= 0 || hh <= 0) return; // minimized / hidden
        w = ww;
        h = hh;
        depthTex.uploadDepth(w, h);
        glBindRenderbuffer(GL_RENDERBUFFER, colorRb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorRb);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthTex.id(), 0);
        const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            throw std::runtime_error("ember: ground framebuffer incomplete");
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }

    void draw(const glm::mat4& vp) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE)
            std::fprintf(stderr, "[basic] ground FBO status 0x%X\n", st);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        glViewport(0, 0, w, h);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glUseProgram(prog.id());
        prog.setMat4("uVP", vp);
        vao.bind();
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        vao.unbind();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
};

std::string pickConfig(int argc, char** argv) {
    if (argc > 1) return argv[1];
    for (const char* p : {"config/example.ini", "../config/example.ini", "example.ini"}) {
        std::ifstream f(p);
        if (f) return p;
    }
    return "config/example.ini"; // will produce a clear error if missing
}

} // namespace

int main(int argc, char** argv) {
    try {
        Window win(1280, 720, "ember — fountain (drag: orbit, R: reload, V: vortex)");
        OrbitCamera cam;
        cam.target = {0.f, 2.5f, 0.f};
        cam.distance = 16.f;

        ParticleSystem sys({150000, 40000});
        sys.setGravity({0.f, -7.f, 0.f});
        sys.setDrag(0.05f);

        std::string cfgPath = pickConfig(argc, argv);
        Config cfg;
        ember_example::EmitterEditor editor;
        try {
            Config candidate = Config::fromFile(cfgPath.c_str());
            sys.apply(candidate);
            cfg = std::move(candidate);
            editor.enabled = cfg.system.editor;
            std::printf("[basic] loaded config: %s\n", cfgPath.c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[basic] warning: %s (running with defaults)\n", e.what());
        }

        std::vector<Attractor> attractors = {{{0.f, 0.6f, 0.f}, 60.f}};
        sys.setAttractors(attractors);

        // Soft particles: host provides a depth texture (ground plane).
        GroundDepth ground;
        ground.init();
        setOpenGLSoftParticles(sys,cfg.system.softParticles, ground.depthTex.id(), cfg.system.softRadius);

        glm::vec2 lastCursor = win.cursor();
        bool reload = false;
        win.onKey = [&](int key, int, int action, int) {
            if (key == GLFW_KEY_R && action == GLFW_PRESS) reload = true;
            if (key == GLFW_KEY_V && action == GLFW_PRESS) {
                sys.setForceEnabled(Force::Vortex, !sys.forceEnabled(Force::Vortex));
                std::printf("[basic] vortex %s\n", sys.forceEnabled(Force::Vortex) ? "ON" : "OFF");
            }
        };

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
                    if (cfg.has("system.editor")) editor.enabled = cfg.system.editor;
                    std::printf("[basic] config reloaded\n");
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "[basic] reload failed, keeping previous config: %s\n", e.what());
                }
            }

            const glm::vec2 cur = win.cursor();
            if (win.mouseButton(GLFW_MOUSE_BUTTON_LEFT)) cam.orbit(cur.x - lastCursor.x, cur.y - lastCursor.y);
            cam.zoom(win.scrollDelta().y);
            lastCursor = cur;

            editor.update(sys, win, cam, dt);

            // Mouse position on the interaction plane -> attractor + trail emitter.
            const float aspect = vp.y > 0 ? (float)vp.x / (float)vp.y : 1.f;
            const glm::vec2 ndc(cur.x / (float)windowSize.x * 2.f - 1.f, 1.f - cur.y / (float)windowSize.y * 2.f);
            glm::vec3 mouse;
            if (ember_example::rayPlanePoint(cam, ndc, aspect, 0.6f, mouse)) {
                attractors[0].position = mouse;
                sys.setAttractors(attractors);
                if (auto* trail = sys.emitter(1)) trail->position = mouse;
            }

            sys.update(dt);

            // Ground depth pass (for soft particles), then the particles.
            ground.resize(vp.x, vp.y);
            ground.draw(cam.projection(aspect) * cam.view());
            if (glGetError() != GL_NO_ERROR) std::fprintf(stderr, "[basic] error after ground.draw\n");
            while (glGetError() != GL_NO_ERROR) {}

            glViewport(0, 0, vp.x, vp.y);
            glClearColor(0.02f, 0.02f, 0.04f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            sys.render(cam.view(), cam.projection(aspect), (float)vp.x, (float)vp.y, cam.fovY);
            if (glGetError() != GL_NO_ERROR) std::fprintf(stderr, "[basic] error after sys.render\n");
            while (glGetError() != GL_NO_ERROR) {}

            win.swapBuffers();

            fpsAccum += elapsed;
            ++fpsFrames;
            if (fpsAccum > 0.5) {
                char title[160];
                std::snprintf(title, sizeof(title),
                              "ember — fountain | particles: %u/%u | %.0f fps",
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
