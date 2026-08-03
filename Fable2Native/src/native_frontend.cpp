#include "f2/native_frontend.h"

#include <algorithm>

namespace f2 {

FrontendController::FrontendController() {
    menu_items_ = {
        {"new_game", "New Game"},
        {"load_game", "Load Game"},
        {"options", "Options"},
        {"mod_manager", "Mods"},
        {"exit", "Exit to Desktop"},
    };
    intro_videos_.set_sequence({
        {"microsoft_logo", "videos/microsoft_logo.mp4", 8.808809, true},
        {"lionhead_logo", "videos/lionhead_logo.mp4", 8.320000, true},
        {"middlewarelogos", "videos/middlewarelogos.mp4", 6.506507, true},
        {"intro", "videos/intro.mp4", 76.366590, true},
    });
}

void FrontendController::reset() {
    state_ = FrontendState::Boot;
    state_time_ = 0.0;
    selected_item_ = 0;
    quit_requested_ = false;
    intro_videos_.reset();
}

void FrontendController::tick(double delta_seconds) {
    state_time_ += std::clamp(delta_seconds, 0.0, 0.25);
    switch (state_) {
        case FrontendState::Boot:
            if (state_time_ >= 0.5) enter(FrontendState::IntroVideo);
            break;
        case FrontendState::IntroVideo:
            intro_videos_.tick(delta_seconds);
            if (intro_videos_.finished()) enter(FrontendState::Title);
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
            }
            break;
        case FrontendAction::Up:
            if (state_ == FrontendState::MainMenu || state_ == FrontendState::Options) {
                move_selection(-1);
            }
            break;
        case FrontendAction::Down:
            if (state_ == FrontendState::MainMenu || state_ == FrontendState::Options) {
                move_selection(1);
            }
            break;
        case FrontendAction::Back:
            if (state_ == FrontendState::MainMenu) enter(FrontendState::Title);
            else if (state_ == FrontendState::Options) enter(FrontendState::MainMenu);
            break;
        case FrontendAction::Accept:
            if (state_ == FrontendState::Title) {
                enter(FrontendState::MainMenu);
            } else if (state_ == FrontendState::MainMenu && !menu_items_.empty()) {
                const auto& id = menu_items_[selected_item_].id;
                if (id == "new_game" || id == "load_game") enter(FrontendState::Loading);
                else if (id == "options") enter(FrontendState::Options);
                else if (id == "exit") quit_requested_ = true;
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

bool FrontendController::select_menu_item(std::string_view id) {
    const auto it = std::ranges::find(menu_items_, id, &MenuItem::id);
    if (it == menu_items_.end() || !it->enabled) return false;
    selected_item_ = static_cast<std::size_t>(std::distance(menu_items_.begin(), it));
    return true;
}

void FrontendController::enter(FrontendState next) {
    state_ = next;
    state_time_ = 0.0;
    selected_item_ = 0;
}

void FrontendController::move_selection(int direction) {
    if (menu_items_.empty()) return;
    const auto count = static_cast<int>(menu_items_.size());
    int next = static_cast<int>(selected_item_) + direction;
    next = (next % count + count) % count;
    selected_item_ = static_cast<std::size_t>(next);
    for (int i = 0; i < count && !menu_items_[selected_item_].enabled; ++i) {
        selected_item_ = static_cast<std::size_t>((next + direction * (i + 1) + count * 2) % count);
    }
}

}  // namespace f2
