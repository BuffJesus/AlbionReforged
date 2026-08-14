#include "../../../Fable2AssetBrowser/source/src/textures/LhTexCodec.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

uint16_t read_be16(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[offset]) << 8) |
                                 static_cast<uint16_t>(bytes[offset + 1]));
}

void write_ppm(const std::string& path, const std::vector<uint8_t>& bc1, int width,
               int height) {
    const int blocks_w = (width + 3) / 4;
    const int blocks_h = (height + 3) / 4;
    std::ofstream output(path, std::ios::binary);
    output << "P6\n" << width << ' ' << height << "\n255\n";

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto* block = bc1.data() +
                                (static_cast<size_t>(y / 4) * blocks_w + x / 4) * 8;
            const uint16_t c0 = static_cast<uint16_t>(block[0] | (block[1] << 8));
            const uint16_t c1 = static_cast<uint16_t>(block[2] | (block[3] << 8));
            uint8_t colors[4][3]{};
            auto decode565 = [](uint16_t c, uint8_t* out) {
                out[0] = static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
                out[1] = static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
                out[2] = static_cast<uint8_t>((c & 0x1F) * 255 / 31);
            };
            decode565(c0, colors[0]);
            decode565(c1, colors[1]);
            if (c0 > c1) {
                for (int channel = 0; channel < 3; ++channel) {
                    colors[2][channel] = static_cast<uint8_t>((2 * colors[0][channel] +
                                                                 colors[1][channel]) /
                                                                3);
                    colors[3][channel] = static_cast<uint8_t>((colors[0][channel] +
                                                                 2 * colors[1][channel]) /
                                                                3);
                }
            } else {
                for (int channel = 0; channel < 3; ++channel) {
                    colors[2][channel] = static_cast<uint8_t>((colors[0][channel] +
                                                                 colors[1][channel]) /
                                                                2);
                    colors[3][channel] = 0;
                }
            }
            const int local = (y & 3) * 4 + (x & 3);
            const uint32_t indices = static_cast<uint32_t>(block[4]) |
                                     (static_cast<uint32_t>(block[5]) << 8) |
                                     (static_cast<uint32_t>(block[6]) << 16) |
                                     (static_cast<uint32_t>(block[7]) << 24);
            const int index = static_cast<int>((indices >> (2 * local)) & 3);
            output.write(reinterpret_cast<const char*>(colors[index]), 3);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: decode_large_ring <header> <body> <ppm>\n";
        return 2;
    }
    std::ifstream header_file(argv[1], std::ios::binary);
    std::ifstream body_file(argv[2], std::ios::binary);
    std::vector<uint8_t> header((std::istreambuf_iterator<char>(header_file)), {});
    std::vector<uint8_t> body((std::istreambuf_iterator<char>(body_file)), {});
    if (header.size() < 0x1C || body.size() < 48) {
        std::cerr << "short texture data\n";
        return 1;
    }
    const int width = read_be16(header, 0x14);
    const int height = read_be16(header, 0x18);
    const uint32_t body_size = (static_cast<uint32_t>(body[8]) << 24) |
                               (static_cast<uint32_t>(body[9]) << 16) |
                               (static_cast<uint32_t>(body[10]) << 8) |
                               static_cast<uint32_t>(body[11]);
    int decoded_width = 0;
    int decoded_height = 0;
    std::vector<uint8_t> bc1;
    std::string error;
    if (!lh_decode_compressed_mip(body.data() + 48,
                                  std::min<size_t>(body_size, body.size() - 48),
                                  decoded_width, decoded_height, bc1, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cerr << "header=" << width << 'x' << height << " decoded=" << decoded_width
              << 'x' << decoded_height << "\n";
    write_ppm(argv[3], bc1, decoded_width, decoded_height);
    return 0;
}
