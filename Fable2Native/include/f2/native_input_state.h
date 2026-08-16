#pragma once

// Gameplay analog input for Fable2Native.
//
// This sits ALONGSIDE the frontend's discrete NativeInputRouter (native_input.h),
// which stays responsible for menu navigation. InputState mirrors the retail pad
// object's analog fields and the event edge-detect, and adds the PC-port layer:
// mouse+keyboard as a first-class scheme feeding the SAME action space, plus
// last-active-device tracking that drives dynamic button prompts.
//
// Decomp grounding (see ghidra_out/p2_feel_constants.txt + input_spec.txt):
//   - pad object: LX/LY @+0x18/+0x1C, RX/RY @+0x20/+0x24 (normalized float),
//     held button mask @+0x3C (prev-frame XOR for press/release).
//   - stick config .data defaults: deadzone 0.4 (0x8331959C), dampening 0.6
//     (0x833195A0), extent 0.8 (0x833195A4); stick-move gate 0.0001 (0x82099A80).
//   - KB&M has NO guest path (retail is controller-only); retail injects KB/M
//     host-side into the same MESSAGE_EVENT queue. So in the native port KB&M +
//     last_active_device is entirely OUR layer over the retail action set.

#include <array>
#include <cstdint>

namespace f2 {

// Which device last produced input, for dynamic on-screen prompts.
enum class InputDevice : std::uint8_t {
    KeyboardMouse,
    Controller,
};

// Gameplay button set (retail XINPUT-360 layout; bit order per input_spec.txt).
// Values are the XINPUT button bits so a raw XINPUT_GAMEPAD.wButtons maps 1:1.
enum class PadButton : std::uint16_t {
    DPadUp       = 0x0001,
    DPadDown     = 0x0002,
    DPadLeft     = 0x0004,
    DPadRight    = 0x0008,
    Start        = 0x0010,
    Back         = 0x0020,
    LeftThumb    = 0x0040,
    RightThumb   = 0x0080,
    LeftShoulder = 0x0100,
    RightShoulder= 0x0200,
    A            = 0x1000,
    B            = 0x2000,
    X            = 0x4000,
    Y            = 0x8000,
};

// Analog-stick response thresholds. All three are decomp-grounded shipped
// defaults (the runtime SetJoystick* natives overwrite these same .data slots).
// NOTE: the three THRESHOLDS are grounded; how they combine into a response
// CURVE is NOT in the decomp (the normalize fn func_0x82189a00 was not
// decompiled), so the curve shape below is a documented modeling choice.
struct InputAnalogConfig {
    float deadzone         = 0.4f;   // 0x8331959C (3ECCCCCD) inner cutoff
    float dampening_zone   = 0.6f;   // 0x833195A0 (3F19999A) soft mid-zone (retail attenuates here; exact curve unknown)
    float extent_threshold = 0.8f;   // 0x833195A4 (3F4CCCCD) saturation point
    float move_event_gate  = 0.0001f;// 0x82099A80 (38D1B717) stick-move event emit gate
};

// Snapshot of gameplay input for one frame. Mirrors the retail pad analog block
// + adds host-side KB&M axes and the active-device flag.
struct InputState {
    // Controller-space analog, deadzone/extent-remapped to [-1,1] (retail +0x18..+0x24).
    std::array<float, 2> left_stick{0.0f, 0.0f};
    std::array<float, 2> right_stick{0.0f, 0.0f};
    float left_trigger = 0.0f;
    float right_trigger = 0.0f;

    // Button state + edge-detect (retail held-mask XOR).
    std::uint16_t buttons = 0;          // currently-down XINPUT bits
    std::uint16_t buttons_pressed = 0;  // went down this frame
    std::uint16_t buttons_released = 0; // went up this frame

    // Host-side KB&M layer (PC port). WASD is injected into the SAME move space
    // as left_stick; mouse delta drives camera look like the right stick.
    std::array<float, 2> keyboard_move{0.0f, 0.0f};  // WASD, [-1,1] per axis
    std::array<float, 2> mouse_look{0.0f, 0.0f};     // relative pixels this frame

    // Unified movement/look the gameplay code should read (controller OR KB&M,
    // whichever is active — resolved in InputSampler::tick).
    std::array<float, 2> move{0.0f, 0.0f};  // desired planar move, [-1,1]
    std::array<float, 2> look{0.0f, 0.0f};  // desired look delta (stick units or scaled mouse)

    InputDevice last_active_device = InputDevice::Controller;
    bool controller_connected = false;

    [[nodiscard]] bool down(PadButton b) const noexcept {
        return (buttons & static_cast<std::uint16_t>(b)) != 0;
    }
    [[nodiscard]] bool pressed(PadButton b) const noexcept {
        return (buttons_pressed & static_cast<std::uint16_t>(b)) != 0;
    }
    [[nodiscard]] bool released(PadButton b) const noexcept {
        return (buttons_released & static_cast<std::uint16_t>(b)) != 0;
    }
};

// Samples XInput + keyboard/mouse once per frame and produces an InputState.
// Self-contained (reads GetAsyncKeyState / XInputGetState / GetCursorPos), matching
// the idiom already used by NativeInputRouter in native_input.cpp.
class InputSampler {
public:
    InputAnalogConfig config{};

    // Sample all devices for player index `pad_index` (default 0). Updates `out`,
    // including edge-detect against the previous frame and last_active_device.
    void tick(InputState& out, int pad_index = 0);

    // Apply the retail radial deadzone/extent remap to a raw normalized stick
    // vector. Static so it can be unit-tested without device state.
    static std::array<float, 2> apply_stick_curve(std::array<float, 2> raw,
                                                  const InputAnalogConfig& cfg) noexcept;

private:
    std::uint16_t previous_buttons_ = 0;  // held mask, prev frame (retail +0x3C)
    bool have_prev_cursor_ = false;
    long prev_cursor_x_ = 0;
    long prev_cursor_y_ = 0;
};

}  // namespace f2
