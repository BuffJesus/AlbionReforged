#pragma once

// Third-person follow-camera controller.
//
// Retail read contract (camera_system.txt): the camera exposes world pos @+0 and
// forward @+32; a follow-vs-cutscene selector (sub_82265F90) picks the active camera;
// GetCameraValues @0x82967988 supplies per-region tuning. The follow MATH (spring/lag,
// stick->yaw/pitch curve, pitch clamps) was NOT statically isolated (resolver §7a-d),
// AND the CameraValues data is a CAGE/TRACK model (FocalOffset/TrackAmount/SlotWidth),
// not orbit distance/height. So this ORBIT follow cam is an ENGINEERING MODEL; only the
// vertical FOV (70deg, 0x82100efc) is decomp-grounded.
//
// It outputs position + yaw + pitch matching the world renderer's forward convention
// (fwd = {cos(pitch)sin(yaw), sin(pitch), cos(pitch)cos(yaw)}, pos = target - fwd*dist),
// so writing them into NativeGame.camera frames the hero with no renderer change.

#include <array>

namespace f2 {

enum class CameraMode {
    Follow,  // orbit the hero
    Free,    // free-fly debug (today's behaviour), --freefly
};

struct FollowCameraConfig {
    float fov_y_radians = 1.22171938f;  // GROUNDED: 70deg default (0x82100efc)
    float distance = 4.0f;              // ENGINEERING (retail = cage/track, not orbit)
    float height = 1.6f;                // ENGINEERING: look-target height above hero feet
    float min_pitch = -1.2f;            // ENGINEERING clamps (radians)
    float max_pitch = 0.45f;
    float stick_yaw_speed = 2.5f;       // rad/s at full right-stick (ENGINEERING)
    float stick_pitch_speed = 1.5f;
    float mouse_yaw_sensitivity = 0.004f;   // rad/pixel (ENGINEERING)
    float mouse_pitch_sensitivity = 0.004f;
};

class CameraController {
public:
    CameraMode mode = CameraMode::Follow;
    FollowCameraConfig config;

    float yaw = 0.0f;      // orbit yaw (radians)
    float pitch = -0.3f;   // orbit pitch (radians)
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};  // computed camera position
    std::array<float, 3> target{0.0f, 0.0f, 0.0f};    // look target (hero + height)

    // Advance the orbit from look input and reposition around the hero.
    // `look` = unified look delta (right-stick units, or mouse pixels if look_is_mouse).
    void update(const std::array<float, 3>& hero_position,
                const std::array<float, 2>& look, bool look_is_mouse, float dt);

    [[nodiscard]] float fov_y() const noexcept { return config.fov_y_radians; }
};

}  // namespace f2
