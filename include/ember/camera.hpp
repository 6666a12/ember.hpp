#pragma once

// Minimal orbit camera (header-only). Used by the examples; the library
// itself only needs view/projection matrices, whatever produces them.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace ember {

class OrbitCamera {
public:
    glm::vec3 target{0.f, 0.f, 0.f};
    float yaw = 0.f;       // radians, around Y
    float pitch = 0.6f;    // radians
    float distance = 12.f;
    float fovY = 50.f;     // degrees
    float nearPlane = 0.05f;
    float farPlane = 500.f;

    glm::vec3 position() const {
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        return target + distance * glm::vec3(cp * std::sin(yaw), sp, cp * std::cos(yaw));
    }

    glm::mat4 view() const { return glm::lookAt(position(), target, glm::vec3(0.f, 1.f, 0.f)); }

    glm::mat4 projection(float aspect) const {
        return glm::perspective(glm::radians(fovY), aspect, nearPlane, farPlane);
    }

    // Drag-to-orbit. dx/dy are pixel deltas (positive y drags the camera up).
    void orbit(float dx, float dy) {
        yaw -= dx * 0.005f;
        pitch = glm::clamp(pitch + dy * 0.005f, -1.55f, 1.55f);
    }

    void zoom(float scrollY) {
        if (scrollY == 0.f) return; // no wheel motion (examples call this every frame)
        distance = glm::clamp(distance * std::pow(0.88f, scrollY), 0.5f, 300.f);
    }
};

} // namespace ember
