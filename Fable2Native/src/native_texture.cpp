#include "f2/native_texture.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <ranges>

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace f2 {
namespace {

bool fail(std::string& error, std::string message) {
    error = std::move(message);
    return false;
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
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

void write_pixel(NativeTexture& texture, std::uint32_t x, std::uint32_t y,
                 const std::array<std::uint8_t, 4>& color) {
    if (x >= texture.width || y >= texture.height) return;
    const auto offset = (static_cast<std::size_t>(y) * texture.width + x) * 4;
    std::copy(color.begin(), color.end(), texture.rgba8.begin() + offset);
}

void decode_color_block(const std::uint8_t* block, NativeTexture& texture,
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

    const auto indices = read_u32(std::span(block + 4, 4), 0);
    for (std::uint32_t row = 0; row < 4; ++row) {
        for (std::uint32_t column = 0; column < 4; ++column) {
            const auto index = (indices >> (2 * (row * 4 + column))) & 0x3;
            write_pixel(texture, block_x + column, block_y + row, colors[index]);
        }
    }
}

void decode_dxt5_block(const std::uint8_t* block, NativeTexture& texture,
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
    const auto color_indices = read_u32(std::span(block + 12, 4), 0);
    for (std::uint32_t row = 0; row < 4; ++row) {
        for (std::uint32_t column = 0; column < 4; ++column) {
            const auto pixel = row * 4 + column;
            auto color = colors[(color_indices >> (2 * pixel)) & 0x3];
            color[3] = alpha_values[(alpha_indices >> (3 * pixel)) & 0x7];
            write_pixel(texture, block_x + column, block_y + row, color);
        }
    }
}

}  // namespace

bool decode_dds_rgba8(std::span<const std::uint8_t> bytes,
                      NativeTexture& texture,
                      std::string& error) {
    if (bytes.size() < 128 || std::memcmp(bytes.data(), "DDS ", 4) != 0) {
        return fail(error, "not a legacy DDS file");
    }
    const auto header_size = read_u32(bytes, 4);
    const auto width = read_u32(bytes, 16);
    const auto height = read_u32(bytes, 12);
    const auto pixel_format_size = read_u32(bytes, 76);
    if (header_size != 124 || pixel_format_size != 32 || width == 0 || height == 0) {
        return fail(error, "DDS header is invalid");
    }
    const std::string four_cc(reinterpret_cast<const char*>(bytes.data() + 84), 4);
    std::size_t block_bytes = 0;
    enum class Format { Dxt1, Dxt5 } format;
    if (four_cc == "DXT1") {
        format = Format::Dxt1;
        block_bytes = 8;
    } else if (four_cc == "DXT5") {
        format = Format::Dxt5;
        block_bytes = 16;
    } else {
        return fail(error, "DDS format is not supported yet; expected DXT1 or DXT5");
    }

    const auto blocks_x = (width + 3) / 4;
    const auto blocks_y = (height + 3) / 4;
    const auto required_size = 128ull + static_cast<std::uint64_t>(blocks_x) * blocks_y * block_bytes;
    if (required_size > bytes.size()) return fail(error, "DDS payload is truncated");

    NativeTexture decoded;
    decoded.width = width;
    decoded.height = height;
    decoded.rgba8.resize(static_cast<std::size_t>(width) * height * 4);
    const auto* payload = bytes.data() + 128;
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

bool load_dds_rgba8(const std::filesystem::path& path,
                    NativeTexture& texture,
                    std::string& error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return fail(error, "unable to open texture: " + path.string());
    const auto size = input.tellg();
    if (size <= 0) return fail(error, "texture is empty: " + path.string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input) return fail(error, "unable to read texture: " + path.string());
    if (!decode_dds_rgba8(bytes, texture, error)) {
        error = path.string() + ": " + error;
        return false;
    }
    return true;
}

bool load_image_rgba8(const std::filesystem::path& path,
                      NativeTexture& texture,
                      std::string& error) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (extension == ".dds") return load_dds_rgba8(path, texture, error);
#ifdef _WIN32
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        return fail(error, "WIC imaging factory could not be created");
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr,
                                                   GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                                   &decoder))) {
        return fail(error, "unable to open image: " + path.string());
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return fail(error, "image has no frame");
    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) {
        return fail(error, "image dimensions are invalid: " + path.string());
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return fail(error, "image is not convertible to RGBA8: " + path.string());
    }
    NativeTexture decoded;
    decoded.width = width;
    decoded.height = height;
    decoded.rgba8.resize(static_cast<std::size_t>(width) * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4,
                                     static_cast<UINT>(decoded.rgba8.size()),
                                     decoded.rgba8.data()))) {
        return fail(error, "unable to decode image pixels: " + path.string());
    }
    texture = std::move(decoded);
    return true;
#else
    return fail(error, "PNG/BMP loading is only available on Windows: " + path.string());
#endif
}

}  // namespace f2
