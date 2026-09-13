#pragma once
#include "albion/archive_read_view.h"

namespace albion::compat {

struct ReadRequest {
    std::uint32_t hash = 0;
    std::uint64_t offset = 0;
    std::uint64_t count = 0;
};
struct ObservedRead {
    std::uint32_t status = 0;
    const std::uint8_t* bytes = nullptr;
    std::size_t size = 0;
};
enum class ReadAgreement { equal, byte_mismatch, length_mismatch, outcome_mismatch, both_failed };
struct ReadComparison {
    ReadAgreement agreement = ReadAgreement::outcome_mismatch;
    albion::data::ErrorCode native_error = albion::data::ErrorCode::none;
    std::uint64_t native_size = 0;
    std::uint64_t observed_size = 0;
    std::optional<std::uint64_t> first_difference;
};

// Comparison only: never writes caller/guest storage or changes the guest result.
// Specific to TU1 synchronous entry read 0x82B5ACC8: 0 means success,
// 0x80004005 means failure. Other statuses are unsupported, not guessed.
// Call only after the observed read finishes, while its byte span is valid.
// The view must describe the same selected mounts/entries and immutable content.
// Two failures are reported separately from equality: their causes may differ.
albion::data::Result<ReadComparison> CompareTu1SynchronousRead(
    const albion::data::ArchiveReadView& view, ReadRequest request, ObservedRead observed);

} // namespace albion::compat
