#pragma once
#include "albion/result.h"
#include <string>
#include <string_view>

namespace albion::compat {
// Counted UTF-16 code units: lowercase ASCII A-Z and replace '/' with '\'.
// Preserve all other units, including surrogates, NUL and FFFF. This is not
// Unicode case folding, encoding validation, path canonicalization or hashing.
// Caller must establish that the guest path actually invokes 8217AF70 (manifest
// EOF tails take another constructor path). No guest storage is accessed.
albion::data::Result<std::u16string> NormalizeTu1ResourceName(
    std::u16string_view input, std::size_t max_units = 2048);
}
