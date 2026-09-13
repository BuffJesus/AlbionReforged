#include "albion/tu1_name.h"
#include <iostream>
#include <iterator>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
int main(int argc, char** argv) {
    if (argc != 2) return 2;
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::vector<unsigned char> bytes(std::istreambuf_iterator<char>(std::cin), {});
    if (bytes.size() % 2 || bytes.size() > 2u << 20) return 3;
    std::u16string input;
    for (std::size_t i = 0; i < bytes.size(); i += 2)
        input.push_back(static_cast<char16_t>((bytes[i] << 8) | bytes[i+1]));
    auto result = albion::compat::NormalizeTu1ResourceName(input, std::stoull(argv[1]));
    if (!result) return 20 + static_cast<int>(result.error.code);
    for (auto unit : result.value) {
        std::cout.put(static_cast<char>(unit >> 8));
        std::cout.put(static_cast<char>(unit & 255));
    }
    return 0;
}
