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
    Title,
    MainMenu,
    Options,
    Loading,
    World,
};

enum class FrontendAction {
    None,
    Accept,
    Back,
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
    [[nodiscard]] double state_time() const noexcept { return state_time_; }
    [[nodiscard]] bool quit_requested() const noexcept { return quit_requested_; }
    [[nodiscard]] const std::vector<MenuItem>& menu_items() const noexcept { return menu_items_; }
    [[nodiscard]] const NativeVideoPlayer& intro_videos() const noexcept { return intro_videos_; }

    // Stable IDs are the modding surface. Mods may append or replace entries
    // without depending on a visual index or an implementation address.
    bool add_menu_item(MenuItem item, std::size_t position = static_cast<std::size_t>(-1));
    bool select_menu_item(std::string_view id);

private:
    void enter(FrontendState next);
    void move_selection(int direction);

    FrontendState state_ = FrontendState::Boot;
    double state_time_ = 0.0;
    std::size_t selected_item_ = 0;
    bool quit_requested_ = false;
    std::vector<MenuItem> menu_items_;
    NativeVideoPlayer intro_videos_;
};

}  // namespace f2
