#pragma once
#include <string>

namespace albion::data {
enum class ErrorCode { none, io, unsupported, malformed, limit, decode, size_mismatch, not_found, ambiguous, out_of_range };
struct Error {
    ErrorCode code = ErrorCode::none;
    std::string message;
};
template<class T> struct Result {
    T value{};
    Error error{};
    explicit operator bool() const { return error.code == ErrorCode::none; }
};
} // namespace albion::data
