#include "albion/tu1_read_compare.h"

#include <algorithm>

namespace albion::compat {
using namespace albion::data;

Result<ReadComparison> CompareTu1SynchronousRead(
    const ArchiveReadView& view, ReadRequest request, ObservedRead observed) {
    if (observed.status != 0 && observed.status != 0x80004005u)
        return {{}, {ErrorCode::unsupported, "unmapped TU1 synchronous read status"}};
    if ((observed.size && !observed.bytes) || (observed.status && observed.size))
        return {{}, {ErrorCode::malformed, "invalid completed-read observation"}};
    auto native = view.ReadRange(request.hash, request.offset, request.count);
    ReadComparison result;
    result.native_error = native.error.code;
    result.native_size = native.value.size();
    result.observed_size = observed.size;
    if (observed.status != 0) {
        result.agreement = native ? ReadAgreement::outcome_mismatch : ReadAgreement::both_failed;
    } else if (!native) {
        result.agreement = ReadAgreement::outcome_mismatch;
    } else if (native.value.size() != observed.size) {
        result.agreement = ReadAgreement::length_mismatch;
    } else {
        result.agreement = ReadAgreement::equal;
        for (std::size_t i = 0; i < observed.size; ++i) {
            if (native.value[i] != observed.bytes[i]) {
                result.agreement = ReadAgreement::byte_mismatch;
                result.first_difference = i;
                break;
            }
        }
    }
    return {result, {}};
}

} // namespace albion::compat
