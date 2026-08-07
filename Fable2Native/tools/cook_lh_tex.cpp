// LhTex .tex -> DDS cooker (built as f2native_cook_lh_tex).
//
//   f2native_cook_lh_tex <input.tex> <output.dds> [--pf N --width W --height H]
//
// Handles the two frontend texture families found in gui_textures.bnk:
//   * body mip0 comp 1/11  -> custom-compressed DXT1  (LhTexCodec; dims from the body)
//   * body mip0 comp 7     -> Xbox-360 TILED BCn      (needs --pf/--width/--height from the
//                             84-byte gui_texture_headers.bnk header): pf35 -> DXT1, pf39 -> DXT5.
// The Xbox tiling untile + BCn endian-swap are ported (unmodified logic) from the AssetBrowser
// decoder (Fable2AssetBrowser/source/src/UI/ModelPreview.cpp). Output DXT1/DXT5 loads directly in
// the runtime (native_texture.cpp decode_dds_rgba8 supports DXT1 + DXT5).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "LhTexCodec.h"

namespace {

std::uint32_t read_be32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

void write_le32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

// Write a block-compressed DDS. four_cc is "DXT1" (8-byte blocks) or "DXT5" (16-byte blocks).
bool write_dds(const std::filesystem::path& output, std::uint32_t width, std::uint32_t height,
               const std::vector<std::uint8_t>& blocks, const char four_cc[4]) {
    std::vector<std::uint8_t> dds(128 + blocks.size(), 0);
    dds[0] = 'D'; dds[1] = 'D'; dds[2] = 'S'; dds[3] = ' ';
    write_le32(dds, 4, 124);
    write_le32(dds, 8, 0x00081007); // CAPS | HEIGHT | WIDTH | PIXELFORMAT | LINEARSIZE
    write_le32(dds, 12, height);
    write_le32(dds, 16, width);
    write_le32(dds, 20, static_cast<std::uint32_t>(blocks.size()));
    write_le32(dds, 28, 1);
    write_le32(dds, 76, 32);
    write_le32(dds, 80, 0x00000004); // DDPF_FOURCC
    dds[84] = four_cc[0]; dds[85] = four_cc[1]; dds[86] = four_cc[2]; dds[87] = four_cc[3];
    write_le32(dds, 108, 0x00001000); // DDSCAPS_TEXTURE
    std::copy(blocks.begin(), blocks.end(), dds.begin() + 128);

    std::ofstream stream(output, std::ios::binary);
    if (!stream) return false;
    stream.write(reinterpret_cast<const char*>(dds.data()),
                 static_cast<std::streamsize>(dds.size()));
    return static_cast<bool>(stream);
}

// ---- Xbox-360 tiling (ported verbatim from AssetBrowser ModelPreview.cpp) --------------------
std::uint32_t xbox360_tiled_offset(std::uint32_t x, std::uint32_t y,
                                   std::uint32_t width_in_blocks, std::uint32_t texel_byte_size) {
    std::uint32_t aligned_w = (width_in_blocks + 31u) & ~31u;
    std::uint32_t log_bpp = (texel_byte_size >> 2) +
                            ((texel_byte_size >> 1) >> (texel_byte_size >> 2));
    std::uint32_t macro = ((x >> 5) + (y >> 5) * (aligned_w >> 5)) << (log_bpp + 7);
    std::uint32_t micro = (((x & 7) + ((y & 6) << 2)) << log_bpp);
    std::uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15u) +
                           ((y & 8u) << (3 + log_bpp)) + ((y & 16u) << 7);
    std::uint32_t final_off =
        ((offset & ~511u) << 3) + ((offset & 448u) << 2) + (offset & 63u) +
        ((y & 1u) << 4) + (((x & 7u) << 1) ^ ((y >> 1) & 1u));
    return final_off & ~((1u << log_bpp) - 1u);
}

std::uint32_t xg_address_2d_tiled_x(std::uint32_t block_offset, std::uint32_t width_in_blocks,
                                    std::uint32_t texel_byte_pitch) {
    std::uint32_t aligned_width = (width_in_blocks + 31u) & ~31u;
    std::uint32_t log_bpp = (texel_byte_pitch >> 2) +
                            ((texel_byte_pitch >> 1) >> (texel_byte_pitch >> 2));
    std::uint32_t offset_byte = block_offset << log_bpp;
    std::uint32_t offset_tile = ((offset_byte & ~0xFFFu) >> 3) +
                                ((offset_byte & 0x700u) >> 2) + (offset_byte & 0x3Fu);
    std::uint32_t offset_macro = offset_tile >> (7 + log_bpp);
    std::uint32_t macro_x = (offset_macro % (aligned_width >> 5)) << 2;
    std::uint32_t tile = (((offset_tile >> (5 + log_bpp)) & 2) + (offset_byte >> 6)) & 3;
    std::uint32_t macro = (macro_x + tile) << 3;
    std::uint32_t micro = ((((offset_tile >> 1) & ~0xFu) + (offset_tile & 0xFu)) &
                           ((texel_byte_pitch << 3) - 1)) >> log_bpp;
    return macro + micro;
}

std::uint32_t xg_address_2d_tiled_y(std::uint32_t block_offset, std::uint32_t width_in_blocks,
                                    std::uint32_t texel_byte_pitch) {
    std::uint32_t aligned_width = (width_in_blocks + 31u) & ~31u;
    std::uint32_t log_bpp = (texel_byte_pitch >> 2) +
                            ((texel_byte_pitch >> 1) >> (texel_byte_pitch >> 2));
    std::uint32_t offset_byte = block_offset << log_bpp;
    std::uint32_t offset_tile = ((offset_byte & ~0xFFFu) >> 3) +
                                ((offset_byte & 0x700u) >> 2) + (offset_byte & 0x3Fu);
    std::uint32_t offset_macro = offset_tile >> (7 + log_bpp);
    std::uint32_t macro_y = (offset_macro / (aligned_width >> 5)) << 2;
    std::uint32_t tile = ((offset_tile >> (6 + log_bpp)) & 1) + ((offset_byte & 0x800u) >> 10);
    std::uint32_t macro = (macro_y + tile) << 3;
    std::uint32_t micro = (((offset_tile & (((texel_byte_pitch << 6) - 1) & ~0x1Fu)) +
                            ((offset_tile & 0xFu) << 1)) >> (3 + log_bpp)) & ~1u;
    return macro + micro + ((offset_tile & 0x10u) >> 4);
}

void untile_xbox360_bc(const std::uint8_t* tiled, std::size_t tiled_size,
                       std::vector<std::uint8_t>& linear_out, int width, int height,
                       std::uint32_t block_size) {
    const int blocks_w = (width + 3) / 4;
    const int blocks_h = (height + 3) / 4;
    linear_out.assign(static_cast<std::size_t>(blocks_w) * blocks_h * block_size, 0);

    bool use_xg_scatter = false;
    {
        const std::size_t total_blocks = static_cast<std::size_t>(blocks_w) * blocks_h;
        std::size_t oob = 0, dup = 0;
        std::vector<std::uint8_t> seen(tiled_size / block_size + 1, 0);
        for (int by = 0; by < blocks_h; ++by)
            for (int bx = 0; bx < blocks_w; ++bx) {
                std::uint32_t off = xbox360_tiled_offset(bx, by, blocks_w, block_size);
                if (off + block_size > tiled_size) { ++oob; continue; }
                const std::size_t idx = off / block_size;
                if (idx < seen.size()) { if (seen[idx]) ++dup; seen[idx] = 1; }
            }
        if (oob * 20 > total_blocks || dup * 20 > total_blocks) use_xg_scatter = true;
    }

    if (use_xg_scatter) {
        const std::size_t total_blocks = static_cast<std::size_t>(blocks_w) * blocks_h;
        for (std::size_t i = 0; i < total_blocks; ++i) {
            const std::size_t src_off = i * block_size;
            if (src_off + block_size > tiled_size) continue;
            std::uint32_t dx = xg_address_2d_tiled_x(i, blocks_w, block_size);
            std::uint32_t dy = xg_address_2d_tiled_y(i, blocks_w, block_size);
            if (dx >= static_cast<std::uint32_t>(blocks_w) ||
                dy >= static_cast<std::uint32_t>(blocks_h)) continue;
            std::memcpy(linear_out.data() + (static_cast<std::size_t>(dy) * blocks_w + dx) * block_size,
                        tiled + src_off, block_size);
        }
        return;
    }
    for (int by = 0; by < blocks_h; ++by)
        for (int bx = 0; bx < blocks_w; ++bx) {
            std::uint32_t src_off = xbox360_tiled_offset(bx, by, blocks_w, block_size);
            if (src_off + block_size > tiled_size) continue;
            std::memcpy(linear_out.data() + (static_cast<std::size_t>(by) * blocks_w + bx) * block_size,
                        tiled + src_off, block_size);
        }
}

void swap_bc_endian(std::vector<std::uint8_t>& data) {  // 16-bit word byte-swap (BC1 & BC3)
    for (std::size_t i = 0; i + 2 <= data.size(); i += 2)
        std::swap(data[i], data[i + 1]);
}

int fail(const std::string& message) {
    fprintf(stderr, "%s\n", message.c_str());
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    std::string input, output;
    int pf = -1, arg_w = 0, arg_h = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--pf") pf = std::atoi(next());
        else if (a == "--width") arg_w = std::atoi(next());
        else if (a == "--height") arg_h = std::atoi(next());
        else if (input.empty()) input = a;
        else output = a;
    }
    if (input.empty() || output.empty()) {
        fprintf(stderr, "usage: cook_lh_tex input.tex output.dds [--pf N --width W --height H]\n");
        return 2;
    }

    std::ifstream in(input, std::ios::binary | std::ios::ate);
    if (!in) return fail("unable to open input: " + input);
    const auto size = in.tellg();
    if (size <= 48) return fail("input too small");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), size);

    const auto comp = read_be32(bytes, 0);
    const auto data_off = read_be32(bytes, 4);
    const auto data_size = read_be32(bytes, 8);
    if (static_cast<std::uint64_t>(data_off) + data_size > bytes.size())
        return fail("mip data out of bounds");

    // comp 1 / 11 : custom-compressed DXT1, dims recovered by the codec from the body.
    if (comp == 1 || comp == 11) {
        int width = 0, height = 0;
        std::vector<std::uint8_t> bc1;
        std::string error;
        if (!lh_decode_compressed_mip(bytes.data() + 48, data_size, width, height, bc1,
                                      &error, comp == 11))
            return fail("decode failed: " + error);
        if (!write_dds(output, width, height, bc1, "DXT1"))
            return fail("unable to write output: " + output);
        printf("%dx%d -> %s\n", width, height, output.c_str());
        return 0;
    }

    // comp 7 : Xbox-360 tiled BCn (pixel format selects the block codec). Needs dims from the header.
    if (comp == 7) {
        if (arg_w <= 0 || arg_h <= 0 || pf < 0)
            return fail("comp=7 needs --pf --width --height (from gui_texture_headers.bnk)");
        std::uint32_t block_size = 0;
        const char* four_cc = nullptr;
        if (pf == 35) { block_size = 8; four_cc = "DXT1"; }       // BC1
        else if (pf == 39) { block_size = 16; four_cc = "DXT5"; } // BC3
        else return fail("comp=7 pf=" + std::to_string(pf) + " unsupported (only pf35 DXT1 / pf39 DXT5)");
        std::vector<std::uint8_t> linear;
        untile_xbox360_bc(bytes.data() + data_off, data_size, linear, arg_w, arg_h, block_size);
        swap_bc_endian(linear);
        if (!write_dds(output, static_cast<std::uint32_t>(arg_w), static_cast<std::uint32_t>(arg_h),
                       linear, four_cc))
            return fail("unable to write output: " + output);
        printf("%dx%d -> %s\n", arg_w, arg_h, output.c_str());
        return 0;
    }

    return fail("unsupported first mip: comp=" + std::to_string(comp));
}
