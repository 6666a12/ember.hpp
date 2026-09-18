// Reproducible workload + raw measurements. See benchmarks/README.md for scope.
#include "ember/particle_system.hpp"
#include "ember/glfw_window.hpp"
#include "ember_benchmark_hooks.hpp"
#include "benchmark_build_info.hpp"
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace bench {
using Clock = std::chrono::steady_clock;
using ember::benchmark::Stage;
constexpr int stageCount = static_cast<int>(Stage::Count);
constexpr float dt = 1.f / 60.f;
constexpr int burstPeriod = 120;
const std::vector<std::string> scenarios = {
    "dense", "forces", "churn", "burst", "sparse", "sparse_control",
    "sort", "bloom", "refraction_simple", "refraction_depth", "refraction_noise"
};

double milliseconds(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
std::string quote(const std::string& s) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c);
        else out << c;
    }
    out << '"';
    return out.str();
}
std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string item;
    while (std::getline(in, item, ',')) {
        if (item.empty()) throw std::runtime_error("empty list item");
        out.push_back(item);
    }
    if (out.empty() || s.back() == ',') throw std::runtime_error("empty list item");
    return out;
}
int number(const std::string& s, int lo, int hi) {
    std::size_t used = 0;
    long long n = std::stoll(s, &used);
    if (used != s.size() || n < lo || n > hi) throw std::runtime_error("invalid numeric argument: " + s);
    return static_cast<int>(n);
}
struct Options {
    int width = 1280, height = 720, frames = 240, warmup = 120, repeats = 3;
    std::uint32_t seed = 20260917;
    std::vector<int> counts{10000, 100000, 1000000};
    std::vector<std::string> selected = scenarios;
    std::string output = "benchmark.json", shaderDir = "shaders";
    bool gpuTimers = true, gpuDriven = false;
};
Options options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help") {
            std::cout << "ember_benchmark_" << EMBER_BENCH_VARIANT
                << " [--output FILE] [--counts 10000,100000,1000000]\n"
                << "  [--scenarios dense,forces,churn,burst,sparse,sparse_control,sort,bloom,\n"
                << "               refraction_simple,refraction_depth,refraction_noise]\n"
                << "  [--frames 240] [--warmup 120] [--repeats 3] [--seed 20260917]\n"
                << "  [--width 1280] [--height 720] [--shader-dir shaders] [--gpu-timers on|off]\n"
                << "  [--scheduling sync|gpu]\n";
            std::exit(0);
        }
        if (++i == argc) throw std::runtime_error("missing value: " + key);
        const std::string value = argv[i];
        if (key == "--output") o.output = value;
        else if (key == "--shader-dir") o.shaderDir = value;
        else if (key == "--width") o.width = number(value, 16, 8192);
        else if (key == "--height") o.height = number(value, 16, 8192);
        else if (key == "--frames") o.frames = number(value, 1, 10000);
        else if (key == "--warmup") o.warmup = number(value, 1, 10000);
        else if (key == "--repeats") o.repeats = number(value, 1, 100);
        else if (key == "--seed") o.seed = number(value, 1, std::numeric_limits<int>::max());
        else if (key == "--counts") {
            o.counts.clear();
            for (const auto& s : split(value)) o.counts.push_back(number(s, 100, 1000000));
        } else if (key == "--scenarios") {
            o.selected = split(value);
            for (const auto& s : o.selected)
                if (std::find(scenarios.begin(), scenarios.end(), s) == scenarios.end())
                    throw std::runtime_error("unknown scenario: " + s);
        } else if (key == "--gpu-timers" && (value == "on" || value == "off")) {
            o.gpuTimers = value == "on";
        } else if (key == "--scheduling" && (value == "sync" || value == "gpu")) {
            o.gpuDriven = value == "gpu";
        } else throw std::runtime_error("unknown option/value: " + key + " " + value);
    }
    if (std::getenv("EMBER_DEBUG")) throw std::runtime_error("unset EMBER_DEBUG before benchmarking");
    // Fail rather than silently exercising a fallback shader or disabled bloom.
    for (const auto& f : {"simulate.comp", "particle.vert", "particle.frag", "sort.comp",
                          "bloom.vert", "bloom_threshold.frag", "bloom_blur.frag", "bloom_composite.frag"})
        if (!std::filesystem::is_regular_file(std::filesystem::path(o.shaderDir) / f))
            throw std::runtime_error("missing benchmark shader: " + std::string(f));
    return o;
}

struct Sample {
    std::array<double, stageCount> cpu{}, gpu{};
    std::array<Clock::time_point, stageCount> start{};
    std::array<bool, stageCount> used{};
    GLuint lastQuery = 0;
    unsigned alive = 0;
    int frame = 0;
    bool collected = false;
};
bool gpuStage(Stage s) { return s != Stage::Readback && s != Stage::Update; }

// Query objects are preallocated for the whole sample window. An unavailable
// result is NEVER read or overwritten while recording. Collect >= 4 frames
// later; outstanding results are drained only after the measurement window.
struct Recorder {
    std::vector<Sample> samples;
    std::vector<GLuint> queries;
    int current = -1, nextCollect = 0;
    bool gpuTimers;
    explicit Recorder(int frames, bool timers) : samples(frames), gpuTimers(timers) {
        if (timers) {
            queries.resize(static_cast<std::size_t>(frames) * stageCount * 2);
            glGenQueries(static_cast<GLsizei>(queries.size()), queries.data());
        }
    }
    ~Recorder() { if (!queries.empty()) glDeleteQueries(static_cast<GLsizei>(queries.size()), queries.data()); }
    GLuint query(int frame, int stage, int end) const {
        return queries[(static_cast<std::size_t>(frame) * stageCount + stage) * 2 + end];
    }
    void begin(Stage stage) {
        if (current < 0) return;
        const int i = static_cast<int>(stage);
        auto& s = samples[current];
        if (s.used[i]) throw std::runtime_error("duplicate benchmark stage");
        s.used[i] = true;
        s.start[i] = Clock::now();
        if (gpuTimers && gpuStage(stage)) glQueryCounter(query(current, i, 0), GL_TIMESTAMP);
    }
    void end(Stage stage) {
        if (current < 0) return;
        const int i = static_cast<int>(stage);
        if (gpuTimers && gpuStage(stage)) {
            samples[current].lastQuery = query(current, i, 1);
            glQueryCounter(samples[current].lastQuery, GL_TIMESTAMP);
        }
        samples[current].cpu[i] = milliseconds(samples[current].start[i], Clock::now());
    }
    void collect(bool drained) {
        const int limit = drained ? static_cast<int>(samples.size()) : current - 3;
        while (nextCollect < limit) {
            auto& s = samples[nextCollect];
            if (gpuTimers) {
                GLint ready = GL_FALSE;
                glGetQueryObjectiv(s.lastQuery, GL_QUERY_RESULT_AVAILABLE, &ready);
                if (!ready) {
                    if (drained) throw std::runtime_error("timer result unavailable after final drain");
                    break;
                }
                for (int i = 0; i < stageCount; ++i) {
                    if (!s.used[i] || !gpuStage(static_cast<Stage>(i))) continue;
                    GLuint64 a = 0, b = 0;
                    glGetQueryObjectui64v(query(nextCollect, i, 0), GL_QUERY_RESULT, &a);
                    glGetQueryObjectui64v(query(nextCollect, i, 1), GL_QUERY_RESULT, &b);
                    if (b < a) throw std::runtime_error("non-monotonic GPU timestamp");
                    s.gpu[i] = static_cast<double>(b - a) / 1e6;
                }
            }
            s.collected = true;
            ++nextCollect;
        }
    }
};
Recorder* recorder = nullptr;

void glCheck(const char* where) {
    if (ember::gl::popErrors(where)) throw std::runtime_error(std::string("GL error: ") + where);
}

struct Target {
    GLuint fbo = 0, color = 0, depth = 0, sceneColor = 0, sceneDepth = 0;
    int width, height;
    glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 12), glm::vec3(0), glm::vec3(0, 1, 0));
    glm::mat4 proj;
    Target(int w, int h) : width(w), height(h),
        proj(glm::perspective(glm::radians(60.f), float(w) / h, .1f, 100.f)) {
        glGenFramebuffers(1, &fbo);
        glGenTextures(1, &color);
        glBindTexture(GL_TEXTURE_2D, color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenRenderbuffers(1, &depth);
        glBindRenderbuffer(GL_RENDERBUFFER, depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("benchmark framebuffer incomplete");

        std::vector<unsigned char> pixels(static_cast<std::size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const auto p = (static_cast<std::size_t>(y) * w + x) * 4;
            pixels[p] = static_cast<unsigned char>(255 * x / (w - 1));
            pixels[p + 1] = static_cast<unsigned char>(255 * y / (h - 1));
            pixels[p + 2] = ((x / 16 + y / 16) % 2) ? 220 : 40;
            pixels[p + 3] = 255;
        }
        glGenTextures(1, &sceneColor);
        glBindTexture(GL_TEXTURE_2D, sceneColor);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        textureParams(GL_LINEAR);
        const auto clip = proj * glm::vec4(0, 0, -16, 1);
        std::vector<float> depths(static_cast<std::size_t>(w) * h, (clip.z / clip.w + 1.f) * .5f);
        glGenTextures(1, &sceneDepth);
        glBindTexture(GL_TEXTURE_2D, sceneDepth);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, w, h, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depths.data());
        textureParams(GL_NEAREST);
        glCheck("target construction");
    }
    static void textureParams(GLint filter) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    ~Target() {
        const GLuint textures[] = {color, sceneColor, sceneDepth};
        glDeleteTextures(3, textures);
        glDeleteRenderbuffers(1, &depth);
        glDeleteFramebuffers(1, &fbo);
    }
    void clear() {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearDepth(1.0);
        glClearColor(.02f, .025f, .04f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    void render(ember::ParticleSystem& sys) {
        sys.render(view, proj, float(width), float(height), 60.f);
    }
    unsigned changedPixels() {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        std::vector<float> pixels(static_cast<std::size_t>(width) * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data());
        unsigned changed = 0;
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            for (int c = 0; c < 4; ++c)
                if (!std::isfinite(pixels[i + c])) throw std::runtime_error("nonfinite benchmark image");
            if (std::abs(pixels[i] - .02f) + std::abs(pixels[i + 1] - .025f) +
                std::abs(pixels[i + 2] - .04f) > .002f) ++changed;
        }
        return changed;
    }
};

ember::Emitter cloud(bool refractive) {
    ember::Emitter e;
    e.shape = ember::Emitter::Shape::Box;
    e.extents = {6.f, 3.f, 2.f};
    e.rate = 0;
    e.speedMin = e.speedMax = 0;
    e.lifeMin = e.lifeMax = 1000000.f;
    e.sizeMin = e.sizeMax = .018f;
    e.colorMin = e.colorMax = {2.f, 1.f, .5f, .25f};
    e.refractive = refractive;
    return e;
}
// Batch initialization avoids making an enormous single append dispatch part
// of setup; all setup and shader compilation are excluded from samples.
void fill(ember::ParticleSystem& sys, ember::Emitter e, unsigned n) {
    sys.addEmitter(e);
    while (n) {
        const unsigned batch = std::min(n, 10000u);
        sys.emitter(0)->accumulator = float(batch);
        sys.update(0);
        n -= batch;
    }
    sys.clearEmitters();
}
void step(ember::ParticleSystem& sys, const std::string& name, unsigned n, int frame) {
    if (name == "burst") {
        const int phase = frame % burstPeriod;
        // Fill over ten frames, then decay completely before the next burst.
        sys.emitter(0)->accumulator = phase < 10 ? float(n / 10 + (unsigned(phase) < n % 10)) : 0.f;
    }
    sys.update(dt);
}
void population(const std::string& name, unsigned n, unsigned alive) {
    if (alive > n) throw std::runtime_error("capacity overflow");
    if (name == "burst") return;
    const unsigned expected = (name == "sparse" || name == "sparse_control") ? n / 100 : n;
    if (name == "churn") {
        if (alive < n * .98 || alive > n) throw std::runtime_error("churn did not reach steady state");
    } else if (alive != expected) throw std::runtime_error("unexpected live count in " + name);
}

void runCase(std::ostream& out, const Options& o, Target& target,
             const std::string& name, unsigned n, int repeat) {
    std::cerr << EMBER_BENCH_VARIANT << " " << name << " " << n << " repeat " << repeat + 1 << std::endl;
    ember::ParticleSystem sys({n, std::max(10000u, (n + 9) / 10), o.seed});
    sys.setShaderDirectory(o.shaderDir.c_str());
    sys.setForceMask(0);
    sys.setUseSprite(false);
    sys.setDepthTest(true);
    sys.setDepthWrite(false);
    sys.setBlendMode(name == "sort" ? ember::BlendMode::Normal : ember::BlendMode::Additive);
    sys.setSortEnabled(name == "sort");
    sys.setGpuDriven(o.gpuDriven);
    sys.setBloom(name == "bloom");
    sys.setBloomThreshold(.5f);
    const bool refractive = name.rfind("refraction_", 0) == 0;
    if (refractive) {
        auto r = ember::Refraction::glass();
        r.enabled = true;
        r.sceneColorTex = target.sceneColor;
        r.sceneDepthTex = target.sceneDepth;
        r.mode = name == "refraction_depth" ? 1 : name == "refraction_noise" ? 2 : 0;
        setOpenGLRefraction(sys,r);
        sys.setSpin(1.f);
    }
    auto e = cloud(refractive);
    if (name == "churn") {
        e.rate = float(n) / 2.f;
        e.lifeMin = e.lifeMax = 2.f + dt * .5f;
        sys.addEmitter(e);
        for (int f = 0; f < 240; ++f) sys.update(dt);
    } else if (name == "burst") {
        e.lifeMin = e.lifeMax = .5f;
        sys.addEmitter(e);
    } else if (name == "sparse" || name == "sparse_control") {
        fill(sys, e, n / 100);
        if (name == "sparse") {
            e.lifeMin = e.lifeMax = .05f;
            fill(sys, e, n - n / 100);
            sys.update(.1f);
        }
    } else fill(sys, e, n);
    if (name == "forces") {
        sys.setGravity({0.f, -.1f, 0.f});
        sys.setDrag(.05f);
        sys.setTurbulence(.25f);
        sys.setForceMask((1u << 0) | (1u << 1) | (1u << 3));
    }
    for (int f = 0; f < o.warmup; ++f) {
        target.clear();
        step(sys, name, n, f);
        target.render(sys);
    }
    if (name == "burst") {
        sys.clear();
        step(sys, name, n, 0);
        target.clear();
        target.render(sys);
    }
    const unsigned changed = target.changedPixels();
    if (!changed) throw std::runtime_error("empty benchmark rendering: " + name);
    if (name == "bloom" && !sys.bloom()) throw std::runtime_error("bloom fell back");
    if (name == "sort" && !sys.sortEnabled()) throw std::runtime_error("sort fell back");
    if (name == "burst") sys.clear();
    population(name, n, sys.aliveCount());
    Recorder rec(o.frames, o.gpuTimers);
    // Record exact per-frame counts with GPU copies, then read once after the
    // completed window. Calling aliveCount here would serialize GPU scheduling.
    ember::Buffer history(GL_COPY_WRITE_BUFFER);
    history.data(nullptr,GLsizeiptr(o.frames)*5*sizeof(GLuint),GL_STREAM_READ);
    glFinish(); // setup/warmup boundary only
    glCheck("preflight");
    const auto windowStart = Clock::now();
    for (int f = 0; f < o.frames; ++f) {
        target.clear(); // excluded from per-frame spans, included in window throughput
        rec.current = f;
        recorder = &rec;
        rec.samples[f].frame = f;
        rec.begin(Stage::Frame);
        rec.begin(Stage::Update);
        step(sys, name, n, f);
        rec.end(Stage::Update);
        rec.begin(Stage::Render);
        target.render(sys);
        rec.end(Stage::Render);
        rec.end(Stage::Frame);
        recorder = nullptr;
        GLint counters=0;
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING,4,&counters);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_COPY_READ_BUFFER,GLuint(counters));
        glBindBuffer(GL_COPY_WRITE_BUFFER,history.id());
        glCopyBufferSubData(GL_COPY_READ_BUFFER,GL_COPY_WRITE_BUFFER,0,
                           GLsizeiptr(f)*5*sizeof(GLuint),5*sizeof(GLuint));
        glFlush(); // offscreen submission; no swap, no vsync
        rec.collect(false);
    }
    glFinish(); // include completion of last frame in aggregate window throughput
    const double wall = milliseconds(windowStart, Clock::now());
    std::vector<GLuint> counts(static_cast<std::size_t>(o.frames)*5);
    history.getSubData(0,GLsizeiptr(counts.size()*sizeof(GLuint)),counts.data());
    for(int f=0;f<o.frames;++f) {
        rec.samples[f].alive=counts[std::size_t(f)*5];
        population(name,n,rec.samples[f].alive);
    }
    rec.collect(true);
    glCheck("measured window");
    out << "{\"scenario\":" << quote(name) << ",\"capacity\":" << n << ",\"repeat\":" << repeat
        << ",\"preflight_changed_pixels\":" << changed
        << ",\"dropped_statistics\":" << sys.droppedStatistics()
        << ",\"window_wall_ms\":" << wall << ",\"samples\":[";
    for (int f = 0; f < o.frames; ++f) {
        const auto& s = rec.samples[f];
        if (!s.collected) throw std::runtime_error("missing sample");
        if (f) out << ',';
        out << "{\"frame\":" << f << ",\"alive\":" << s.alive;
        if (name == "burst") out << ",\"cycle_frame\":" << f % burstPeriod;
        for (int i = 0; i < stageCount; ++i) {
            if (!s.used[i]) continue;
            const std::string suffix = ember::benchmark::stageNames[i];
            out << ',' << quote("cpu_" + suffix + "_ms") << ':' << s.cpu[i];
            if (o.gpuTimers && gpuStage(static_cast<Stage>(i))) {
                const bool span = i == int(Stage::Frame) || i == int(Stage::Render);
                out << ',' << quote("gpu_" + suffix + (span ? "_span_ms" : "_ms")) << ':' << s.gpu[i];
            }
        }
        out << '}';
    }
    out << "]}";
    out.flush();
}
} // namespace bench

namespace ember::benchmark {
void beginStage(Stage stage) { if (bench::recorder) bench::recorder->begin(stage); }
void endStage(Stage stage) { if (bench::recorder) bench::recorder->end(stage); }
}

int main(int argc, char** argv) {
    try {
        const auto o = bench::options(argc, argv);
        ember::Window window(64, 64, "ember benchmark", 0, false, false);
        GLint timestampBits = 0;
        glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &timestampBits);
        if (o.gpuTimers && timestampBits < 64) throw std::runtime_error("64-bit timestamp queries required");
        bench::Target target(o.width, o.height);
        std::ofstream out(o.output, std::ios::binary);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out << std::setprecision(9);
        out << "{\"schema_version\":1,\"workload_version\":1,\"measurement_version\":2,\"backend\":\"opengl\","
            << "\"variant\":" << bench::quote(EMBER_BENCH_VARIANT)
            << ",\"scheduling\":" << bench::quote(o.gpuDriven?"gpu":"sync")
            << ",\"population_capture\":\"gpu_history_after_window\""
            << ",\"compiler\":" << bench::quote(EMBER_BENCH_COMPILER)
            << ",\"build_type\":" << bench::quote(EMBER_BENCH_BUILD_TYPE)
            << ",\"system\":" << bench::quote(EMBER_BENCH_SYSTEM)
            << ",\"gl_vendor\":" << bench::quote(reinterpret_cast<const char*>(glGetString(GL_VENDOR)))
            << ",\"gl_renderer\":" << bench::quote(reinterpret_cast<const char*>(glGetString(GL_RENDERER)))
            << ",\"gl_version\":" << bench::quote(reinterpret_cast<const char*>(glGetString(GL_VERSION)))
            << ",\"timestamp_bits\":" << timestampBits << ",\"gpu_timers\":" << (o.gpuTimers ? "true" : "false")
            << ",\"width\":" << o.width << ",\"height\":" << o.height << ",\"seed\":" << o.seed
            << ",\"dt_seconds\":" << bench::dt << ",\"warmup_frames\":" << o.warmup
            << ",\"measured_frames\":" << o.frames << ",\"repeats\":" << o.repeats
            << ",\"samples\":1,\"target_format\":\"RGBA16F+D24\",\"presentation\":\"offscreen_no_swap\""
            << ",\"cases\":[";
        bool first = true;
        // Reverse scenario order on alternating repeats to reduce fixed-order
        // warmup/thermal bias. Every case creates a fresh, identically seeded system.
        for (int r = 0; r < o.repeats; ++r) {
            auto selected = o.selected;
            if (r % 2) std::reverse(selected.begin(), selected.end());
            for (int n : o.counts) for (const auto& name : selected) {
                window.pollEvents();
                if (!first) out << ',';
                first = false;
                bench::runCase(out, o, target, name, n, r);
            }
        }
        out << "]}\n";
        return 0;
    } catch (const std::exception& e) {
        bench::recorder = nullptr;
        std::cerr << "benchmark FAILED: " << e.what() << '\n';
        return 1; // unsupported context is a failed run, never a successful baseline
    }
}
