#pragma once

// Minimal GLFW window wrapper (optional convenience module).
// Creates a 4.3 core-profile context, initializes glad, tracks input state
// and per-frame delta time. Host applications that bring their own windowing
// (SDL / Qt / Win32 / ...) do NOT need this — see INTEGRATION.md.

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>

#include <functional>

namespace ember {

class Window {
public:
    Window(int width, int height, const char* title, int samples = 4, bool vsync = true, bool visible = true);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool shouldClose() const { return glfwWindowShouldClose(w_) != 0; }
    void pollEvents();
    void swapBuffers() { glfwSwapBuffers(w_); }
    void setTitle(const char* t) { glfwSetWindowTitle(w_, t); }
    void setVsync(bool on) { glfwSwapInterval(on ? 1 : 0); }
    void setCursorPos(glm::vec2 p) { glfwSetCursorPos(w_, p.x, p.y); }

    double time() const { return glfwGetTime(); }
    // Seconds since the last pollEvents(); updated every poll.
    float deltaTime() const { return lastDt_; }

    glm::ivec2 size() const;
    glm::ivec2 framebufferSize() const;

    bool key(int glfwKey) const { return glfwGetKey(w_, glfwKey) == GLFW_PRESS; }
    bool mouseButton(int button) const { return glfwGetMouseButton(w_, button) == GLFW_PRESS; }
    glm::vec2 cursor() const;

    // Accumulated scroll since the last call (returns and resets).
    glm::vec2 scrollDelta();

    GLFWwindow* handle() const { return w_; }

    // Optional callbacks (GLFW-style signatures).
    std::function<void(int key, int scancode, int action, int mods)> onKey;
    std::function<void(double x, double y)> onCursorPos;
    std::function<void(int button, int action, int mods)> onMouseButton;
    std::function<void(double xoff, double yoff)> onScroll;
    std::function<void(int width, int height)> onResize;

private:
    static Window* fromHandle(GLFWwindow* w) {
        return static_cast<Window*>(glfwGetWindowUserPointer(w));
    }
    static void keyCb(GLFWwindow*, int, int, int, int);
    static void cursorCb(GLFWwindow*, double, double);
    static void mouseCb(GLFWwindow*, int, int, int);
    static void scrollCb(GLFWwindow*, double, double);
    static void resizeCb(GLFWwindow*, int, int);

    GLFWwindow* w_ = nullptr;
    float lastDt_ = 0.f;
    double lastTime_ = 0.0;
    glm::vec2 scrollAccum_{0.f};
};

} // namespace ember
