// terrain_splat_bake - bake a Fable II terrain ground albedo by compositing per-LOD
// ground textures according to the level's .ehf splat map. Faithful port of the
// SPLAT-COMPOSITE part of Fable2AssetBrowser/source/src/Level/LevelLoader.cpp
// (BakeEhfTerrainCompositeWithBnk, ~lines 10947-11285) with the texture-bnk lookup
// replaced by caller-supplied --lod i=path.dds cooked DDS files.
//
//   terrain_splat_bake <in.ehf> <out.dds> --res N --lod <i>=<path.dds> [--lod ...]
//
// Writes an UNCOMPRESSED RGBA8 DDS the native runtime loads, then prints one line:
//   BOUNDS minx=.. minz=.. spanx=.. spanz=.. outw=.. outh=..
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../Fable2AssetBrowser/source/src/Level/EhfChunkParser.h"
#include "../../Fable2AssetBrowser/source/src/Level/TextureAtlasDecoder.h"

// ============================================================================
// DDS decode (ported verbatim from Fable2Native/src/native_texture.cpp
// decode_dds_rgba8 + helpers). Uses a local Img instead of NativeTexture.
// ============================================================================
struct Img {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

namespace {

std::uint32_t read_u32(const std::uint8_t* bytes, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

std::uint16_t read_u16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
}

std::array<std::uint8_t, 4> decode_rgb565(std::uint16_t value) {
    return {
        static_cast<std::uint8_t>(((value >> 11) & 0x1f) * 255 / 31),
        static_cast<std::uint8_t>(((value >> 5) & 0x3f) * 255 / 63),
        static_cast<std::uint8_t>((value & 0x1f) * 255 / 31),
        255,
    };
}

void write_pixel(Img& texture, std::uint32_t x, std::uint32_t y,
                 const std::array<std::uint8_t, 4>& color) {
    if ((int)x >= texture.w || (int)y >= texture.h) return;
    const auto offset = (static_cast<std::size_t>(y) * texture.w + x) * 4;
    std::copy(color.begin(), color.end(), texture.rgba.begin() + offset);
}

void decode_color_block(const std::uint8_t* block, Img& texture,
                        std::uint32_t block_x, std::uint32_t block_y,
                        bool allow_transparent) {
    const auto color0 = read_u16(block);
    const auto color1 = read_u16(block + 2);
    std::array<std::array<std::uint8_t, 4>, 4> colors{
        decode_rgb565(color0), decode_rgb565(color1), {}, {}};
    if (color0 > color1 || !allow_transparent) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            colors[2][channel] = static_cast<std::uint8_t>(
                (2 * colors[0][channel] + colors[1][channel]) / 3);
            colors[3][channel] = static_cast<std::uint8_t>(
                (colors[0][channel] + 2 * colors[1][channel]) / 3);
        }
    } else {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            colors[2][channel] = static_cast<std::uint8_t>(
                (colors[0][channel] + colors[1][channel]) / 2);
        }
        colors[2][3] = 255;
        colors[3] = {0, 0, 0, 0};
    }

    const auto indices = read_u32(block + 4, 0);
    for (std::uint32_t row = 0; row < 4; ++row) {
        for (std::uint32_t column = 0; column < 4; ++column) {
            const auto index = (indices >> (2 * (row * 4 + column))) & 0x3;
            write_pixel(texture, block_x + column, block_y + row, colors[index]);
        }
    }
}

void decode_dxt5_block(const std::uint8_t* block, Img& texture,
                       std::uint32_t block_x, std::uint32_t block_y) {
    std::array<std::uint8_t, 8> alpha_values{};
    alpha_values[0] = block[0];
    alpha_values[1] = block[1];
    if (alpha_values[0] > alpha_values[1]) {
        for (std::size_t index = 2; index < 8; ++index) {
            alpha_values[index] = static_cast<std::uint8_t>(
                ((8 - index) * alpha_values[0] + (index - 1) * alpha_values[1]) / 7);
        }
    } else {
        for (std::size_t index = 2; index < 6; ++index) {
            alpha_values[index] = static_cast<std::uint8_t>(
                ((6 - index) * alpha_values[0] + (index - 1) * alpha_values[1]) / 5);
        }
        alpha_values[6] = 0;
        alpha_values[7] = 255;
    }

    std::uint64_t alpha_indices = 0;
    for (std::size_t index = 0; index < 6; ++index) {
        alpha_indices |= static_cast<std::uint64_t>(block[2 + index]) << (8 * index);
    }
    const auto color0 = read_u16(block + 8);
    const auto color1 = read_u16(block + 10);
    std::array<std::array<std::uint8_t, 4>, 4> colors{
        decode_rgb565(color0), decode_rgb565(color1), {}, {}};
    for (std::size_t channel = 0; channel < 4; ++channel) {
        colors[2][channel] = static_cast<std::uint8_t>(
            (2 * colors[0][channel] + colors[1][channel]) / 3);
        colors[3][channel] = static_cast<std::uint8_t>(
            (colors[0][channel] + 2 * colors[1][channel]) / 3);
    }
    const auto color_indices = read_u32(block + 12, 0);
    for (std::uint32_t row = 0; row < 4; ++row) {
        for (std::uint32_t column = 0; column < 4; ++column) {
            const auto pixel = row * 4 + column;
            auto color = colors[(color_indices >> (2 * pixel)) & 0x3];
            color[3] = alpha_values[(alpha_indices >> (3 * pixel)) & 0x7];
            write_pixel(texture, block_x + column, block_y + row, color);
        }
    }
}

bool decode_dds_rgba8(const std::vector<std::uint8_t>& bytes_v, Img& texture) {
    const std::uint8_t* bytes = bytes_v.data();
    if (bytes_v.size() < 128 || std::memcmp(bytes, "DDS ", 4) != 0) return false;
    const auto header_size = read_u32(bytes, 4);
    const auto width = read_u32(bytes, 16);
    const auto height = read_u32(bytes, 12);
    const auto pixel_format_size = read_u32(bytes, 76);
    if (header_size != 124 || pixel_format_size != 32 || width == 0 || height == 0) return false;
    const auto pf_flags = read_u32(bytes, 80);
    const auto rgb_bit_count = read_u32(bytes, 88);
    if ((pf_flags & 0x4u) == 0 && rgb_bit_count == 32) {
        const auto required = 128ull + static_cast<std::uint64_t>(width) * height * 4;
        if (required > bytes_v.size()) return false;
        texture.w = (int)width;
        texture.h = (int)height;
        texture.rgba.assign(bytes + 128, bytes + 128 +
                            static_cast<std::size_t>(width) * height * 4);
        return true;
    }

    const std::string four_cc(reinterpret_cast<const char*>(bytes + 84), 4);
    std::size_t block_bytes = 0;
    enum class Format { Dxt1, Dxt5 } format;
    if (four_cc == "DXT1") {
        format = Format::Dxt1;
        block_bytes = 8;
    } else if (four_cc == "DXT5") {
        format = Format::Dxt5;
        block_bytes = 16;
    } else {
        return false;
    }

    const auto blocks_x = (width + 3) / 4;
    const auto blocks_y = (height + 3) / 4;
    const auto required_size = 128ull + static_cast<std::uint64_t>(blocks_x) * blocks_y * block_bytes;
    if (required_size > bytes_v.size()) return false;

    Img decoded;
    decoded.w = (int)width;
    decoded.h = (int)height;
    decoded.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    const auto* payload = bytes + 128;
    for (std::uint32_t block_y = 0; block_y < blocks_y; ++block_y) {
        for (std::uint32_t block_x = 0; block_x < blocks_x; ++block_x) {
            const auto* block = payload +
                (static_cast<std::size_t>(block_y) * blocks_x + block_x) * block_bytes;
            if (format == Format::Dxt1) {
                decode_color_block(block, decoded, block_x * 4, block_y * 4, true);
            } else {
                decode_dxt5_block(block, decoded, block_x * 4, block_y * 4);
            }
        }
    }
    texture = std::move(decoded);
    return true;
}

bool load_dds(const std::filesystem::path& path, Img& out) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const auto size = input.tellg();
    if (size <= 0) return false;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input) return false;
    return decode_dds_rgba8(bytes, out);
}

// ============================================================================
// DDS write (ported verbatim from Fable2Native/tools/cook_lh_tex.cpp
// write_dds_rgba8, plus its write_le32 helper).
// ============================================================================
void write_le32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

// Write an UNCOMPRESSED 32-bpp RGBA8 DDS (no FourCC; DDPF_RGB|DDPF_ALPHAPIXELS).
bool write_dds_rgba8(const std::filesystem::path& output, std::uint32_t width,
                     std::uint32_t height, const std::vector<std::uint8_t>& rgba) {
    std::vector<std::uint8_t> dds(128 + rgba.size(), 0);
    dds[0] = 'D'; dds[1] = 'D'; dds[2] = 'S'; dds[3] = ' ';
    write_le32(dds, 4, 124);
    write_le32(dds, 8, 0x0000100F);  // CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT
    write_le32(dds, 12, height);
    write_le32(dds, 16, width);
    write_le32(dds, 20, width * 4);  // pitch = w*4 (uncompressed)
    write_le32(dds, 28, 1);
    write_le32(dds, 76, 32);
    write_le32(dds, 80, 0x00000041);  // DDPF_RGB | DDPF_ALPHAPIXELS
    write_le32(dds, 84, 0);           // no FourCC
    write_le32(dds, 88, 32);          // RGBBitCount
    write_le32(dds, 92, 0x000000FF);  // R mask
    write_le32(dds, 96, 0x0000FF00);  // G mask
    write_le32(dds, 100, 0x00FF0000); // B mask
    write_le32(dds, 104, 0xFF000000); // A mask
    write_le32(dds, 108, 0x00001000); // DDSCAPS_TEXTURE
    std::copy(rgba.begin(), rgba.end(), dds.begin() + 128);

    std::ofstream stream(output, std::ios::binary);
    if (!stream) return false;
    stream.write(reinterpret_cast<const char*>(dds.data()),
                 static_cast<std::streamsize>(dds.size()));
    return static_cast<bool>(stream);
}

}  // namespace

using namespace Level;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: terrain_splat_bake <in.ehf> <out.dds> --res N "
            "--lod <i>=<path.dds> [--lod ...]\n");
        return 1;
    }
    const std::string ehf_path = argv[1];
    const std::string out_path = argv[2];
    int res = 2048;
    std::vector<std::pair<int, std::string>> lod_args;  // (index, dds path)

    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--res" && i + 1 < argc) {
            res = std::atoi(argv[++i]);
        } else if (a == "--lod" && i + 1 < argc) {
            std::string kv = argv[++i];
            auto eq = kv.find('=');
            if (eq == std::string::npos) {
                std::fprintf(stderr, "bad --lod arg (expected i=path): %s\n", kv.c_str());
                return 1;
            }
            int idx = std::atoi(kv.substr(0, eq).c_str());
            lod_args.emplace_back(idx, kv.substr(eq + 1));
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 1;
        }
    }
    if (res <= 0) res = 2048;

    // ---- read + parse the ehf ---------------------------------------------
    std::vector<uint8_t> ehf;
    {
        std::ifstream f(ehf_path, std::ios::binary | std::ios::ate);
        if (!f) { std::fprintf(stderr, "FAILED cannot open ehf: %s\n", ehf_path.c_str()); return 1; }
        auto sz = f.tellg();
        ehf.resize((size_t)sz);
        f.seekg(0);
        f.read(reinterpret_cast<char*>(ehf.data()), sz);
    }
    EhfParsedBody parsed;
    if (!ParseEhfBody(ehf, parsed) || !parsed.ok) {
        std::fprintf(stderr, "FAILED ParseEhfBody: %s\n", parsed.error.c_str());
        return 1;
    }

    // Terrain AO/lightmap from the .ehf body atlas (pixel_format 24). The game/AB multiplies the
    // ground albedo by ao*0.55+0.45 (ModelPreview g_terrain_ps :1363-1365) to add the crevice /
    // large-scale baked shading the flat splat composite lacks — the "muddy/flat ground" fix.
    // Same decode + header offset the AB uses (LevelLoader ~10929; body offset at ehf+0x37 BE).
    std::vector<uint8_t> lm_rgba; int lm_w = 0, lm_h = 0; bool ao_ok = false;
    if (ehf.size() > 0x3b) {
        const uint32_t body_off = (uint32_t(ehf[0x37]) << 24) | (uint32_t(ehf[0x38]) << 16)
                                | (uint32_t(ehf[0x39]) << 8) | uint32_t(ehf[0x3a]);
        if (body_off < ehf.size()) {
            std::vector<uint8_t> body_slice(ehf.begin() + body_off, ehf.end());
            auto dec = TextureAtlas::DecodeAtlas(body_slice);
            if (dec.ok && dec.pixel_format == 24u && dec.width > 0 && dec.height > 0) {
                lm_rgba = std::move(dec.rgba); lm_w = dec.width; lm_h = dec.height; ao_ok = true;
                std::printf("terrain AO atlas: %dx%d (pf24)\n", lm_w, lm_h);
            } else {
                std::fprintf(stderr, "warn: terrain AO skipped (%s)\n", dec.error.c_str());
            }
        }
    }

    // ---- build mats[] (one per parsed.lods) from --lod DDS ----------------
    struct Mat {
        bool                 decoded = false;
        std::vector<uint8_t> rgba;
        int                  w = 0, h = 0;
        std::string          name;
        float                tile_scale = 0.125f;
    };
    std::vector<Mat> mats(parsed.lods.size());
    int first_decoded = -1;
    for (const auto& [idx, path] : lod_args) {
        if (idx < 0 || idx >= (int)mats.size()) continue;
        Img img;
        if (!load_dds(path, img)) {
            std::fprintf(stderr, "warn: LOD %d decode failed: %s\n", idx, path.c_str());
            continue;
        }
        mats[idx].decoded    = true;
        mats[idx].rgba       = std::move(img.rgba);
        mats[idx].w          = img.w;
        mats[idx].h          = img.h;
        mats[idx].name       = path;
        float ts = parsed.lods[idx].params[0][0];
        mats[idx].tile_scale = (ts > 0.f && ts < 1.f) ? ts : 0.125f;
        if (first_decoded < 0) first_decoded = idx;
    }

    if (first_decoded < 0) {
        std::printf("FAILED no LOD decoded\n");
        return 1;
    }

    // ---- output buffer + world bounds / chunk grid (LevelLoader ~11036-11065)
    const int out_w = res;
    const int out_h = res;
    std::vector<uint8_t> out_rgba(size_t(out_w) * size_t(out_h) * 4, 0);

    float world_min_x =  1e30f;
    float world_min_z =  1e30f;
    float world_max_x = -1e30f;
    float world_max_z = -1e30f;
    for (const auto& c : parsed.chunks) {
        world_min_x = std::min(world_min_x, c.origin[0]);
        world_min_z = std::min(world_min_z, c.origin[1]);
        world_max_x = std::max(world_max_x, c.extent[0]);
        world_max_z = std::max(world_max_z, c.extent[1]);
    }
    const float world_span_x = std::max(1e-6f, world_max_x - world_min_x);
    const float world_span_z = std::max(1e-6f, world_max_z - world_min_z);
    const float chunk_size_x = world_span_x / std::max(1u, parsed.chunk_w);
    const float chunk_size_z = world_span_z / std::max(1u, parsed.chunk_h);
    std::vector<const EhfChunk*> chunk_grid(
        size_t(parsed.chunk_w) * size_t(parsed.chunk_h), nullptr);
    for (const auto& c : parsed.chunks) {
        const int cx = std::clamp(
            int(std::lround((c.origin[0] - world_min_x) / chunk_size_x)),
            0, int(parsed.chunk_w) - 1);
        const int cy = std::clamp(
            int(std::lround((c.origin[1] - world_min_z) / chunk_size_z)),
            0, int(parsed.chunk_h) - 1);
        chunk_grid[size_t(cy) * size_t(parsed.chunk_w) + size_t(cx)] = &c;
    }

    // ---- sample_mat lambda (LevelLoader ~11075-11107) ---------------------
    auto sample_mat = [&](int idx, float u_world, float v_world,
                          uint8_t out_rgb[3])
    {
        const Mat& m = (idx >= 0 && idx < (int)mats.size() && mats[idx].decoded)
            ? mats[idx] : mats[first_decoded];
        const float ts = (m.tile_scale > 0.f && m.tile_scale < 1.f)
                            ? m.tile_scale : 0.125f;
        float u = (u_world * ts);
        float v = (v_world * ts);
        u = u - std::floor(u);
        v = v - std::floor(v);
        const float fx = u * m.w;
        const float fy = v * m.h;
        const int x0 = int(fx);
        const int y0 = int(fy);
        const int x1 = (x0 + 1) % m.w;
        const int y1 = (y0 + 1) % m.h;
        const float dx = fx - float(x0);
        const float dy = fy - float(y0);
        const uint8_t* p00 = m.rgba.data() + (size_t(y0) * m.w + x0) * 4;
        const uint8_t* p10 = m.rgba.data() + (size_t(y0) * m.w + x1) * 4;
        const uint8_t* p01 = m.rgba.data() + (size_t(y1) * m.w + x0) * 4;
        const uint8_t* p11 = m.rgba.data() + (size_t(y1) * m.w + x1) * 4;
        const float w00b = (1.f - dx) * (1.f - dy);
        const float w10b =        dx  * (1.f - dy);
        const float w01b = (1.f - dx) *        dy;
        const float w11b =        dx  *        dy;
        for (int c = 0; c < 3; ++c) {
            out_rgb[c] = uint8_t(
                w00b * p00[c] + w10b * p10[c] +
                w01b * p01[c] + w11b * p11[c]);
        }
    };

    // ---- sample_mask lambda (LevelLoader ~11109-11156) --------------------
    auto sample_mask = [&](const EhfChunkLayer& L,
                           float local_x, float local_z) -> float
    {
        if (parsed.splat_indices.empty() ||
            parsed.splat_w == 0 || parsed.splat_h == 0 ||
            parsed.splat_indices.size() !=
                size_t(parsed.splat_w) * size_t(parsed.splat_h))
        {
            return 1.0f;
        }

        const float scale_u = (L.mask_scale[0] > 0.0f)
            ? L.mask_scale[0]
            : 32.0f / float(parsed.splat_w);
        const float scale_v = (L.mask_scale[1] > 0.0f)
            ? L.mask_scale[1]
            : 32.0f / float(parsed.splat_h);

        const float u = L.tile_uv[0]
            + std::clamp(local_x, 0.0f, 1.0f)
            * scale_u * 2.0f;
        const float v = L.tile_uv[1]
            + std::clamp(local_z, 0.0f, 1.0f)
            * scale_v * 2.0f;

        float px = u * float(parsed.splat_w) - 0.5f;
        float py = v * float(parsed.splat_h) - 0.5f;
        px = std::clamp(px, 0.0f, float(parsed.splat_w - 1));
        py = std::clamp(py, 0.0f, float(parsed.splat_h - 1));

        const int x0 = int(px);
        const int y0 = int(py);
        const int x1 = std::min<int>(x0 + 1, int(parsed.splat_w) - 1);
        const int y1 = std::min<int>(y0 + 1, int(parsed.splat_h) - 1);
        const float dx = px - float(x0);
        const float dy = py - float(y0);
        auto at = [&](int x, int y) -> float {
            return parsed.splat_indices[
                size_t(y) * size_t(parsed.splat_w) + size_t(x)] / 255.0f;
        };
        const float w00m = (1.0f - dx) * (1.0f - dy);
        const float w10m =         dx  * (1.0f - dy);
        const float w01m = (1.0f - dx) *         dy;
        const float w11m =         dx  *         dy;
        return std::clamp(at(x0, y0) * w00m + at(x1, y0) * w10m
                        + at(x0, y1) * w01m + at(x1, y1) * w11m,
                          0.0f, 1.0f);
    };

    constexpr float kBlendMax     = 3.0f;

    // ---- compositing loop (LevelLoader ~11160-11285) ----------------------
    for (int y = 0; y < out_h; ++y) {
        const float v_norm = (out_h > 1)
            ? float(y) / float(out_h - 1)
            : 0.0f;
        const float world_z = world_min_z + v_norm * world_span_z;
        const float fy_chunk = (world_z - world_min_z) / chunk_size_z;
        const int   cy       = std::min<int>(parsed.chunk_h - 1, int(fy_chunk));
        const float fy_in    = std::clamp(fy_chunk - float(cy), 0.f, 1.f);
        for (int x = 0; x < out_w; ++x) {
            const float u_norm = (out_w > 1)
                ? float(x) / float(out_w - 1)
                : 0.0f;
            const float world_x = world_min_x + u_norm * world_span_x;
            const float fx_chunk = (world_x - world_min_x) / chunk_size_x;
            const int   cx       = std::min<int>(parsed.chunk_w - 1, int(fx_chunk));
            const float fx_in    = std::clamp(fx_chunk - float(cx), 0.f, 1.f);

            const float w00 = (1.f - fx_in) * (1.f - fy_in);
            const float w10 =        fx_in  * (1.f - fy_in);
            const float w01 = (1.f - fx_in) *        fy_in;
            const float w11 =        fx_in  *        fy_in;

            const size_t chunk_index =
                size_t(cy) * size_t(parsed.chunk_w) + size_t(cx);
            const EhfChunk& chunk =
                (chunk_index < chunk_grid.size() && chunk_grid[chunk_index])
                    ? *chunk_grid[chunk_index]
                    : parsed.chunks.front();

            float accum_r = 0.f, accum_g = 0.f, accum_b = 0.f;
            float accum_a = 0.f;
            uint8_t first_rgb[3] = {0, 0, 0};
            bool have_first_rgb = false;

            const float wu = world_x;
            const float wv = world_z;

            for (const auto& L : chunk.layers) {
                auto corner_material = [&](int corner) -> int {
                    const uint32_t layer_idx = L.texture_idx[corner];
                    uint32_t mat_idx = L.material_idx;
                    if (layer_idx < chunk.layers.size()) {
                        mat_idx = chunk.layers[size_t(layer_idx)].material_idx;
                    }
                    if (mat_idx < mats.size()) return int(mat_idx);
                    if (L.material_idx < mats.size()) return int(L.material_idx);
                    return -1;
                };
                const int material_ids[4] = {
                    corner_material(0), corner_material(1),
                    corner_material(2), corner_material(3),
                };
                const float corner_alpha[4] = {
                    w00 * float(L.blend[0]) / kBlendMax,
                    w10 * float(L.blend[1]) / kBlendMax,
                    w01 * float(L.blend[2]) / kBlendMax,
                    w11 * float(L.blend[3]) / kBlendMax,
                };
                const float blend_px = corner_alpha[0] + corner_alpha[1] +
                                       corner_alpha[2] + corner_alpha[3];
                if (blend_px <= 1e-6f) continue;

                float rgb_f[3] = {0.0f, 0.0f, 0.0f};
                bool have_corner_rgb = false;
                for (int ci = 0; ci < 4; ++ci) {
                    if (material_ids[ci] < 0 || corner_alpha[ci] <= 0.0f) {
                        continue;
                    }
                    uint8_t corner_rgb[3];
                    sample_mat(material_ids[ci], wu, wv, corner_rgb);
                    const float w = corner_alpha[ci] / blend_px;
                    rgb_f[0] += float(corner_rgb[0]) * w;
                    rgb_f[1] += float(corner_rgb[1]) * w;
                    rgb_f[2] += float(corner_rgb[2]) * w;
                    have_corner_rgb = true;
                }
                if (!have_corner_rgb) continue;
                uint8_t rgb[3];
                rgb[0] = uint8_t(std::clamp(int(std::round(rgb_f[0])), 0, 255));
                rgb[1] = uint8_t(std::clamp(int(std::round(rgb_f[1])), 0, 255));
                rgb[2] = uint8_t(std::clamp(int(std::round(rgb_f[2])), 0, 255));
                if (!have_first_rgb) {
                    first_rgb[0] = rgb[0];
                    first_rgb[1] = rgb[1];
                    first_rgb[2] = rgb[2];
                    have_first_rgb = true;
                }

                const float alpha = std::clamp(blend_px, 0.f, 1.f)
                                  * sample_mask(L, fx_in, fy_in);
                if (alpha < 1.f / 255.f) continue;

                const float keep = 1.0f - alpha;
                accum_r = accum_r * keep + float(rgb[0]) * alpha;
                accum_g = accum_g * keep + float(rgb[1]) * alpha;
                accum_b = accum_b * keep + float(rgb[2]) * alpha;
                accum_a = accum_a * keep + alpha;
            }

            if (accum_a < 0.999f && have_first_rgb) {
                const float fill = 1.0f - accum_a;
                accum_r += float(first_rgb[0]) * fill;
                accum_g += float(first_rgb[1]) * fill;
                accum_b += float(first_rgb[2]) * fill;
                accum_a = 1.0f;
            }

            if (accum_a > 1e-4f && accum_a < 0.999f) {
                accum_r /= accum_a;
                accum_g /= accum_a;
                accum_b /= accum_a;
            } else {
                uint8_t base[3];
                sample_mat(first_decoded, wu, wv, base);
                if (!have_first_rgb) {
                    accum_r = base[0]; accum_g = base[1]; accum_b = base[2];
                }
            }

            // Bake the terrain AO in (ao*0.55+0.45), sampled across the whole terrain by the
            // normalized output UV — matches the AB's lm_uv = chunk_co/(CW,CH).
            if (ao_ok) {
                const int ax = std::clamp(int(u_norm * float(lm_w)), 0, lm_w - 1);
                const int ay = std::clamp(int(v_norm * float(lm_h)), 0, lm_h - 1);
                const float ao = float(lm_rgba[(size_t(ay) * size_t(lm_w) + size_t(ax)) * 4]) / 255.0f;
                const float k = ao * 0.55f + 0.45f;
                accum_r *= k; accum_g *= k; accum_b *= k;
            }
            uint8_t* dst = out_rgba.data() + (size_t(y) * out_w + x) * 4;
            dst[0] = uint8_t(std::clamp(accum_r, 0.f, 255.f));
            dst[1] = uint8_t(std::clamp(accum_g, 0.f, 255.f));
            dst[2] = uint8_t(std::clamp(accum_b, 0.f, 255.f));
            dst[3] = 0xFF;
        }
    }

    if (!write_dds_rgba8(out_path, (uint32_t)out_w, (uint32_t)out_h, out_rgba)) {
        std::fprintf(stderr, "FAILED writing DDS: %s\n", out_path.c_str());
        return 1;
    }

    std::printf("BOUNDS minx=%.6f minz=%.6f spanx=%.6f spanz=%.6f outw=%d outh=%d\n",
                world_min_x, world_min_z, world_span_x, world_span_z, out_w, out_h);
    return 0;
}
