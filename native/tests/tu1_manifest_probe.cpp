#include "albion/tu1_manifest.h"
#include <iostream>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
using namespace albion::data;
int main(int argc, char** argv) {
    if (argc != 6) return 2;
    albion::compat::ManifestLimits limits;
    limits.read_bytes = static_cast<std::uint32_t>(std::stoul(argv[2]));
    limits.bytes = std::stoull(argv[3]);
    limits.records = static_cast<std::uint32_t>(std::stoul(argv[4]));
    limits.line_bytes = static_cast<std::uint32_t>(std::stoul(argv[5]));
    auto source = OpenFileSource(argv[1]);
    if (!source) return 20 + static_cast<int>(source.error.code);
    auto records = albion::compat::ReadTu1Manifest(source.value, limits);
    if (!records) { std::cerr << records.error.message; return 20 + static_cast<int>(records.error.code); }
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    auto word = [](std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) std::cout.put(static_cast<char>(value >> shift));
    };
    for (const auto& record : records.value) {
        word(record.ordinal); word(record.terminated ? 1 : 0);
        word(static_cast<std::uint32_t>(record.raw_name.size()));
        std::cout.write(record.raw_name.data(), static_cast<std::streamsize>(record.raw_name.size()));
    }
}
