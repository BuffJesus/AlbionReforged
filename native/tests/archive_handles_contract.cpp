#include "albion/archive_handles.h"
#include "albion/resource_index.h"

#include <future>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace albion::data;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
std::shared_ptr<const Archive> open(const char* path) {
    auto archive = Archive::Open(std::filesystem::u8path(path));
    require(bool(archive), "open fixture");
    return std::shared_ptr<const Archive>(std::move(archive.value));
}
void bytes(const std::shared_ptr<const Archive>& archive, const std::string& expected) {
    auto result = archive->Read(std::size_t{0});
    require(bool(result), "leased read failed");
    require(std::string(result.value.begin(), result.value.end()) == expected, "leased read changed archive");
}

int main(int argc, char** argv) try {
    if (argc != 3) return 2;
    auto a = open(argv[1]), b = open(argv[2]);
    ArchiveHandles table(1);
    require(table.Insert({}).error.code == ErrorCode::malformed, "null insert accepted");
    for (auto raw : {0u, 1u, 0x10000u, 0xffffffffu}) {
        require(table.Acquire({raw}).error.code == ErrorCode::not_found, "invalid acquire accepted");
        require(table.Release({raw}).code == ErrorCode::not_found, "invalid release accepted");
    }
    auto first = table.Insert(a);
    require(bool(first) && first.value.value != 0, "insert failed");
    auto lease = table.Acquire(first.value);
    require(bool(lease), "acquire failed");
    require(table.Insert(b).error.code == ErrorCode::limit, "capacity ignored");
    require(table.Release(first.value).code == ErrorCode::none, "release failed");
    require(table.Release(first.value).code == ErrorCode::not_found, "double release accepted");
    require(!table.Acquire(first.value), "released token remained valid");
    auto second = table.Insert(b);
    require(bool(second) && second.value.value != first.value.value, "generation did not change");
    require(!table.Acquire(first.value), "old token acquired new archive");
    bytes(lease.value, "archive A");
    bytes(table.Acquire(second.value).value, "archive B");

    // A retained lease survives both closing its token and destruction of the
    // table. Weak ownership observes actual Archive destruction at final release.
    std::shared_ptr<const Archive> surviving;
    std::weak_ptr<const Archive> weak;
    {
        ArchiveHandles scope;
        auto original = open(argv[1]);
        weak = original;
        auto token = scope.Insert(original).value;
        original.reset();
        surviving = scope.Acquire(token).value;
        scope.Release(token);
        require(!weak.expired(), "closed an in-flight archive");
    }
    bytes(surviving, "archive A");
    surviving.reset();
    require(weak.expired(), "leaked archive after final lease");

    // Synchronize a queued worker so its actual read begins AFTER token close
    // and slot reuse. This would fail a raw-pointer or token-only job design.
    ArchiveHandles jobs(1);
    auto token = jobs.Insert(a).value;
    auto retained = jobs.Acquire(token).value;
    std::promise<void> begin;
    auto ready = begin.get_future();
    auto worker = std::async(std::launch::async, [retained = std::move(retained), ready = std::move(ready)]() mutable {
        ready.get();
        bytes(retained, "archive A");
    });
    jobs.Release(token);
    auto replacement = jobs.Insert(b).value;
    begin.set_value();
    worker.get();
    bytes(jobs.Acquire(replacement).value, "archive B");

    // Join real native selection, handle acquisition and decoding. The fixture
    // supplies mount order/priorities/hashes; it does not emulate the registry.
    ArchiveHandles mounted;
    const auto mount_a = mounted.Insert(a).value;
    const auto mount_b = mounted.Insert(b).value;
    for (auto priorities : {std::pair<int, int>{0, 1}, {0, 0}, {0, -1}}) {
        auto index = ResourceIndex::Build({{priorities.first, {{42, 0}}},
                                          {priorities.second, {{42, 0}}}});
        require(bool(index), "mounted resource index build failed");
        auto selected = index.value.Find(42);
        require(bool(selected), "mounted resource not found");
        require(selected->entry_index == 0, "mounted entry index changed");
        auto handle = selected->mount_index == 0 ? mount_a : mount_b;
        auto selected_lease = mounted.Acquire(handle);
        require(bool(selected_lease), "selected mount lease failed");
        bytes(selected_lease.value, priorities.second >= priorities.first ? "archive B" : "archive A");
        require(!index.value.Find(43), "missing resource unexpectedly resolved");
    }

    // Acquire racing close may win a lease or report not_found. It may never
    // return the replacement resource under the old token.
    for (int i = 0; i < 128; ++i) {
        ArchiveHandles racing(1);
        auto old = racing.Insert(a).value;
        std::promise<void> start;
        auto signal = start.get_future().share();
        auto reader = std::async(std::launch::async, [&] {
            signal.wait();
            auto acquired = racing.Acquire(old);
            if (acquired) bytes(acquired.value, "archive A");
            else require(acquired.error.code == ErrorCode::not_found, "wrong racing error");
        });
        start.set_value();
        racing.Release(old);
        require(bool(racing.Insert(b)), "racing reuse failed");
        reader.get();
    }

    // Walk the entire generation space; no wraparound may revive the first ID.
    ArchiveHandles exhausted(1);
    ArchiveHandle ancient;
    for (std::uint32_t generation = 1; generation <= 65535; ++generation) {
        auto item = exhausted.Insert(a);
        require(bool(item), "generation prematurely exhausted");
        if (generation == 1) ancient = item.value;
        else require(!exhausted.Acquire(ancient), "generation ABA resurrected stale token");
        require(exhausted.Release(item.value).code == ErrorCode::none, "generation release failed");
        require(!exhausted.Acquire(item.value), "closed generation still live");
    }
    require(exhausted.Insert(b).error.code == ErrorCode::limit, "generation wrapped instead of retiring");
    require(!exhausted.Acquire(ancient), "ancient handle revived after retirement");
    ArchiveHandles disabled(0);
    require(disabled.Insert(a).error.code == ErrorCode::limit, "zero capacity ignored");
    std::cout << "handle ownership passed; 128 close/acquire races; 65535 generation cycles; exhausted slot retired\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
