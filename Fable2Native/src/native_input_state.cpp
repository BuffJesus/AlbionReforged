#include "f2/native_input_state.h"

#include <windows.h>
#include <xinput.h>

#include <cmath>

namespace f2 {

std::array<float, 2> InputSampler::apply_stick_curve(std::array<float, 2> raw,
                                                     const InputAnalogConfig& cfg) noexcept {
    // Radial deadzone: magnitude below `deadzone` reads as zero; at/above
    // `extent_threshold` saturates to 1.0; linear remap between (modeling choice).
    // Direction is preserved. Thresholds are decomp-grounded (see header).
    const float mag = std::sqrt(raw[0] * raw[0] + raw[1] * raw[1]);
    if (mag <= cfg.deadzone || mag <= 0.0f) {
        return {0.0f, 0.0f};
    }
    const float span = cfg.extent_threshold - cfg.deadzone;
    float t = span > 0.0f ? (mag - cfg.deadzone) / span : 1.0f;
    if (t > 1.0f) t = 1.0f;
    const float inv = t / mag;  // scale factor along the raw direction
    return {raw[0] * inv, raw[1] * inv};
}

void InputSampler::tick(InputState& out, int pad_index) {
    // ---- Controller (XINPUT) ----
    XINPUT_STATE state{};
    const bool connected = XInputGetState(static_cast<DWORD>(pad_index), &state) == ERROR_SUCCESS;
    out.controller_connected = connected;

    std::array<float, 2> raw_left{0.0f, 0.0f};
    std::array<float, 2> raw_right{0.0f, 0.0f};
    std::uint16_t pad_buttons = 0;
    float lt = 0.0f, rt = 0.0f;
    if (connected) {
        const XINPUT_GAMEPAD& gp = state.Gamepad;
        // Normalize int16 axes to [-1,1] (retail normalize scale ~1/32767, 0x82000C20).
        raw_left = {gp.sThumbLX / 32767.0f, gp.sThumbLY / 32767.0f};
        raw_right = {gp.sThumbRX / 32767.0f, gp.sThumbRY / 32767.0f};
        pad_buttons = gp.wButtons;  // XINPUT bits map 1:1 to PadButton
        lt = gp.bLeftTrigger / 255.0f;
        rt = gp.bRightTrigger / 255.0f;
    }
    out.left_stick = apply_stick_curve(raw_left, config);
    out.right_stick = apply_stick_curve(raw_right, config);
    out.left_trigger = lt;
    out.right_trigger = rt;

    // Button edge-detect against previous-frame held mask (retail +0x3C XOR).
    out.buttons = pad_buttons;
    const std::uint16_t changed = pad_buttons ^ previous_buttons_;
    out.buttons_pressed = changed & pad_buttons;
    out.buttons_released = changed & previous_buttons_;
    previous_buttons_ = pad_buttons;

    // ---- Keyboard/mouse (host-side PC layer) ----
    auto key_down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    std::array<float, 2> kb{0.0f, 0.0f};
    if (key_down('D') || key_down(VK_RIGHT)) kb[0] += 1.0f;
    if (key_down('A') || key_down(VK_LEFT)) kb[0] -= 1.0f;
    if (key_down('W') || key_down(VK_UP)) kb[1] += 1.0f;
    if (key_down('S') || key_down(VK_DOWN)) kb[1] -= 1.0f;
    // Normalize diagonal so KB&M isn't faster on the diagonal.
    const float kb_mag = std::sqrt(kb[0] * kb[0] + kb[1] * kb[1]);
    if (kb_mag > 1.0f) { kb[0] /= kb_mag; kb[1] /= kb_mag; }
    out.keyboard_move = kb;

    // Relative mouse motion for camera look.
    std::array<float, 2> mouse{0.0f, 0.0f};
    POINT cursor{};
    if (GetCursorPos(&cursor)) {
        if (have_prev_cursor_) {
            mouse[0] = static_cast<float>(cursor.x - prev_cursor_x_);
            mouse[1] = static_cast<float>(cursor.y - prev_cursor_y_);
        }
        prev_cursor_x_ = cursor.x;
        prev_cursor_y_ = cursor.y;
        have_prev_cursor_ = true;
    }
    out.mouse_look = mouse;

    const bool keyboard_active = kb_mag > 0.0f;
    const bool mouse_active = std::abs(mouse[0]) > 0.5f || std::abs(mouse[1]) > 0.5f;
    // Controller counts as "active" only past the analog deadzone, or on a button.
    const float pad_mag = std::sqrt(raw_left[0] * raw_left[0] + raw_left[1] * raw_left[1]);
    const float pad_mag_r = std::sqrt(raw_right[0] * raw_right[0] + raw_right[1] * raw_right[1]);
    const bool controller_active = pad_buttons != 0 || lt > 0.1f || rt > 0.1f ||
                                   pad_mag > config.deadzone || pad_mag_r > config.deadzone;

    // Flip last_active_device only when the OTHER device is actively used
    // (matches NativeInputRouter's transition idiom; avoids per-frame flicker).
    if (controller_active) {
        out.last_active_device = InputDevice::Controller;
    } else if (keyboard_active || mouse_active) {
        out.last_active_device = InputDevice::KeyboardMouse;
    }

    // Resolve the unified move/look the gameplay layer reads.
    if (out.last_active_device == InputDevice::KeyboardMouse) {
        out.move = out.keyboard_move;
        out.look = out.mouse_look;
    } else {
        out.move = out.left_stick;
        out.look = out.right_stick;
    }
}

}  // namespace f2
