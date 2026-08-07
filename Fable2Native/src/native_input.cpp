#include "f2/native_input.h"

#ifdef _WIN32
#include <windows.h>
#include <Xinput.h>
#endif

#include <algorithm>
#include <cctype>
#include <fstream>
#include <ranges>
#include <string_view>
#include <utility>

namespace f2 {
namespace {

std::string trim(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

}  // namespace

NativeInputRouter::NativeInputRouter() {
    set_default_bindings();
}

void NativeInputRouter::set_default_bindings() {
#ifdef _WIN32
    bindings_[static_cast<std::size_t>(NativeInputAction::Accept)] =
        {VK_RETURN, XINPUT_GAMEPAD_A, "ENTER", "A"};
    bindings_[static_cast<std::size_t>(NativeInputAction::Back)] =
        {VK_ESCAPE, XINPUT_GAMEPAD_B, "ESC", "B"};
    bindings_[static_cast<std::size_t>(NativeInputAction::Left)] =
        {VK_LEFT, 0, "LEFT", "LEFT"};
    bindings_[static_cast<std::size_t>(NativeInputAction::Right)] =
        {VK_RIGHT, 0, "RIGHT", "RIGHT"};
    bindings_[static_cast<std::size_t>(NativeInputAction::Up)] =
        {VK_UP, XINPUT_GAMEPAD_DPAD_UP, "UP", "DPAD UP"};
    bindings_[static_cast<std::size_t>(NativeInputAction::Down)] =
        {VK_DOWN, XINPUT_GAMEPAD_DPAD_DOWN, "DOWN", "DPAD DOWN"};
    bindings_[static_cast<std::size_t>(NativeInputAction::Skip)] =
        {VK_SPACE, XINPUT_GAMEPAD_START, "SPACE", "START"};
#else
    bindings_[static_cast<std::size_t>(NativeInputAction::Accept)] = {0, 0, "ENTER", "A"};
    bindings_[static_cast<std::size_t>(NativeInputAction::Back)] = {0, 0, "ESC", "B"};
    bindings_[static_cast<std::size_t>(NativeInputAction::Left)] = {0, 0, "LEFT", "LEFT"};
    bindings_[static_cast<std::size_t>(NativeInputAction::Right)] = {0, 0, "RIGHT", "RIGHT"};
    bindings_[static_cast<std::size_t>(NativeInputAction::Up)] = {0, 0, "UP", "DPAD UP"};
    bindings_[static_cast<std::size_t>(NativeInputAction::Down)] = {0, 0, "DOWN", "DPAD DOWN"};
    bindings_[static_cast<std::size_t>(NativeInputAction::Skip)] = {0, 0, "SPACE", "START"};
#endif
}

void NativeInputRouter::load_bindings(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) return;
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        load_binding_line(trim(line.substr(0, separator)), trim(line.substr(separator + 1)));
    }
}

void NativeInputRouter::load_binding_line(const std::string& key, const std::string& value) {
    const auto normalized_key = lower(key);
    const auto normalized_value = lower(value);
    NativeInputAction action = NativeInputAction::Count;
    if (normalized_key == "accept") action = NativeInputAction::Accept;
    else if (normalized_key == "back") action = NativeInputAction::Back;
    else if (normalized_key == "left") action = NativeInputAction::Left;
    else if (normalized_key == "right") action = NativeInputAction::Right;
    else if (normalized_key == "up") action = NativeInputAction::Up;
    else if (normalized_key == "down") action = NativeInputAction::Down;
    else if (normalized_key == "skip") action = NativeInputAction::Skip;
    if (action == NativeInputAction::Count) return;

    static constexpr std::pair<std::string_view, std::uint8_t> keys[] = {
        {"enter", static_cast<std::uint8_t>(VK_RETURN)},
        {"return", static_cast<std::uint8_t>(VK_RETURN)},
        {"escape", static_cast<std::uint8_t>(VK_ESCAPE)},
        {"esc", static_cast<std::uint8_t>(VK_ESCAPE)},
        {"space", static_cast<std::uint8_t>(VK_SPACE)},
        {"up", static_cast<std::uint8_t>(VK_UP)},
        {"down", static_cast<std::uint8_t>(VK_DOWN)},
        {"left", static_cast<std::uint8_t>(VK_LEFT)},
        {"right", static_cast<std::uint8_t>(VK_RIGHT)},
        {"tab", static_cast<std::uint8_t>(VK_TAB)},
    };
    const auto it = std::ranges::find_if(keys, [&](const auto& entry) {
        return entry.first == normalized_value;
    });
    if (it == std::ranges::end(keys)) return;
    auto& binding = bindings_[static_cast<std::size_t>(action)];
    binding.virtual_key = it->second;
    binding.keyboard_prompt = lower(value);
    std::ranges::transform(binding.keyboard_prompt, binding.keyboard_prompt.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
}

void NativeInputRouter::mark_pressed(NativeInputAction action) {
    pressed_[static_cast<std::size_t>(action)] = true;
}

void NativeInputRouter::update_prompt_device(bool controller, bool mouse) {
    if (controller_prompts_forced_) return;
    controller_prompt_active_ = controller;
    mouse_prompt_active_ = mouse;
}

void NativeInputRouter::poll() {
    pressed_.fill(false);
#ifdef _WIN32
    for (std::size_t index = 0; index < bindings_.size(); ++index) {
        const auto& binding = bindings_[index];
        const bool down = (GetAsyncKeyState(binding.virtual_key) & 0x8000) != 0;
        if (down && !previous_keyboard_[index]) {
            mark_pressed(static_cast<NativeInputAction>(index));
            update_prompt_device(false);
        }
        previous_keyboard_[index] = down;
    }

    const bool left_mouse = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool right_mouse = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    static bool previous_left_mouse = false;
    static bool previous_right_mouse = false;
    if (left_mouse && !previous_left_mouse) update_prompt_device(false, true);
    if (right_mouse && !previous_right_mouse) {
        mark_pressed(NativeInputAction::Back);
        update_prompt_device(false, true);
    }
    previous_left_mouse = left_mouse;
    previous_right_mouse = right_mouse;

    XINPUT_STATE state{};
    const bool connected = XInputGetState(0, &state) == ERROR_SUCCESS;
    controller_connected_ = connected;
    if (connected) {
        const auto buttons = state.Gamepad.wButtons;
        const auto pressed_buttons = static_cast<std::uint16_t>(buttons & ~previous_controller_buttons_);
        for (std::size_t index = 0; index < bindings_.size(); ++index) {
            if ((pressed_buttons & bindings_[index].controller_button) != 0) {
                mark_pressed(static_cast<NativeInputAction>(index));
                update_prompt_device(true);
            }
        }
        previous_controller_buttons_ = buttons;
    } else {
        previous_controller_buttons_ = 0;
    }
#else
    previous_keyboard_.fill(false);
#endif
}

bool NativeInputRouter::pressed(NativeInputAction action) const noexcept {
    return pressed_[static_cast<std::size_t>(action)];
}

const std::string& NativeInputRouter::prompt(NativeInputAction action) const noexcept {
    const auto& binding = bindings_[static_cast<std::size_t>(action)];
    if (controller_prompt_active_) return binding.controller_prompt;
    if (mouse_prompt_active_ && action == NativeInputAction::Accept) {
        static const std::string left_click = "LEFT CLICK";
        return left_click;
    }
    return binding.keyboard_prompt;
}

}  // namespace f2
