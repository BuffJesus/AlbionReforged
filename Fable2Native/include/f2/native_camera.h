#pragma once

// Third-person DEFAULT FOLLOW camera (directly behind the hero, centred).
//
// Retail read contract (camera_system.txt): the camera exposes world pos @+0 and
// forward @+32; a follow-vs-cutscene selector (sub_82265F90) picks the active camera;
// GetCameraValues @0x82967988 supplies per-region tuning.
//
// The default follow is CENTRED BEHIND the hero (no lateral offset). This is the
// non-guessing choice: the follow-camera CONTROLLER math (spring/lag, distance,
// height, any offset) lives in camera->[+4]->[+60] and was NOT statically isolated
// (camera_system.txt §7a — the "HIGHEST-VALUE gap"), so no shoulder offset is asserted.
// OVER-THE-SHOULDER is a SEPARATE mode in retail: the AIM/TARGET camera (§6,
// SetInTargetingCamera @0x82967560) that engages on lock-on/aim and shifts framing
// toward the target (FilterFoesForCombatCamera / CameraInterest). That belongs with
// the combat/targeting phase — it is deliberately NOT part of this base follow.
//
// Grounded here: vertical FOV (70deg, 0x82100efc). ENGINEERING (no retail source):
// distance/height/pitch-clamps/sensitivities/smoothing — the CameraValues data is a
// cage/track model (FocalOffset/TrackAmount/SlotWidth), not orbit dist/height.
//
// Outputs position + yaw + pitch matching the renderer's forward convention
// (fwd = {cos(pitch)sin(yaw), sin(pitch), cos(pitch)cos(yaw)}, pos = target - fwd*dist),
// so writing them into NativeGame.camera frames the hero with no renderer change.

#include <array>

namespace f2 {

class NativeCollisionWorld;  // optional: camera terrain clamp

enum class CameraMode {
    Follow,  // orbit the hero
    Free,    // free-fly debug (today's behaviour), --freefly
};

struct FollowCameraConfig {
    float fov_y_radians = 1.22171938f;  // GROUNDED: 70deg default (0x82100efc)
    float distance = 4.5f;              // ENGINEERING (retail = cage/track, not orbit)
    float height = 1.6f;                // ENGINEERING: look-target height above hero feet
    float min_pitch = -0.9f;            // ENGINEERING clamps (radians): look slightly down..up a bit
    float max_pitch = 0.55f;
    float stick_yaw_speed = 2.6f;       // rad/s at full right-stick (ENGINEERING)
    float stick_pitch_speed = 1.8f;
    float mouse_yaw_sensitivity = 0.0025f;   // rad/pixel (ENGINEERING)
    float mouse_pitch_sensitivity = 0.0025f;
    float follow_smoothing = 12.0f;     // position lerp rate (1/s); higher = snappier
    float ground_margin = 0.4f;         // keep the camera this far above terrain
};

class CameraController {
public:
    CameraMode mode = CameraMode::Follow;
    FollowCameraConfig config;

    float yaw = 0.0f;      // orbit yaw (radians)
    float pitch = -0.25f;  // orbit pitch (radians)
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};  // smoothed camera position (renderer reads this)
    std::array<float, 3> target{0.0f, 0.0f, 0.0f};    // look target (hero + height)

    // Advance the orbit from look input and reposition around the hero. `look` = unified
    // look delta (right-stick units, or mouse pixels if look_is_mouse). If `world` is
    // given, the camera is kept above the terrain (no clipping through the ground).
    void update(const std::array<float, 3>& hero_position, const std::array<float, 2>& look,
                bool look_is_mouse, float dt, const NativeCollisionWorld* world = nullptr);

    [[nodiscard]] float fov_y() const noexcept { return config.fov_y_radians; }

private:
    bool initialised_ = false;  // snap (no smoothing) on the first frame
};

}  // namespace f2
