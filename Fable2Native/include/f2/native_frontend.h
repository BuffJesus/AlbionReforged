#pragma once

#include "native_video.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace f2 {

enum class FrontendState {
    Boot,
    IntroVideo,
    AttractVideo,
    Title,
    MainMenu,
    ChooseCard,
    Options,
    Loading,
    World,
};

enum class FrontendAction {
    None,
    Accept,
    Back,
    Left,
    Right,
    Up,
    Down,
    Skip,
};

struct MenuItem {
    std::string id;
    std::string label;
    bool enabled = true;
};

class FrontendController {
public:
    FrontendController();

    void reset();
    void tick(double delta_seconds);
    void dispatch(FrontendAction action);

    [[nodiscard]] FrontendState state() const noexcept { return state_; }
    [[nodiscard]] std::size_t selected_item() const noexcept { return selected_item_; }
    [[nodiscard]] std::size_t previous_selected_item() const noexcept {
        return previous_selected_item_;
    }
    [[nodiscard]] bool options_page_open() const noexcept { return options_page_open_; }
    [[nodiscard]] bool selection_animating() const noexcept {
        return selection_animating_;
    }
    [[nodiscard]] bool girl_selected() const noexcept { return girl_selected_; }
    [[nodiscard]] bool sound_enabled() const noexcept { return sound_enabled_; }
    [[nodiscard]] bool subtitles_enabled() const noexcept { return subtitles_enabled_; }
    [[nodiscard]] bool tutorial_boxes_enabled() const noexcept { return tutorial_boxes_enabled_; }
    [[nodiscard]] bool multiplayer_orbs_enabled() const noexcept {
        return multiplayer_orbs_enabled_;
    }
    [[nodiscard]] bool auto_joinable_enabled() const noexcept { return auto_joinable_enabled_; }
    [[nodiscard]] bool invert_aim_enabled() const noexcept { return invert_aim_enabled_; }
    [[nodiscard]] int breadcrumb_size() const noexcept { return breadcrumb_size_; }
    [[nodiscard]] int gamma_percent() const noexcept { return gamma_percent_; }
    [[nodiscard]] int resolution_index() const noexcept { return resolution_index_; }
    [[nodiscard]] int anti_aliasing_index() const noexcept { return anti_aliasing_index_; }
    // Resolution is a real PC backend setting: the index maps to a concrete pixel size the app
    // applies to the window + swapchain. (Options page §6; the app owns the actual resize.)
    [[nodiscard]] int resolution_width() const noexcept;
    [[nodiscard]] int resolution_height() const noexcept;
    [[nodiscard]] static int resolution_option_count() noexcept;
    // Render backend (0 = D3D12, 1 = Vulkan). Restart-applied: changing it persists the preference;
    // the unified launcher (frontend_main.cpp) reads it on next start. Initialized from the pref.
    [[nodiscard]] int render_backend_index() const noexcept { return render_backend_index_; }
    // Cursor for the focused option row within an open Options tab (Up/Down moves it, Left/Right
    // changes that row's value). Named per-tab-generic; the Video page also keys its row highlight to it.
    [[nodiscard]] int option_row() const noexcept { return option_row_; }
    // Number of adjustable rows in the currently selected Options tab.
    [[nodiscard]] int option_row_count() const noexcept;
    // On-screen FPS counter toggle (Video options row 3). A real native setting the app applies by
    // drawing the counter each frame when enabled.
    [[nodiscard]] bool fps_display_enabled() const noexcept { return fps_display_enabled_; }
    void set_fps_display_enabled(bool enabled) noexcept { fps_display_enabled_ = enabled; }
    [[nodiscard]] int sounds_volume() const noexcept { return sounds_volume_; }
    [[nodiscard]] int music_volume() const noexcept { return music_volume_; }
    [[nodiscard]] int voice_volume() const noexcept { return voice_volume_; }
    [[nodiscard]] int speaker_mode() const noexcept { return speaker_mode_; }
    [[nodiscard]] bool frontend_music_active() const noexcept {
        return state_ == FrontendState::Title || state_ == FrontendState::MainMenu ||
               state_ == FrontendState::ChooseCard || state_ == FrontendState::Options;
    }
    [[nodiscard]] double state_time() const noexcept { return state_time_; }
    [[nodiscard]] double selection_time() const noexcept { return selection_time_; }
    [[nodiscard]] bool quit_requested() const noexcept { return quit_requested_; }
    [[nodiscard]] const std::vector<MenuItem>& menu_items() const noexcept { return menu_items_; }
    [[nodiscard]] const std::vector<MenuItem>& options_items() const noexcept {
        return options_items_;
    }
    [[nodiscard]] const NativeVideoPlayer& intro_videos() const noexcept { return intro_videos_; }
    [[nodiscard]] const NativeVideoPlayer& attract_videos() const noexcept {
        return attract_videos_;
    }

    // Stable IDs are the modding surface. Mods may append or remove entries
    // without depending on a visual index or an implementation address.
    bool add_menu_item(MenuItem item, std::size_t position = static_cast<std::size_t>(-1));
    bool remove_menu_item(std::string_view id);
    bool select_menu_item(std::string_view id);
    bool select_card(bool girl);

private:
    void enter(FrontendState next);
    void move_selection(int direction);
    void persist_options() const;  // write the Options settings to options.ini

    FrontendState state_ = FrontendState::Boot;
    double state_time_ = 0.0;
    double selection_time_ = 0.0;
    std::size_t selected_item_ = 0;
    std::size_t previous_selected_item_ = 0;
    bool selection_animating_ = false;
    bool options_page_open_ = false;
    bool quit_requested_ = false;
    bool girl_selected_ = false;
    bool sound_enabled_ = true;
    bool subtitles_enabled_ = true;
    bool tutorial_boxes_enabled_ = true;
    bool multiplayer_orbs_enabled_ = true;
    bool auto_joinable_enabled_ = true;
    bool invert_aim_enabled_ = false;
    int breadcrumb_size_ = 1;
    int gamma_percent_ = 50;
    int resolution_index_ = 1;
    int anti_aliasing_index_ = 2;
    int render_backend_index_ = 0;
    int option_row_ = 0;
    bool fps_display_enabled_ = false;
    int sounds_volume_ = 80;
    int music_volume_ = 80;
    int voice_volume_ = 80;
    int speaker_mode_ = 0;
    std::vector<MenuItem> menu_items_;
    std::vector<MenuItem> options_items_;
    NativeVideoPlayer intro_videos_;
    NativeVideoPlayer attract_videos_;
};

}  // namespace f2
