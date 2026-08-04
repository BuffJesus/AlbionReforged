#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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

bool write_dds(const std::filesystem::path& output, std::uint32_t width,
              std::uint32_t height, const std::vector<std::uint8_t>& bc1) {
    std::vector<std::uint8_t> dds(128 + bc1.size(), 0);
    dds[0] = 'D'; dds[1] = 'D'; dds[2] = 'S'; dds[3] = ' ';
    write_le32(dds, 4, 124);
    write_le32(dds, 8, 0x00081007); // CAPS | HEIGHT | WIDTH | PIXELFORMAT | LINEARSIZE
    write_le32(dds, 12, height);
    write_le32(dds, 16, width);
    write_le32(dds, 20, static_cast<std::uint32_t>(bc1.size()));
    write_le32(dds, 28, 1);
    write_le32(dds, 76, 32);
    write_le32(dds, 80, 0x00000004); // DDPF_FOURCC
    dds[84] = 'D'; dds[85] = 'X'; dds[86] = 'T'; dds[87] = '1';
    write_le32(dds, 108, 0x00001000); // DDSCAPS_TEXTURE
    std::copy(bc1.begin(), bc1.end(), dds.begin() + 128);

    std::ofstream stream(output, std::ios::binary);
    if (!stream) return false;
    stream.write(reinterpret_cast<const char*>(dds.data()),
                 static_cast<std::streamsize>(dds.size()));
    return static_cast<bool>(stream);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: cook_lh_tex input.tex output.dds\n";
        return 2;
    }

    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    if (!input) {
        std::cerr << "unable to open input: " << argv[1] << '\n';
        return 1;
    }
    const auto size = input.tellg();
    if (size <= 0) {
        std::cerr << "input is empty\n";
        return 1;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input || bytes.size() < 48) {
        std::cerr << "input is truncated\n";
        return 1;
    }

    const auto comp_flag = read_be32(bytes, 0);
    const auto data_size = read_be32(bytes, 8);
    if ((comp_flag != 1 && comp_flag != 11) || data_size < 444 ||
        48ull + data_size > bytes.size()) {
        std::cerr << "unsupported first mip: comp=" << comp_flag
                  << " data_size=" << data_size << '\n';
        return 1;
    }

    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bc1;
    std::string error;
    if (!lh_decode_compressed_mip(bytes.data() + 48, data_size, width, height, bc1,
                                  &error, comp_flag == 11)) {
        std::cerr << "decode failed: " << error << '\n';
        return 1;
    }
    if (!write_dds(argv[2], static_cast<std::uint32_t>(width),
                   static_cast<std::uint32_t>(height), bc1)) {
        std::cerr << "unable to write output: " << argv[2] << '\n';
        return 1;
    }
    std::cout << width << 'x' << height << " -> " << argv[2] << '\n';
    return 0;
}
