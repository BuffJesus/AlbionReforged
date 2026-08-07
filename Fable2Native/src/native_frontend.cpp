#include "f2/native_frontend.h"

#include "f2/native_frontend_config.h"

#include <algorithm>
#include <array>

namespace f2 {
namespace {

// Native PC resolution options (index → pixel size). Kept in sync with the Left/Right clamp
// (0..2) in FrontendController::dispatch. The D3D12/Vulkan apps apply these to the real
// window + swapchain; the settings survive leaving/reopening Options because they live here.
struct ResolutionMode {
    int width;
    int height;
};
constexpr std::array<ResolutionMode, 3> kResolutionModes{{
    {1280, 720},
    {1600, 900},
    {1920, 1080},
}};

}  // namespace

int FrontendController::resolution_option_count() noexcept {
    return static_cast<int>(kResolutionModes.size());
}

int FrontendController::resolution_width() const noexcept {
    return kResolutionModes[std::clamp(resolution_index_, 0,
                                       static_cast<int>(kResolutionModes.size()) - 1)].width;
}

int FrontendController::resolution_height() const noexcept {
    return kResolutionModes[std::clamp(resolution_index_, 0,
                                       static_cast<int>(kResolutionModes.size()) - 1)].height;
}

FrontendController::FrontendController() {
    menu_items_ = {
        {"new_game", "New Game"},
        {"continue", "Continue"},
        {"language", "Language"},
        {"subtitles", "Subtitles"},
        {"options", "Options"},
    };
    options_items_ = {
        {"game", "Game"},
        {"video", "Video"},
        {"controls", "Controls"},
        {"audio", "Audio"},
    };
    // Boot sequence is just the two publisher/developer logos, then the title screen. The long
    // "intro" cinematic + attract_mode play as the title-idle attract movie (attract_videos_).
    intro_videos_.set_sequence({
        {"microsoft_logo", "videos/microsoft_logo.mp4", 8.808809, true},
        {"lionhead_logo", "videos/lionhead_logo.mp4", 8.320000, true},
    });
    attract_videos_.set_sequence({
        {"attract_mode", "videos/attract_mode.mp4", 29.966637, true},
    });
}

void FrontendController::reset() {
    state_ = FrontendState::Boot;
    state_time_ = 0.0;
    selection_time_ = 0.0;
    selected_item_ = 0;
    previous_selected_item_ = 0;
    selection_animating_ = false;
    options_page_open_ = false;
    quit_requested_ = false;
    girl_selected_ = false;
    sound_enabled_ = true;
    subtitles_enabled_ = true;
    tutorial_boxes_enabled_ = true;
    multiplayer_orbs_enabled_ = true;
    auto_joinable_enabled_ = true;
    invert_aim_enabled_ = false;
    breadcrumb_size_ = 1;
    gamma_percent_ = 50;
    resolution_index_ = 1;
    anti_aliasing_index_ = 2;
    render_backend_index_ = (read_render_backend_preference() == RenderBackend::Vulkan) ? 1 : 0;
    video_setting_row_ = 0;
    sounds_volume_ = 80;
    music_volume_ = 80;
    voice_volume_ = 80;
    speaker_mode_ = 0;
    intro_videos_.reset();
    attract_videos_.reset();
}

void FrontendController::tick(double delta_seconds) {
    state_time_ += std::clamp(delta_seconds, 0.0, 0.25);
    selection_time_ += std::clamp(delta_seconds, 0.0, 0.25);
    if (selection_animating_ && selection_time_ >= 0.15) {
        selection_animating_ = false;
    }
    switch (state_) {
        case FrontendState::Boot:
            if (state_time_ >= 0.5) enter(FrontendState::IntroVideo);
            break;
        case FrontendState::IntroVideo:
            intro_videos_.tick(delta_seconds);
            if (intro_videos_.finished()) enter(FrontendState::Title);
            break;
        case FrontendState::Title:
            // Retail's title screen enters the attract movie after the idle
            // timeout.  Keep the timeout on the title state so returning from
            // the movie replays the complete title reveal/prompt cycle.
            if (state_time_ >= 30.0) {
                attract_videos_.reset();
                enter(FrontendState::AttractVideo);
            }
            break;
        case FrontendState::AttractVideo:
            attract_videos_.tick(delta_seconds);
            if (attract_videos_.finished()) enter(FrontendState::Title);
            break;
        case FrontendState::Loading:
            if (state_time_ >= 1.0) enter(FrontendState::World);
            break;
        default:
            break;
    }
}

void FrontendController::dispatch(FrontendAction action) {
    switch (action) {
        case FrontendAction::Skip:
            if (state_ == FrontendState::Boot || state_ == FrontendState::IntroVideo) {
                intro_videos_.skip_all();
                enter(FrontendState::Title);
            } else if (state_ == FrontendState::AttractVideo) {
                attract_videos_.skip_all();
                enter(FrontendState::Title);
            }
            break;
        case FrontendAction::Up:
        case FrontendAction::Down:
            if (state_ == FrontendState::AttractVideo) {
                enter(FrontendState::Title);
            } else if (state_ == FrontendState::MainMenu || state_ == FrontendState::Options) {
                move_selection(action == FrontendAction::Up ? -1 : 1);
            }
            break;
        case FrontendAction::Left:
        case FrontendAction::Right:
            if (state_ == FrontendState::AttractVideo) enter(FrontendState::Title);
            else if (state_ == FrontendState::ChooseCard) select_card(!girl_selected_);
            else if (state_ == FrontendState::Options && !options_items_.empty()) {
                const int delta = action == FrontendAction::Left ? -10 : 10;
                const auto& id = options_items_[selected_item_].id;
                if (id == "game") {
                    breadcrumb_size_ = std::clamp(breadcrumb_size_ + (delta > 0 ? 1 : -1), 0, 2);
                } else if (id == "video") {
                    if (video_setting_row_ == 0) {
                        gamma_percent_ = std::clamp(gamma_percent_ + delta, 0, 100);
                    } else if (video_setting_row_ == 1) {
                        resolution_index_ = std::clamp(resolution_index_ + (delta > 0 ? 1 : -1), 0, 2);
                    } else if (video_setting_row_ == 2) {
                        anti_aliasing_index_ = std::clamp(anti_aliasing_index_ + (delta > 0 ? 1 : -1), 0, 3);
                    } else {
                        // Renderer backend (restart-applied): persist so the launcher picks it up.
                        render_backend_index_ = std::clamp(render_backend_index_ + (delta > 0 ? 1 : -1), 0, 1);
                        save_render_backend_preference(render_backend_index_ == 1 ? RenderBackend::Vulkan
                                                                                  : RenderBackend::D3D12);
                    }
                } else if (id == "audio") {
                    sounds_volume_ = std::clamp(sounds_volume_ + delta, 0, 100);
                    music_volume_ = std::clamp(music_volume_ + delta, 0, 100);
                    voice_volume_ = std::clamp(voice_volume_ + delta, 0, 100);
                }
            }
            break;
        case FrontendAction::Back:
            if (state_ == FrontendState::AttractVideo) enter(FrontendState::Title);
            else if (state_ == FrontendState::ChooseCard) enter(FrontendState::MainMenu);
            else if (state_ == FrontendState::MainMenu) enter(FrontendState::Title);
            else if (state_ == FrontendState::Options) {
                if (options_page_open_) options_page_open_ = false;
                else enter(FrontendState::MainMenu);
            }
            break;
        case FrontendAction::Accept:
            if (state_ == FrontendState::AttractVideo) {
                enter(FrontendState::Title);
            } else if (state_ == FrontendState::Title) {
                enter(FrontendState::MainMenu);
            } else if (state_ == FrontendState::MainMenu && !menu_items_.empty()) {
                const auto& id = menu_items_[selected_item_].id;
                if (id == "new_game") enter(FrontendState::ChooseCard);
                else if (id == "continue") enter(FrontendState::Loading);
                else if (id == "language" || id == "subtitles" || id == "options") {
                    enter(FrontendState::Options);
                }
                else if (id == "exit") quit_requested_ = true;
            }
            else if (state_ == FrontendState::ChooseCard) {
                enter(FrontendState::Loading);
            }
            else if (state_ == FrontendState::Options && !options_items_.empty()) {
                const auto& id = options_items_[selected_item_].id;
                if (!options_page_open_) {
                    options_page_open_ = true;
                } else if (id == "game") subtitles_enabled_ = !subtitles_enabled_;
                else if (id == "controls") invert_aim_enabled_ = !invert_aim_enabled_;
                else if (id == "video") video_setting_row_ = (video_setting_row_ + 1) % 4;
                else if (id == "audio") speaker_mode_ = speaker_mode_ == 0 ? 1 : 0;
            }
            break;
        case FrontendAction::None:
            break;
    }
}

bool FrontendController::add_menu_item(MenuItem item, std::size_t position) {
    if (item.id.empty() || item.label.empty() ||
        std::ranges::any_of(menu_items_, [&](const MenuItem& existing) {
            return existing.id == item.id;
        })) {
        return false;
    }
    if (position == static_cast<std::size_t>(-1) || position >= menu_items_.size()) {
        menu_items_.push_back(std::move(item));
    } else {
        menu_items_.insert(menu_items_.begin() + static_cast<std::ptrdiff_t>(position),
                           std::move(item));
    }
    return true;
}

bool FrontendController::remove_menu_item(std::string_view id) {
    const auto it = std::ranges::find(menu_items_, id, &MenuItem::id);
    if (it == menu_items_.end()) return false;
    const auto removed_index = static_cast<std::size_t>(std::distance(menu_items_.begin(), it));
    menu_items_.erase(it);
    if (menu_items_.empty()) {
        selected_item_ = 0;
    } else if (selected_item_ > removed_index) {
        --selected_item_;
    } else if (selected_item_ >= menu_items_.size()) {
        selected_item_ = menu_items_.size() - 1;
    }
    return true;
}

bool FrontendController::select_menu_item(std::string_view id) {
    const auto& items = state_ == FrontendState::Options ? options_items_ : menu_items_;
    const auto it = std::ranges::find(items, id, &MenuItem::id);
    if (it == items.end() || !it->enabled) return false;
    const auto next_item = static_cast<std::size_t>(std::distance(items.begin(), it));
    if (next_item != selected_item_) {
        previous_selected_item_ = selected_item_;
        selected_item_ = next_item;
        selection_animating_ = true;
    }
    selection_time_ = 0.0;
    options_page_open_ = false;
    return true;
}

bool FrontendController::select_card(bool girl) {
    if (state_ != FrontendState::ChooseCard) return false;
    girl_selected_ = girl;
    return true;
}

void FrontendController::enter(FrontendState next) {
    state_ = next;
    state_time_ = 0.0;
    selected_item_ = 0;
    previous_selected_item_ = 0;
    selection_animating_ = false;
    selection_time_ = 0.0;
    if (next == FrontendState::ChooseCard) girl_selected_ = false;
}

void FrontendController::move_selection(int direction) {
    const auto& items = state_ == FrontendState::Options ? options_items_ : menu_items_;
    if (items.empty()) return;
    const auto count = static_cast<int>(items.size());
    int next = static_cast<int>(selected_item_) + direction;
    if (state_ == FrontendState::Options) {
        // Retail's OptionsMenu root is bounded: Up on Game and Down on Audio
        // do not wrap around the expandable menu.
        next = std::clamp(next, 0, count - 1);
    } else {
        next = (next % count + count) % count;
    }
    previous_selected_item_ = selected_item_;
    selected_item_ = static_cast<std::size_t>(next);
    selection_animating_ = previous_selected_item_ != selected_item_;
    selection_time_ = 0.0;
    for (int i = 0; i < count && !items[selected_item_].enabled; ++i) {
        selected_item_ = static_cast<std::size_t>((next + direction * (i + 1) + count * 2) % count);
    }
}

}  // namespace f2
