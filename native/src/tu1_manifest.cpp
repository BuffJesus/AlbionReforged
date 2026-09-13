#include "albion/tu1_manifest.h"
#include <algorithm>
#include <new>

namespace albion::compat {
using namespace albion::data;
Result<std::vector<ManifestRecord>> ReadTu1Manifest(Source source, ManifestLimits limits) {
    const auto fail = [](ErrorCode code, const char* reason) -> Result<std::vector<ManifestRecord>> {
        return {{}, {code, reason}};
    };
    if (!source) return fail(ErrorCode::malformed, "null manifest source");
    if (!limits.read_bytes || limits.read_bytes > (128u << 10) || limits.line_bytes > 2048)
        return fail(ErrorCode::limit, "unsupported manifest framing budget");
    const auto size = source->size();
    if (size > limits.bytes) return fail(ErrorCode::limit, "manifest exceeds byte budget");
    try {
        std::vector<ManifestRecord> records;
        std::string line;
        auto emit = [&](bool terminated) {
            if (line.empty()) return true;
            if (records.size() >= limits.records) return false;
            const auto ordinal = static_cast<std::uint32_t>(records.size()) + 1;
            records.push_back({std::move(line), ordinal, terminated});
            line.clear();
            return true;
        };
        for (std::uint64_t offset = 0; offset < size;) {
            const auto count = std::min<std::uint64_t>(limits.read_bytes, size - offset);
            auto bytes = source->ReadRange(offset, count);
            if (!bytes) return {{}, bytes.error};
            if (bytes.value.size() != count)
                return fail(ErrorCode::size_mismatch, "manifest source returned wrong byte count");
            for (auto ch : bytes.value) {
                if (ch == '\r' || ch == '\n') {
                    if (!emit(true)) return fail(ErrorCode::limit, "manifest record budget exceeded");
                } else {
                    if (!ch || ch >= 128) return fail(ErrorCode::unsupported, "manifest byte conversion unproved");
                    if (line.size() >= limits.line_bytes)
                        return fail(ErrorCode::limit, "manifest line exceeds budget");
                    line.push_back(static_cast<char>(ch));
                }
            }
            offset += count;
        }
        if (!emit(false)) return fail(ErrorCode::limit, "manifest record budget exceeded");
        return {std::move(records), {}};
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::limit, "manifest allocation failed");
    }
}
} // namespace albion::compat
