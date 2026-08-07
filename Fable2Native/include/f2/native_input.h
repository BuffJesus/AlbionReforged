#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace f2 {

enum class NativeInputAction : std::uint8_t {
    Accept,
    Back,
    Left,
    Right,
    Up,
    Down,
    Skip,
    Count,
};

class NativeInputRouter {
public:
    NativeInputRouter();

    void load_bindings(const std::filesystem::path& path);
    void force_controller_prompts(bool active) noexcept {
        controller_prompts_forced_ = active;
        controller_prompt_active_ = active;
    }
    void poll();

    [[nodiscard]] bool pressed(NativeInputAction action) const noexcept;
    [[nodiscard]] const std::string& prompt(NativeInputAction action) const noexcept;
    [[nodiscard]] bool controller_connected() const noexcept { return controller_connected_; }
    [[nodiscard]] bool using_controller_prompts() const noexcept {
        return controller_prompt_active_;
    }

private:
    struct Binding {
        std::uint8_t virtual_key = 0;
        std::uint16_t controller_button = 0;
        std::string keyboard_prompt;
        std::string controller_prompt;
    };

    void set_default_bindings();
    void load_binding_line(const std::string& key, const std::string& value);
    void mark_pressed(NativeInputAction action);
    void update_prompt_device(bool controller, bool mouse = false);

    std::array<Binding, static_cast<std::size_t>(NativeInputAction::Count)> bindings_{};
    std::array<bool, static_cast<std::size_t>(NativeInputAction::Count)> pressed_{};
    std::array<bool, static_cast<std::size_t>(NativeInputAction::Count)> previous_keyboard_{};
    std::uint16_t previous_controller_buttons_ = 0;
    bool controller_connected_ = false;
    bool controller_prompt_active_ = false;
    bool controller_prompts_forced_ = false;
    bool mouse_prompt_active_ = false;
};

}  // namespace f2
