#include "albion/pcm_wave.h"
#include <fstream>
#include <iostream>
#include <iterator>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) {
    if (argc < 2) return 2;
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) return 3;
    std::vector<std::uint8_t> wave{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    auto result = albion::data::DecodePcm16Wave(wave, argc > 2 ? std::stoull(argv[2]) : 256ull << 20);
    if (!result) {
        if (!result.value.bytes.empty() || result.value.channels || result.value.sample_rate) return 4;
        std::cerr << result.error.message << '\n';
        return 20 + static_cast<int>(result.error.code);
    }
    wave.clear(); wave.shrink_to_fit(); // decoded clip must own its data
    std::cerr << result.value.channels << ' ' << result.value.sample_rate << ' '
              << result.value.channel_mask << ' ' << result.value.frames() << '\n';
    std::cout.write(reinterpret_cast<const char*>(result.value.bytes.data()),
                    static_cast<std::streamsize>(result.value.bytes.size()));
    return std::cout ? 0 : 5;
}
