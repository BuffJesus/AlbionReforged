#include "albion/archive.h"
#include <future>
#include <iostream>
#include <string>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace albion::data;
int error(const Error& e) {
    std::cerr << static_cast<int>(e.code) << ": " << e.message << '\n';
    return 20 + static_cast<int>(e.code);
}
void word(std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) std::cout.put(static_cast<char>(value >> (i * 8)));
}
void output(const std::vector<std::uint8_t>& bytes) {
    std::cout.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
int main(int argc, char** argv) {
    if (argc < 2) return 2;
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    ArchiveLimits limits;
    const std::string mode = argc >= 4 ? argv[3] : "";
    if (mode == "--small-budget") limits.entry_bytes = 16;
    auto archive = Archive::Open(std::filesystem::u8path(argv[1]), limits);
    if (!archive) return error(archive.error);
    if (argc >= 3) {
        auto bytes = mode == "--range" && argc == 6
            ? archive.value->ReadRange(static_cast<std::size_t>(std::stoull(argv[2])),
                                       std::stoull(argv[4]), std::stoull(argv[5]))
            : archive.value->Read(argv[2]);
        if (!bytes) return error(bytes.error);
        if (mode == "--concurrent") {
            std::vector<std::future<Result<std::vector<std::uint8_t>>>> pending;
            for (int i = 0; i < 8; ++i)
                pending.push_back(std::async(std::launch::async, [&] { return archive.value->Read(argv[2]); }));
            for (auto& future : pending) {
                auto other = future.get();
                if (!other) return error(other.error);
                if (other.value != bytes.value) return 3;
            }
        }
        archive.value.reset(); // returned storage must outlive the archive
        output(bytes.value);
    } else {
        std::size_t index = 0;
        for (const auto& entry : archive.value->entries()) {
            auto bytes = archive.value->Read(index++);
            if (!bytes) { std::cerr << entry.name << ": "; return error(bytes.error); }
            word(entry.name.size(), 4);
            std::cout.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
            word(bytes.value.size(), 8);
            output(bytes.value);
        }
    }
    return std::cout ? 0 : 4;
}
