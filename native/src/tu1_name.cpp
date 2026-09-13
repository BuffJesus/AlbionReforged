#include "albion/tu1_name.h"
#include <new>

namespace albion::compat {
albion::data::Result<std::u16string> NormalizeTu1ResourceName(
    std::u16string_view input, std::size_t max_units) {
    using albion::data::ErrorCode;
    if (input.size() > max_units) return {{}, {ErrorCode::limit, "name exceeds unit budget"}};
    try {
        std::u16string result(input);
        for (auto& unit : result) {
            if (unit >= u'A' && unit <= u'Z') unit += 0x20;
            if (unit == u'/') unit = u'\\';
        }
        return {std::move(result), {}};
    } catch (const std::bad_alloc&) {
        return {{}, {ErrorCode::limit, "name allocation failed"}};
    }
}
}
