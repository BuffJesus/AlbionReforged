#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace f2 {

struct NativeTexture {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8;
};

bool decode_dds_rgba8(std::span<const std::uint8_t> bytes,
                      NativeTexture& texture,
                      std::string& error);
bool load_dds_rgba8(const std::filesystem::path& path,
                    NativeTexture& texture,
                    std::string& error);
bool load_image_rgba8(const std::filesystem::path& path,
                      NativeTexture& texture,
                      std::string& error);

}  // namespace f2
