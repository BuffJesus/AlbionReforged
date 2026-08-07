#include "f2/native_audio.h"
#include "f2/native_game.h"
#include "f2/native_install.h"
#include "f2/native_texture.h"
#include "f2/native_ui.h"
#include "f2/render/null_render_backend.h"
#include "f2/render/ui_draw_list.h"

#include <cassert>
#include <array>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>

int main() {
    const std::array<std::uint8_t, 136> dxt1 = [] {
        std::array<std::uint8_t, 136> bytes{};
        bytes[0] = 'D'; bytes[1] = 'D'; bytes[2] = 'S'; bytes[3] = ' ';
        auto write = [&](std::size_t offset, std::uint32_t value) {
            std::memcpy(bytes.data() + offset, &value, sizeof(value));
        };
        write(4, 124); write(12, 4); write(16, 4); write(76, 32);
        bytes[80] = 4; bytes[84] = 'D'; bytes[85] = 'X'; bytes[86] = 'T'; bytes[87] = '1';
        bytes[128] = 0xff; bytes[129] = 0xff; // white endpoint
        bytes[130] = 0; bytes[131] = 0; // black endpoint
        return bytes;
    }();
    f2::NativeTexture decoded_texture;
    std::string texture_error;
    assert(f2::decode_dds_rgba8(dxt1, decoded_texture, texture_error));
    assert(decoded_texture.width == 4 && decoded_texture.height == 4);
    assert(decoded_texture.rgba8.size() == 64);

    const auto ui_root = std::filesystem::temp_directory_path() / "f2native_ui_test";
    std::filesystem::create_directories(ui_root);
    std::ofstream(ui_root / "title_background.dds", std::ios::binary)
        .write(reinterpret_cast<const char*>(dxt1.data()), static_cast<std::streamsize>(dxt1.size()));
    auto black_dxt1 = dxt1;
    black_dxt1[128] = 0; black_dxt1[129] = 0;
    black_dxt1[130] = 0; black_dxt1[131] = 0;
    std::ofstream(ui_root / "ambient_atlas.dds", std::ios::binary)
        .write(reinterpret_cast<const char*>(black_dxt1.data()),
               static_cast<std::streamsize>(black_dxt1.size()));
    std::ofstream(ui_root / "ambient_baseline.dds", std::ios::binary)
        .write(reinterpret_cast<const char*>(dxt1.data()), static_cast<std::streamsize>(dxt1.size()));
    std::ofstream(ui_root / "ambient_detail.dds", std::ios::binary)
        .write(reinterpret_cast<const char*>(dxt1.data()), static_cast<std::streamsize>(dxt1.size()));
    std::ofstream(ui_root / "title_font.ttf", std::ios::binary).put('F');
    std::ofstream(ui_root / "ui_manifest.ini")
        << "title_background=title_background.dds\n"
        << "ambient_atlas=ambient_atlas.dds\n"
        << "ambient_baseline=ambient_baseline.dds\n"
        << "ambient_detail=ambient_detail.dds\n"
        << "title_font=title_font.ttf\n";
    f2::NativeUiAssets ui_assets;
    std::string ui_error;
    assert(ui_assets.load(ui_root, ui_error));
    assert(ui_assets.has(f2::NativeUiAsset::TitleBackground));
    assert(ui_assets.texture(f2::NativeUiAsset::TitleBackground)->width == 4);
    assert(ui_assets.has(f2::NativeUiAsset::AmbientAtlas));
    assert(ui_assets.texture(f2::NativeUiAsset::AmbientAtlas)->height == 4);
    assert(ui_assets.has(f2::NativeUiAsset::AmbientBaseline));
    assert(ui_assets.texture(f2::NativeUiAsset::AmbientBaseline)->width == 4);
    assert(ui_assets.has(f2::NativeUiAsset::AmbientDetail));
    assert(ui_assets.texture(f2::NativeUiAsset::AmbientAtlas)->rgba8[0] == 255);
    std::ofstream(ui_root / "ambient_detail_frame0.dds", std::ios::binary)
        .write(reinterpret_cast<const char*>(dxt1.data()), static_cast<std::streamsize>(dxt1.size()));
    std::ofstream(ui_root / "ambient_detail_frame1.dds", std::ios::binary)
        .write(reinterpret_cast<const char*>(black_dxt1.data()),
               static_cast<std::streamsize>(black_dxt1.size()));
    std::ofstream(ui_root / "ui_manifest.ini", std::ios::trunc)
        << "ambient_atlas=ambient_atlas.dds\n"
        << "ambient_detail_frames=ambient_detail_frame0.dds,ambient_detail_frame1.dds\n";
    f2::NativeUiAssets frame_assets;
    assert(frame_assets.load(ui_root, ui_error));
    assert(frame_assets.ambient_detail_frame_count() == 2);
    assert(frame_assets.texture(f2::NativeUiAsset::AmbientAtlas)->rgba8[0] == 0);
    f2::NativeTexture composed_ambient;
    assert(f2::compose_native_ambient_atlas(
        *frame_assets.texture(f2::NativeUiAsset::AmbientAtlas),
        *frame_assets.ambient_detail_frame(0), composed_ambient));
    assert(composed_ambient.rgba8[0] == 255);
    assert(ui_assets.title_font_path().filename() == "title_font.ttf");
    std::filesystem::remove_all(ui_root);

    const auto path = std::filesystem::temp_directory_path() / "f2native_core_test.f2scene";
    {
        std::ofstream out(path);
        out << "F2SCENE 1\n"
            << "material stone 0.7 0.7 0.7 1 albedo=pubgames/common/bar_focus.dds\n"
            << "mesh floor 3 3 0\n"
            << "vertex 0 0 0 0 1 0 0 0\n"
            << "vertex 1 0 0 0 1 0 1 0\n"
            << "vertex 0 0 1 0 1 0 0 1\n"
            << "index 0\nindex 1\nindex 2\n"
            << "instance floor 0 0 0 0 0 0 1\n";
    }

    f2::NativeGame game;
    std::string error;
    assert(game.load_scene(path, error));
    assert(game.scene.meshes.size() == 1);
    assert(game.scene.instances.size() == 1);
    assert(game.scene.materials[0].albedo == "pubgames/common/bar_focus.dds");
    game.tick(1.0 / 30.0);
    assert(game.elapsed_seconds > 0.0);
    game.tick(0.5);
    assert(game.frontend.state() == f2::FrontendState::IntroVideo);
    assert(game.frontend.intro_videos().current_clip() != nullptr);
    assert(game.frontend.intro_videos().current_clip()->id == "microsoft_logo");
    game.frontend.dispatch(f2::FrontendAction::Skip);
    assert(game.frontend.state() == f2::FrontendState::Title);
    assert(game.frontend.frontend_music_active());
    game.frontend.dispatch(f2::FrontendAction::Accept);
    assert(game.frontend.state() == f2::FrontendState::MainMenu);
    assert(game.frontend.frontend_music_active());
    assert(!game.frontend.select_card(true));
    assert(game.frontend.select_menu_item("new_game"));
    game.frontend.dispatch(f2::FrontendAction::Accept);
    assert(game.frontend.state() == f2::FrontendState::ChooseCard);
    assert(game.frontend.frontend_music_active());
    assert(!game.frontend.girl_selected());
    assert(game.frontend.select_card(true));
    assert(game.frontend.girl_selected());
    assert(game.frontend.select_card(false));
    assert(!game.frontend.girl_selected());
    game.frontend.dispatch(f2::FrontendAction::Right);
    assert(game.frontend.girl_selected());
    game.frontend.dispatch(f2::FrontendAction::Left);
    assert(!game.frontend.girl_selected());
    game.frontend.dispatch(f2::FrontendAction::Back);
    assert(game.frontend.state() == f2::FrontendState::MainMenu);
    assert(game.frontend.options_items().size() == 4);
    assert(game.frontend.select_menu_item("options"));
    game.frontend.dispatch(f2::FrontendAction::Accept);
    assert(game.frontend.state() == f2::FrontendState::Options);
    assert(game.frontend.frontend_music_active());
    assert(game.frontend.sound_enabled());
    assert(game.frontend.select_menu_item("game"));
    game.frontend.dispatch(f2::FrontendAction::Accept);
    assert(!game.frontend.subtitles_enabled());
    game.frontend.dispatch(f2::FrontendAction::Accept);
    assert(game.frontend.subtitles_enabled());
    assert(game.frontend.select_menu_item("controls"));
    game.frontend.dispatch(f2::FrontendAction::Back);
    assert(game.frontend.state() == f2::FrontendState::MainMenu);
    assert(game.frontend.select_menu_item("options"));
    game.frontend.dispatch(f2::FrontendAction::Accept);
    assert(game.frontend.state() == f2::FrontendState::Options);
    assert(game.frontend.remove_menu_item("options"));

    const auto source_root = std::filesystem::temp_directory_path() / "f2native_source_test";
    const auto data_root = source_root / "data";
    std::filesystem::create_directories(data_root / "art/videos");
    for (const auto* relative_path : {"dir.manifest", "art/videos/microsoft_logo.bik",
                                      "art/videos/lionhead_logo.bik", "art/videos/intro.bik"}) {
        std::ofstream(data_root / relative_path) << "test";
    }
    std::string source_error;
    const auto source = f2::detect_game_source(source_root, source_error);
    assert(source.has_value());
    assert(source->data_root == std::filesystem::weakly_canonical(data_root));
    std::filesystem::remove_all(source_root);

    // Frontend sound identities are keyed to the retail SE_GUI event names recovered from the
    // XEX (docs/RETAIL_FRONTEND_SPEC.md §8). Lock the mapping so it cannot silently drift.
    using S = f2::NativeFrontendSound;
    assert(f2::se_gui_event_name(S::NavigateUp) == "SE_GUI_SLIDE_MENU_UP");
    assert(f2::se_gui_event_name(S::NavigateDown) == "SE_GUI_SLIDE_MENU_DOWN");
    assert(f2::se_gui_event_name(S::SelectionLeft) == "SE_GUI_SELECTION_LEFT");
    assert(f2::se_gui_event_name(S::SelectionRight) == "SE_GUI_SELECTION_RIGHT");
    assert(f2::se_gui_event_name(S::Accept) == "SE_GUI_MENU_BOX_SELECT");
    assert(f2::se_gui_event_name(S::Back) == "SE_GUI_MENU_BOX_CANCEL");
    assert(f2::se_gui_event_name(S::Music).empty());

    // Options "Resolution" maps each index to a concrete PC pixel size the app applies to the
    // window/swapchain (docs §6). resolution_width/height are pure functions of the index.
    assert(f2::FrontendController::resolution_option_count() == 3);
    {
        const f2::FrontendController fc;  // default index → a valid, real resolution
        assert(fc.resolution_width() > 0 && fc.resolution_height() > 0);
        assert(fc.resolution_width() >= fc.resolution_height());  // landscape
    }

    // The render-backend contract (f2::render) must be drivable with no GPU — proves the frontend
    // scene layer is decoupled from D3D12/Vulkan. Exercise it through the headless NullRenderBackend.
    {
        f2::render::NullRenderBackend backend;
        std::string backend_error;
        assert(backend.initialize(nullptr, 1280, 720, backend_error));
        assert(backend.initialized() && backend.width() == 1280 && backend.height() == 720);
        const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
        const auto tex = backend.create_texture(white.data(), 1, 1);
        assert(tex != f2::render::kInvalidTexture);
        const std::array<f2::render::UiQuad, 2> quads{{
            {tex, 0, 0, 100, 100},
            {tex, 100, 100, 200, 200},
        }};
        backend.begin_frame();
        backend.draw_ui(quads, 1280, 720);
        backend.present();
        assert(backend.last_quad_count() == 2);
        assert(backend.frames_presented() == 1);
        assert(backend.texture_count() == 1);
        backend.resize(1920, 1080);
        assert(backend.width() == 1920);

        // The backend-neutral scene (UiDrawList) is what a shared scene builder produces and any
        // backend consumes — exercise that path end-to-end.
        f2::render::UiDrawList scene;
        scene.add_sprite(tex, 0, 0, 640, 480);
        scene.add_rect(tex, 0, 0, 10, 10, 0xff0000ffu);
        scene.add_detail_sprite(tex, tex, 0, 0, 128, 128, 0, 0, 1, 1, 0, 0, 1, 1, 0xffffffffu, true);
        scene.set_last_rotation(1.5f);
        assert(scene.size() == 3);
        assert(scene.quads()[2].combine_detail && scene.quads()[2].key_black_matte);
        assert(scene.quads()[2].rotation_radians == 1.5f);
        backend.begin_frame();
        backend.draw_ui(scene.quads(), 1920, 1080);
        backend.present();
        assert(backend.last_quad_count() == 3 && backend.frames_presented() == 2);
    }

    std::filesystem::remove(path);
    return 0;
}
