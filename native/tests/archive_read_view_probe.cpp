#include "albion/archive_read_view.h"
#include "albion/archive_handles.h"
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <string>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace albion::data;
int fail(const Error& e) {
    std::cerr << static_cast<int>(e.code) << ": " << e.message << '\n';
    return 20 + static_cast<int>(e.code);
}
int main(int argc, char** argv) {
    if (argc < 8 || argc > 9) return 2;
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const std::string mode = argc == 9 ? argv[8] : "";
    std::ifstream source(std::filesystem::u8path(argv[1]), std::ios::binary | std::ios::ate);
    if (!source || source.tellg() > 12000000) return 2;
    source.seekg(0);
    std::vector<std::uint8_t> table{std::istreambuf_iterator<char>(source), {}};
    const auto budget = static_cast<std::size_t>(std::stoull(argv[7]));
    auto decoded = DecodeResourceRecords(mode == "--null-span" ? nullptr : table.data(), table.size(), budget);
    if (!decoded) return fail(decoded.error);
    table.clear(); // decoded metadata must own its input
    ArchiveHandles handles(2);
    std::vector<ArchiveHandle> tokens;
    std::vector<std::shared_ptr<const Archive>> leases;
    for (int i = 2; i <= 3; ++i) {
        auto archive = Archive::Open(std::filesystem::u8path(argv[i]));
        if (!archive) return fail(archive.error);
        auto inserted = handles.Insert(std::shared_ptr<const Archive>(std::move(archive.value)));
        if (!inserted) return fail(inserted.error);
        tokens.push_back(inserted.value);
        leases.push_back(handles.Acquire(inserted.value).value);
    }
    if (mode == "--null-mount") leases[0].reset();
    auto view = ArchiveReadView::Create(std::move(leases), std::move(decoded.value), budget);
    if (!view) return fail(view.error);
    for (auto token : tokens) {
        if (handles.Release(token).code != ErrorCode::none || handles.Acquire(token)) return 3;
    }
    const auto hash = static_cast<std::uint32_t>(std::stoull(argv[4]));
    const auto offset = std::stoull(argv[5]), count = std::stoull(argv[6]);
    auto concurrent = std::async(std::launch::async, [&] { return view.value.ReadRange(hash, offset, count); });
    auto bytes = view.value.ReadRange(hash, offset, count);
    auto other = concurrent.get();
    if (bytes.error.code != other.error.code || bytes.value != other.value) return 3;
    if (!bytes) return fail(bytes.error);
    view = {}; // returned storage survives destruction of all source handles/view
    std::cout.write(reinterpret_cast<const char*>(bytes.value.data()), static_cast<std::streamsize>(bytes.value.size()));
    return std::cout ? 0 : 4;
}
