#include "ember/glfw_window.hpp"

#include "ember/gl.hpp"

#include <stdexcept>
#include <exception>

namespace ember {

namespace {
thread_local std::exception_ptr g_callbackError;
template<class F> void callback(F&& f) noexcept {
    try { f(); } catch (...) { if (!g_callbackError) g_callbackError = std::current_exception(); }
}
int g_glfwRefs = 0; // refcount for glfwInit/glfwTerminate
[[noreturn]] void contextFailure(int code, const char* message) {
    if (code == GLFW_API_UNAVAILABLE || code == GLFW_VERSION_UNAVAILABLE ||
        code == GLFW_PLATFORM_UNAVAILABLE || code == GLFW_PLATFORM_ERROR ||
        code == GLFW_FORMAT_UNAVAILABLE)
        throw ContextUnavailable(message);
    throw std::runtime_error(message);
}
}

Window::Window(int width, int height, const char* title, int samples, bool vsync, bool visible) {
    if (g_glfwRefs++ == 0) {
        if (!glfwInit()) {
            const int error = glfwGetError(nullptr);
            --g_glfwRefs;
            contextFailure(error, "ember: glfwInit failed");
        }
    }
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, samples);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE); // (GL 4.1 on macOS — compute shaders need 4.3)
#endif
    w_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!w_) {
        const int error = glfwGetError(nullptr);
        if (--g_glfwRefs == 0) glfwTerminate();
        contextFailure(error, "ember: failed to create GLFW window (needs OpenGL 4.3 core)");
    }
    glfwMakeContextCurrent(w_);
    if (!gl::init(glfwGetProcAddress)) {
        glfwDestroyWindow(w_);
        w_ = nullptr;
        if (--g_glfwRefs == 0) glfwTerminate();
        throw std::runtime_error("ember: glad failed to load OpenGL 4.3 entry points");
    }
    glfwSetWindowUserPointer(w_, this);
    glfwSwapInterval(vsync ? 1 : 0);
    glfwSetKeyCallback(w_, &Window::keyCb);
    glfwSetCursorPosCallback(w_, &Window::cursorCb);
    glfwSetMouseButtonCallback(w_, &Window::mouseCb);
    glfwSetScrollCallback(w_, &Window::scrollCb);
    glfwSetFramebufferSizeCallback(w_, &Window::resizeCb);
    lastTime_ = glfwGetTime();
}

Window::~Window() {
    if (w_) glfwDestroyWindow(w_);
    if (--g_glfwRefs <= 0) glfwTerminate();
}

void Window::checkCallbacks() {
    if (g_callbackError) {
        auto error = g_callbackError;
        g_callbackError = nullptr;
        std::rethrow_exception(error);
    }
}

void Window::setVsync(bool on) {
    GLFWwindow* previous = glfwGetCurrentContext();
    if (previous != w_) glfwMakeContextCurrent(w_);
    glfwSwapInterval(on ? 1 : 0);
    if (previous != w_) glfwMakeContextCurrent(previous);
    checkCallbacks();
}

void Window::pollEvents() {
    glfwPollEvents();
    checkCallbacks();
    const double t = glfwGetTime();
    lastDt_ = (float)(t - lastTime_);
    lastTime_ = t;
}

glm::ivec2 Window::size() const {
    int w = 0, h = 0;
    glfwGetWindowSize(w_, &w, &h);
    return {w, h};
}

glm::ivec2 Window::framebufferSize() const {
    int w = 0, h = 0;
    glfwGetFramebufferSize(w_, &w, &h);
    return {w, h};
}

glm::vec2 Window::cursor() const {
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(w_, &x, &y);
    return {(float)x, (float)y};
}

glm::vec2 Window::scrollDelta() {
    const glm::vec2 d = scrollAccum_;
    scrollAccum_ = {0.f, 0.f};
    return d;
}

// ---- static callbacks -----------------------------------------------------

void Window::keyCb(GLFWwindow* w, int key, int sc, int act, int mods) {
    if (auto* s = fromHandle(w); s && s->onKey) callback([&] { s->onKey(key, sc, act, mods); });
}
void Window::cursorCb(GLFWwindow* w, double x, double y) {
    if (auto* s = fromHandle(w); s && s->onCursorPos) callback([&] { s->onCursorPos(x, y); });
}
void Window::mouseCb(GLFWwindow* w, int b, int act, int mods) {
    if (auto* s = fromHandle(w); s && s->onMouseButton) callback([&] { s->onMouseButton(b, act, mods); });
}
void Window::scrollCb(GLFWwindow* w, double x, double y) {
    if (auto* s = fromHandle(w)) {
        s->scrollAccum_ += glm::vec2((float)x, (float)y);
        if (s->onScroll) callback([&] { s->onScroll(x, y); });
    }
}
void Window::resizeCb(GLFWwindow* w, int width, int height) {
    if (auto* s = fromHandle(w); s && s->onResize) callback([&] { s->onResize(width, height); });
}

} // namespace ember
