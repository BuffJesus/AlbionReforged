#include "f2/native_audio.h"
#include "f2/native_font.h"
#include "f2/native_frontend_config.h"
#include "f2/native_game.h"
#include "f2/native_input_state.h"
#include "f2/native_script_systems.h"
#include "f2/native_entity.h"
#include "f2/native_world.h"
#include "f2/native_gdb_hash.h"
#include "f2/native_physics.h"
#include "f2/native_camera.h"
#include "f2/native_player.h"
#include "f2/native_install.h"
#include "f2/native_scene.h"
#include "f2/native_texture.h"
#include "f2/native_ui.h"
#include "f2/render/null_render_backend.h"
#include "f2/render/texture_registry.h"
#include "f2/render/ui_draw_list.h"

#include <windows.h>

#include <cassert>
#include <cmath>
#include <algorithm>
#include <memory>
#include <vector>
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

        // TextureRegistry: stable key -> TextureId -> backend handle (the resolver indirection).
        f2::render::TextureRegistry registry;
        const auto id_a = registry.id_for_key(7);
        const auto id_b = registry.id_for_key(42);
        assert(id_a != f2::render::kInvalidTexture && id_b != id_a);
        assert(registry.id_for_key(7) == id_a);  // stable
        registry.set_handle(id_a, 0xDEADBEEFull);
        assert(registry.handle(id_a) == 0xDEADBEEFull);
        assert(registry.handle(id_b) == 0);                       // unset
        assert(registry.handle(f2::render::kInvalidTexture) == 0);  // invalid
        assert(registry.size() == 2);
    }

    {
        // NativeFont: rasterize a real TTF into an RGBA8 atlas + ImGui-compatible glyph metrics.
        // Uses a Windows system font (this is a win32-only frontend); skips cleanly if none exist.
        const std::array<const char*, 3> candidates = {
            "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/tahoma.ttf"};
        std::filesystem::path font_path;
        for (const char* c : candidates) {
            if (std::filesystem::exists(c)) { font_path = c; break; }
        }
        if (!font_path.empty()) {
            f2::NativeFont font;
            std::string font_error;
            assert(font.load(font_path, 48.0f, font_error));
            assert(font.ready());
            assert(font.pixel_height() == 48.0f);
            assert(font.atlas_width() == 1024 && font.atlas_height() == 1024);
            assert(font.atlas_rgba8().size() == 1024u * 1024u * 4u);
            // 'A' must be a valid glyph with a positive advance and a non-degenerate UV rect.
            const auto& glyph_a = font.glyph(static_cast<std::uint32_t>('A'));
            assert(glyph_a.valid);
            assert(glyph_a.advance > 0.0f);
            assert(glyph_a.u1 > glyph_a.u0 && glyph_a.v1 > glyph_a.v0);
            // Space advances but draws nothing; out-of-range codepoints are invalid.
            assert(font.glyph(static_cast<std::uint32_t>(' ')).advance > 0.0f);
            assert(!font.glyph(0u).valid && !font.glyph(300u).valid);  // out of Latin-1 range
            // measure() scales advances by size/pixel_height and sums; wider text => wider measure.
            assert(font.measure("A", 48.0f) > 0.0f);
            assert(font.measure("AA", 48.0f) > font.measure("A", 48.0f));
            assert(font.measure("A", 96.0f) > font.measure("A", 48.0f));
        }
    }

    {
        // Options navigation: inside a tab you move a row cursor (Up/Down) and change the focused
        // value (Left/Right) — the fix for "can't cycle options within a tab". Also covers the FPS toggle.
        using A = f2::FrontendAction;
        using S = f2::FrontendState;
        f2::FrontendController fc;
        fc.dispatch(A::Skip);  // Boot -> Title
        assert(fc.state() == S::Title);
        fc.dispatch(A::Accept);  // Title -> MainMenu
        assert(fc.state() == S::MainMenu);
        assert(fc.select_menu_item("options"));
        fc.dispatch(A::Accept);  // -> Options (tabs), page closed
        assert(fc.state() == S::Options);
        assert(!fc.options_page_open());

        // Move to the Video tab (Up/Down cycles tabs while the page is closed).
        for (int i = 0; i < 8 && fc.options_items()[fc.selected_item()].id != "video"; ++i) {
            fc.dispatch(A::Down);
        }
        assert(fc.options_items()[fc.selected_item()].id == "video");

        // Open the tab: the row cursor starts at 0 and Up/Down now moves it (not the tab).
        fc.dispatch(A::Accept);
        assert(fc.options_page_open());
        assert(fc.option_row() == 0);
        assert(fc.option_row_count() == 5);  // Gamma, Resolution, AA, FPS, Renderer
        fc.dispatch(A::Down);
        assert(fc.option_row() == 1);
        fc.dispatch(A::Down);
        fc.dispatch(A::Down);
        assert(fc.option_row() == 3);  // FPS Display row

        // FPS toggle via Left/Right on its row.
        assert(!fc.fps_display_enabled());
        fc.dispatch(A::Right);
        assert(fc.fps_display_enabled());
        fc.dispatch(A::Left);
        assert(!fc.fps_display_enabled());

        // Cursor clamps at the last row.
        fc.dispatch(A::Down);
        assert(fc.option_row() == 4);
        fc.dispatch(A::Down);
        assert(fc.option_row() == 4);

        // Resolution (row 1) changes with Left/Right.
        while (fc.option_row() > 1) fc.dispatch(A::Up);
        assert(fc.option_row() == 1);
        const int res0 = fc.resolution_index();
        fc.dispatch(A::Right);
        assert(fc.resolution_index() == (res0 < 2 ? res0 + 1 : 2));

        // Back closes the page but stays in Options.
        fc.dispatch(A::Back);
        assert(!fc.options_page_open());
        assert(fc.state() == S::Options);

        // The Game tab exposes 5 rows including the toggles reachable by the cursor.
        while (fc.options_items()[fc.selected_item()].id != "game") fc.dispatch(A::Up);
        fc.dispatch(A::Accept);
        assert(fc.option_row_count() == 5);
        const bool subs0 = fc.subtitles_enabled();
        fc.dispatch(A::Left);  // row 0 = Subtitles -> Off
        assert(fc.subtitles_enabled() == false);
        (void)subs0;
    }

    {
        // Options persistence: save_options/read_options round-trip + FrontendController reload,
        // isolated to a temp config dir (FABLE2NATIVE_CONFIG_DIR) so the real options.ini is untouched.
        const auto cfg = std::filesystem::temp_directory_path() / "f2native_cfg_test";
        std::filesystem::create_directories(cfg);
        SetEnvironmentVariableW(L"FABLE2NATIVE_CONFIG_DIR", cfg.wstring().c_str());
        std::filesystem::remove(cfg / "options.ini");
        f2::FrontendOptions written;
        written.subtitles = false;
        written.anti_aliasing_index = 3;
        written.resolution_index = 2;
        written.gamma_percent = 73;
        written.sounds_volume = 42;
        written.music_volume = 17;
        written.fps_display = true;
        f2::save_options(written);
        const f2::FrontendOptions read_back = f2::read_options();
        assert(read_back.subtitles == false);
        assert(read_back.anti_aliasing_index == 3);
        assert(read_back.resolution_index == 2);
        assert(read_back.gamma_percent == 73);
        assert(read_back.sounds_volume == 42);
        assert(read_back.music_volume == 17);
        assert(read_back.fps_display == true);
        // A controller picks the persisted options up on reset().
        f2::FrontendController fc;
        fc.reset();
        assert(fc.anti_aliasing_index() == 3);
        assert(fc.resolution_index() == 2);
        assert(fc.gamma_percent() == 73);
        assert(fc.sounds_volume() == 42);
        assert(fc.fps_display_enabled() == true);
        assert(fc.subtitles_enabled() == false);
        SetEnvironmentVariableW(L"FABLE2NATIVE_CONFIG_DIR", nullptr);
        std::filesystem::remove(cfg / "options.ini");
        std::filesystem::remove(cfg / "renderer.txt");
    }

    {
        // Legacy F2SCENE water materials omit water_params=. They must still use the retail
        // defaults instead of uploading an all-zero constant block to either backend.
        const auto legacy_path = std::filesystem::temp_directory_path() /
                                 "f2native_legacy_water.f2scene";
        {
            std::ofstream output(legacy_path);
            output << "F2SCENE 1\n"
                   << "material water 0.14 0.34 0.52 1\n";
        }
        f2::NativeScene legacy;
        std::string legacy_error;
        assert(f2::load_native_scene(legacy_path, legacy, legacy_error));
        assert(legacy.materials.size() == 1);
        assert(!legacy.materials[0].has_water_params);
        assert(std::abs(legacy.materials[0].water_params[0] - 0.20f) < 1e-5f);
        assert(std::abs(legacy.materials[0].water_params[6] - 0.188f) < 1e-5f);
        assert(std::abs(legacy.materials[0].water_params[29] - 0.75f) < 1e-5f);
        assert(std::abs(legacy.materials[0].water_params[36] - 128.0f) < 1e-5f);
        std::filesystem::remove(legacy_path);
    }

    {
        // F2SCENE writer round-trip: the level cooker's output stage must reload identically.
        f2::NativeScene written;
        written.sun_direction = {0.1f, -0.9f, 0.4f};
        written.sky_color = {0.2f, 0.3f, 0.4f, 1.0f};
        written.has_hero_start = true;
        written.hero_start = {12.5f, 1.25f, -8.25f};
        written.hero_yaw = -0.75f;
        f2::NativeMaterial mat;
        mat.name = "wall_stone";
        mat.base_color = {0.8f, 0.7f, 0.6f, 1.0f};
        mat.albedo = "worlds/albion/bwsslums/wall.dds";
        mat.has_water_params = true;
        for (std::size_t i = 0; i < mat.water_params.size(); ++i)
            mat.water_params[i] = static_cast<float>(i) * 0.125f - 1.0f;
        mat.water_opacity = 0.42f;
        written.materials.push_back(mat);
        f2::NativeMesh mesh;
        mesh.name = "house_01";
        mesh.material = 0;
        mesh.vertices.push_back({{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}});
        mesh.vertices.push_back({{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}});
        mesh.vertices.push_back({{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}});
        mesh.indices = {0, 1, 2};
        written.meshes.push_back(mesh);
        written.instances.push_back({0, {12.5f, 0.0f, -8.25f}, {0.0f, 1.5708f, 0.0f}, 2.0f});
        // Cloud layers (theme Clouds) must round-trip through the F2SCENE writer/reader too.
        written.cloud_global_brightness = 1.0f;
        written.cloud_alpha_ref = 0.019608f;
        f2::NativeCloudLayer cloud;
        cloud.density_map = "clouds/cloud_03.dds";
        cloud.height = 500.0f;
        cloud.size_x = 3000.0f;
        cloud.size_y = 3000.0f;
        cloud.texture_scale_x = 3.0f;
        cloud.texture_scale_y = 5.0f;
        cloud.velocity_x = 10.0f;
        cloud.velocity_y = 20.0f;
        cloud.transparency = 0.5f;
        cloud.brightness = 0.25f;
        cloud.ambient = 0.9f;
        cloud.normal_strength = 0.5f;
        written.clouds.push_back(cloud);
        // Celestial moon billboard must round-trip too.
        written.has_moon = true;
        written.moon.texture = "sky/moonphases.dds";
        written.moon.glare_texture = "sky/sunglare.dds";
        written.moon.direction = {0.566f, 0.766f, -0.305f};
        written.moon.intensity = 1.0f;
        written.moon.size = 1.7f;
        written.moon.transparency = 3.0f;
        written.moon.glare_intensity = 50.0f;
        written.moon.glare_size = 1.4f;
        written.moon.exposure = 10.0f;
        written.moon.phase = 4;
        written.star_brightness = 1.0f;

        const auto scene_path = std::filesystem::temp_directory_path() / "f2native_scene_roundtrip.f2scene";
        std::string scene_error;
        assert(f2::save_native_scene(scene_path, written, scene_error));
        f2::NativeScene reloaded;
        assert(f2::load_native_scene(scene_path, reloaded, scene_error));
        assert(reloaded.materials.size() == 1);
        assert(reloaded.materials[0].name == "wall_stone");
        assert(reloaded.materials[0].albedo == "worlds/albion/bwsslums/wall.dds");
        assert(reloaded.materials[0].has_water_params);
        assert(reloaded.has_hero_start);
        assert(std::abs(reloaded.hero_start[0] - 12.5f) < 1e-5f);
        assert(std::abs(reloaded.hero_start[1] - 1.25f) < 1e-5f);
        assert(std::abs(reloaded.hero_start[2] + 8.25f) < 1e-5f);
        assert(std::abs(reloaded.hero_yaw + 0.75f) < 1e-5f);
        for (std::size_t i = 0; i < reloaded.materials[0].water_params.size(); ++i)
            assert(approx(reloaded.materials[0].water_params[i],
                          static_cast<float>(i) * 0.125f - 1.0f));
        assert(approx(reloaded.materials[0].water_opacity, 0.42f));
        assert(reloaded.meshes.size() == 1);
        assert(reloaded.meshes[0].name == "house_01");
        assert(reloaded.meshes[0].vertices.size() == 3);
        assert(reloaded.meshes[0].indices == std::vector<std::uint32_t>({0, 1, 2}));
        assert(reloaded.instances.size() == 1);
        assert(reloaded.instances[0].mesh == 0);
        const auto approx = [](float a, float b) { return std::abs(a - b) < 1e-5f; };
        assert(approx(reloaded.instances[0].position[0], 12.5f));
        assert(approx(reloaded.instances[0].position[2], -8.25f));
        assert(approx(reloaded.instances[0].rotation[1], 1.5708f));
        assert(approx(reloaded.instances[0].scale, 2.0f));
        assert(approx(reloaded.sun_direction[1], -0.9f));
        assert(approx(reloaded.sky_color[2], 0.4f));
        assert(reloaded.clouds.size() == 1);
        assert(reloaded.clouds[0].density_map == "clouds/cloud_03.dds");
        assert(approx(reloaded.clouds[0].height, 500.0f));
        assert(approx(reloaded.clouds[0].size_x, 3000.0f));
        assert(approx(reloaded.clouds[0].texture_scale_y, 5.0f));
        assert(approx(reloaded.clouds[0].velocity_y, 20.0f));
        assert(approx(reloaded.clouds[0].transparency, 0.5f));
        assert(approx(reloaded.clouds[0].ambient, 0.9f));
        assert(approx(reloaded.clouds[0].normal_strength, 0.5f));
        assert(approx(reloaded.cloud_alpha_ref, 0.019608f));
        assert(reloaded.has_moon);
        assert(reloaded.moon.texture == "sky/moonphases.dds");
        assert(reloaded.moon.glare_texture == "sky/sunglare.dds");
        assert(approx(reloaded.moon.direction[1], 0.766f));
        assert(approx(reloaded.moon.intensity, 1.0f));
        assert(approx(reloaded.moon.glare_intensity, 50.0f));
        assert(reloaded.moon.phase == 4);
        assert(approx(reloaded.star_brightness, 1.0f));
        std::filesystem::remove(scene_path);
    }

    // ---- P0: master gameplay tick order (Quest -> General -> AI) ----
    {
        f2::ScriptSystems ss;
        std::string order;
        ss.quest.enabled = true;
        ss.quest.update = [&](double) { order += 'Q'; };
        ss.general.enabled = true;
        ss.general.update = [&](double) { order += 'G'; };
        ss.ai.enabled = true;
        ss.ai.update = [&](double) { order += 'A'; };
        ss.tick(1.0 / 60.0);
        assert(order == "QGA");  // retail-recovered order (Function_82281FE0)

        // Disabled managers are skipped; order of the rest preserved.
        order.clear();
        ss.general.enabled = false;
        ss.tick(1.0 / 60.0);
        assert(order == "QA");
    }

    // ---- P0: analog stick deadzone/extent curve (decomp-grounded thresholds) ----
    {
        f2::InputAnalogConfig cfg;  // deadzone 0.4, extent 0.8 (shipped defaults)
        // Inside the deadzone reads as zero.
        auto z = f2::InputSampler::apply_stick_curve({0.3f, 0.0f}, cfg);
        assert(approx(z[0], 0.0f) && approx(z[1], 0.0f));
        // At/above the extent threshold saturates to magnitude 1.0.
        auto s = f2::InputSampler::apply_stick_curve({0.8f, 0.0f}, cfg);
        assert(approx(s[0], 1.0f) && approx(s[1], 0.0f));
        // Halfway between deadzone(0.4) and extent(0.8) -> 0.5 magnitude.
        auto m = f2::InputSampler::apply_stick_curve({0.6f, 0.0f}, cfg);
        assert(approx(m[0], 0.5f) && approx(m[1], 0.0f));
        // Direction preserved on a diagonal past the deadzone.
        auto d = f2::InputSampler::apply_stick_curve({0.8f, 0.8f}, cfg);
        assert(d[0] > 0.0f && approx(d[0], d[1]));  // symmetric input -> symmetric output
    }

    // ---- P0: button edge-detect helpers ----
    {
        f2::InputState in;
        in.buttons = static_cast<std::uint16_t>(f2::PadButton::A);
        in.buttons_pressed = static_cast<std::uint16_t>(f2::PadButton::A);
        assert(in.down(f2::PadButton::A));
        assert(in.pressed(f2::PadButton::A));
        assert(!in.down(f2::PadButton::B));
        assert(!in.pressed(f2::PadButton::B));
        // Default active device + a Frontend-mode game does not tick script systems.
        f2::NativeGame g;
        assert(g.mode == f2::GameMode::Frontend);
    }

    // ---- P1: FNV-1 hasher matches byte-verified GDB hashes ----
    {
        assert(f2::gdb::fnv1("RemoveComponent") == 0x9B41D00Au);
        assert(f2::gdb::fnv1("Position") == 0xBD7C27D4u);
        assert(f2::gdb::fnv1("GraphicAppearanceStaticMeshComponent") == 0x29CF50D1u);
        assert(f2::gdb::fnv1("") == f2::gdb::kFnvBasis);
    }

    // ---- P1: component registry lookup (sorted bsearch) ----
    {
        f2::ComponentRegistry reg;
        reg.seed_defaults();
        const auto* d = reg.lookup(f2::gdb::kCompGraphicAppearanceStaticMesh);
        assert(d != nullptr);
        assert(d->type_id == f2::kTypeIdGraphicAppearanceStaticMesh);
        assert(reg.lookup(0xDEADBEEFu) == nullptr);  // unregistered -> null
    }

    // ---- P1: entity factory dup-gate + sorted-by-typeId insert ----
    {
        f2::EntityManager em;
        f2::NativeEntity& e = em.create_entity(0x1234);
        assert(e.uid == 1);
        auto* c1 = em.create_component_by_hash(e, f2::gdb::kCompGraphicAppearanceStaticMesh);
        assert(c1 != nullptr && e.component_count() == 1);
        // Second create of the same typeId is gated (entity+0x24 dup mask): no dup.
        auto* c2 = em.create_component_by_hash(e, f2::gdb::kCompGraphicAppearanceStaticMesh);
        assert(c2 == c1 && e.component_count() == 1);
        // A second, distinct entity gets the next UID.
        assert(em.create_entity().uid == 2);
        // Direct add of Transform (engine-special) coexists, sorted by typeId.
        e.add_component(std::make_unique<f2::TransformComponent>());
        assert(e.component_count() == 2);
        assert(e.component_by_typeid(f2::kTypeIdTransform) != nullptr);
        assert(e.component_by_typeid(f2::kTypeIdGraphicAppearanceStaticMesh) != nullptr);
    }

    // ---- P1: collect_component_hashes (parent-first union minus RemoveComponent) ----
    {
        // Synthetic 3-level chain: grandparent -> parent -> child.
        // grandparent declares {A, B}; parent adds {C}; child removes {B}, adds {D}.
        struct FakeSource : f2::GdbRecordSource {
            bool parent_of(std::uint32_t guid, std::uint32_t& out) const override {
                if (guid == 3) { out = 2; return true; }  // child -> parent
                if (guid == 2) { out = 1; return true; }  // parent -> grandparent
                return false;                              // grandparent -> none
            }
            std::vector<std::uint32_t> component_fields(std::uint32_t guid) const override {
                if (guid == 1) return {0xA, 0xB};
                if (guid == 2) return {0xC};
                if (guid == 3) return {0xD};
                return {};
            }
            std::vector<std::uint32_t> removed_components(std::uint32_t guid) const override {
                if (guid == 3) return {0xB};  // child prunes inherited B
                return {};
            }
        } src;
        auto set = f2::collect_component_hashes(3, src);
        // Expect A, C, D (B pruned); ancestors first.
        assert(set.size() == 3);
        assert(std::find(set.begin(), set.end(), 0xAu) != set.end());
        assert(std::find(set.begin(), set.end(), 0xCu) != set.end());
        assert(std::find(set.begin(), set.end(), 0xDu) != set.end());
        assert(std::find(set.begin(), set.end(), 0xBu) == set.end());
    }

    // ---- P1: entity -> scene.instances transform bridge ----
    {
        f2::NativeScene s;
        f2::NativeInstance inst;
        inst.position = {1.0f, 2.0f, 3.0f};
        s.instances.push_back(inst);

        f2::NativeWorld w;
        w.spawn_from_scene(s);
        assert(w.entities.entity_count() == 1);

        // Move the entity's transform, sync, and confirm the drawn instance moved.
        f2::NativeEntity* e = w.entities.find(1);
        assert(e != nullptr);
        auto* t = e->get<f2::TransformComponent>(f2::kTypeIdTransform);
        assert(t != nullptr);
        assert(approx(t->position[0], 1.0f));  // seeded from the instance
        t->position = {10.0f, 20.0f, 30.0f};
        w.sync_to_scene(s);
        assert(approx(s.instances[0].position[0], 10.0f));
        assert(approx(s.instances[0].position[1], 20.0f));
        assert(approx(s.instances[0].position[2], 30.0f));
    }

    // ---- P2: collision heightfield ground-clamp + collide-and-slide ----
    {
        // A flat 20x20 terrain quad at y=0 (two triangles), plus a wall box.
        f2::NativeScene s;
        f2::NativeMesh ground;
        auto add_v = [&](float x, float y, float z) {
            f2::NativeVertex v; v.position = {x, y, z}; ground.vertices.push_back(v);
        };
        add_v(-10.0f, 0.0f, -10.0f); add_v(10.0f, 0.0f, -10.0f);
        add_v(10.0f, 0.0f, 10.0f);   add_v(-10.0f, 0.0f, 10.0f);
        ground.indices = {0, 1, 2, 0, 2, 3};
        s.meshes.push_back(ground);
        f2::NativeInstance gi; gi.mesh = 0; s.instances.push_back(gi);
        // A small wall block mesh (unit cube) placed at x=5.
        f2::NativeMesh cube;
        for (float xx : {-0.5f, 0.5f}) for (float yy : {0.0f, 2.0f}) for (float zz : {-0.5f, 0.5f}) {
            f2::NativeVertex v; v.position = {xx, yy, zz}; cube.vertices.push_back(v);
        }
        cube.indices = {0, 1, 2};  // geometry irrelevant; AABB is what matters
        s.meshes.push_back(cube);
        f2::NativeInstance ci; ci.mesh = 1; ci.position = {5.0f, 0.0f, 0.0f}; s.instances.push_back(ci);

        f2::NativeCollisionWorld world;
        world.build_from_scene(s);
        assert(world.has_terrain());
        // Ground sample over the flat quad returns ~0.
        float gy = world.sample_ground(0.0f, 0.0f, 3.0f);
        assert(std::isfinite(gy) && approx(gy, 0.0f));

        // Controller starts above ground, falls, and clamps to the terrain.
        f2::CharacterController cc;
        assert(approx(cc.config.capsule_radius, 1.0f));  // GROUNDED value
        cc.position = {0.0f, 0.5f, 0.0f};
        for (int i = 0; i < 30; ++i) cc.move(world, {0.0f, 0.0f}, 1.0f / 60.0f);
        assert(cc.on_ground && approx(cc.position[1], 0.0f));

        // Walking east into the wall box gets stopped/slid (x cannot pass the box face).
        cc.position = {0.0f, 0.0f, 0.0f};
        for (int i = 0; i < 120; ++i) cc.move(world, {5.0f, 0.0f}, 1.0f / 60.0f);  // +x
        assert(cc.position[0] < 5.0f - 0.5f + 0.001f);  // blocked before the box interior
    }

    // ---- P2: wall raycast (camera-collision primitive) ----
    {
        f2::NativeScene s;
        f2::NativeMesh ground;
        auto gv = [&](float x, float y, float z) {
            f2::NativeVertex v; v.position = {x, y, z}; ground.vertices.push_back(v);
        };
        gv(-20, 0, -20); gv(20, 0, -20); gv(20, 0, 20); gv(-20, 0, 20);
        ground.indices = {0, 1, 2, 0, 2, 3};
        s.meshes.push_back(ground);
        f2::NativeInstance gi; gi.mesh = 0; s.instances.push_back(gi);
        // Vertical wall quad in the x=5 plane, z in [-2,2], y in [0,3].
        f2::NativeMesh wall;
        auto wv = [&](float x, float y, float z) {
            f2::NativeVertex v; v.position = {x, y, z}; wall.vertices.push_back(v);
        };
        wv(5, 0, -2); wv(5, 0, 2); wv(5, 3, 2); wv(5, 3, -2);
        wall.indices = {0, 1, 2, 0, 2, 3};
        s.meshes.push_back(wall);
        f2::NativeInstance wi; wi.mesh = 1; s.instances.push_back(wi);

        f2::NativeCollisionWorld world;
        world.build_from_scene(s);
        assert(world.wall_triangle_count() >= 2);
        // Ray toward +x at wall height hits at ~5.
        assert(approx(world.raycast_walls({0, 1, 0}, {1, 0, 0}, 20.0f), 5.0f));
        // Opposite direction misses -> returns max_dist.
        assert(approx(world.raycast_walls({0, 1, 0}, {-1, 0, 0}, 20.0f), 20.0f));
        // Above the wall (y=5 > top 3) misses.
        assert(approx(world.raycast_walls({0, 5, 0}, {1, 0, 0}, 20.0f), 20.0f));

        // Camera boom pulls in when a wall is behind the hero.
        f2::NativeScene s2;
        s2.meshes.push_back(ground);
        f2::NativeInstance gi2; gi2.mesh = 0; s2.instances.push_back(gi2);
        f2::NativeMesh backwall;  // z = -2 plane, x in [-2,2], y in [0,3]
        auto bv = [&](float x, float y, float z) {
            f2::NativeVertex v; v.position = {x, y, z}; backwall.vertices.push_back(v);
        };
        bv(-2, 0, -2); bv(2, 0, -2); bv(2, 3, -2); bv(-2, 3, -2);
        backwall.indices = {0, 1, 2, 0, 2, 3};
        s2.meshes.push_back(backwall);
        f2::NativeInstance wi2; wi2.mesh = 1; s2.instances.push_back(wi2);
        f2::NativeCollisionWorld world2;
        world2.build_from_scene(s2);

        f2::CameraController cam;
        cam.yaw = 0.0f; cam.pitch = 0.0f;  // forward = +z, so eye pulls back to -z
        cam.config.distance = 4.5f;
        cam.update({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, false, 1.0f / 60.0f, &world2);
        // Ideal eye would be z=-4.5; the wall at z=-2 pulls it in to ~-1.7 (2 - margin 0.3).
        assert(cam.position[2] > -2.0f);           // in front of the wall (not clipped)
        assert(approx(cam.position[2], -1.7f));
    }

    // ---- P2: follow-camera pose matches the renderer forward convention ----
    {
        f2::CameraController cam;
        cam.yaw = 0.0f; cam.pitch = 0.0f;
        cam.config.distance = 4.0f; cam.config.height = 1.6f;
        std::array<float, 3> hero{0.0f, 0.0f, 0.0f};
        cam.update(hero, {0.0f, 0.0f}, /*mouse*/ false, 1.0f / 60.0f);
        // yaw=pitch=0 -> forward = (0,0,1); camera pulls back along -z from target.
        assert(approx(cam.target[1], 1.6f));
        assert(approx(cam.position[2], -4.0f));
        assert(approx(cam.position[1], 1.6f));
        assert(approx(cam.fov_y(), 1.22171938f));  // GROUNDED 70deg
        // Right-stick yaw integrates over time.
        float y0 = cam.yaw;
        cam.update(hero, {1.0f, 0.0f}, false, 0.1f);
        assert(cam.yaw > y0);
    }

    // ---- P2: player camera-relative desired velocity ----
    {
        // Facing yaw=0 (forward = +z). Pushing forward (move.y=1) -> +z velocity.
        auto v = f2::NativePlayer::desired_velocity({0.0f, 1.0f}, 0.0f, 2.5f);
        assert(approx(v[0], 0.0f) && approx(v[1], 2.5f));
        // Strafe right (move.x=1) at yaw=0 -> +x velocity.
        v = f2::NativePlayer::desired_velocity({1.0f, 0.0f}, 0.0f, 2.5f);
        assert(approx(v[0], 2.5f) && approx(v[1], 0.0f));
        // Yaw 90deg rotates "forward" to +x.
        v = f2::NativePlayer::desired_velocity({0.0f, 1.0f}, 3.14159265f / 2.0f, 2.0f);
        assert(approx(v[0], 2.0f) && std::abs(v[1]) < 1e-4f);
    }

    std::filesystem::remove(path);
    return 0;
}
