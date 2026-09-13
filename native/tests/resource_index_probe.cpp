#include "albion/resource_index.h"

#include <cstdint>
#include <future>
#include <iostream>
#include <vector>

using namespace albion::data;

// Streaming fixture protocol: mount count, input budget, query count; each
// mount's signed priority/count followed by hash/index pairs; query hashes.
int main() {
    std::size_t mount_count, budget, query_count;
    while (std::cin >> mount_count >> budget >> query_count) {
        if (mount_count > 10000 || query_count > 1000000) return 2;
        std::vector<ResourceMount> mounts(mount_count);
        for (auto& mount : mounts) {
            std::size_t count;
            if (!(std::cin >> mount.priority >> count) || count > 1000000) return 2;
            mount.entries.resize(count);
            for (auto& entry : mount.entries)
                if (!(std::cin >> entry.hash >> entry.entry_index)) return 2;
        }
        std::vector<std::uint32_t> queries(query_count);
        for (auto& hash : queries) if (!(std::cin >> hash)) return 2;
        auto result = ResourceIndex::Build(mounts, budget);
        mounts.clear(); // index owns its selected data, independent of inputs
        std::cout << static_cast<int>(result.error.code) << ' ' << result.value.size();
        if (result) {
            auto read = [&] {
                std::vector<std::uint64_t> locations;
                for (auto hash : queries) {
                    auto location = result.value.Find(hash);
                    locations.push_back(location
                        ? (static_cast<std::uint64_t>(location->mount_index) << 32) | location->entry_index
                        : UINT64_MAX);
                }
                return locations;
            };
            auto concurrent = std::async(std::launch::async, read);
            auto values = read();
            if (concurrent.get() != values) return 3;
            for (auto value : values) std::cout << ' ' << value;
        }
        std::cout << '\n';
    }
    return std::cin.eof() ? 0 : 2;
}
