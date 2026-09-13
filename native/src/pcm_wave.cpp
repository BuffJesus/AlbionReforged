#include "albion/pcm_wave.h"
#include <array>
#include <cstring>
#include <new>

namespace albion::data {
namespace {
std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8));
}
std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
Result<Pcm16Clip> fail(ErrorCode code, const char* message) { return {{}, {code, message}}; }
}

Result<Pcm16Clip> DecodePcm16Wave(const std::vector<std::uint8_t>& wave, std::uint64_t max_pcm_bytes) {
    if (wave.size() < 12 || std::memcmp(wave.data(), "RIFF", 4) ||
        std::memcmp(wave.data() + 8, "WAVE", 4))
        return fail(ErrorCode::unsupported, "expected RIFF/WAVE");
    const auto extent = std::uint64_t(le32(wave.data() + 4)) + 8;
    if (extent != wave.size()) return fail(ErrorCode::malformed, "RIFF extent differs from input size");
    const std::uint8_t* format = nullptr;
    const std::uint8_t* samples = nullptr;
    std::size_t format_size = 0, sample_size = 0;
    for (std::size_t pos = 12; pos < wave.size();) {
        if (wave.size() - pos < 8) return fail(ErrorCode::malformed, "truncated chunk header");
        const auto* chunk = wave.data() + pos;
        const auto size = le32(chunk + 4);
        pos += 8;
        if (size > wave.size() - pos) return fail(ErrorCode::malformed, "truncated chunk payload");
        if (!std::memcmp(chunk, "fmt ", 4)) {
            if (format) return fail(ErrorCode::ambiguous, "multiple format chunks");
            format = wave.data() + pos; format_size = size;
        } else if (!std::memcmp(chunk, "data", 4)) {
            if (samples) return fail(ErrorCode::ambiguous, "multiple data chunks");
            samples = wave.data() + pos; sample_size = size;
        }
        pos += size;
        if (size & 1) {
            if (pos == wave.size()) return fail(ErrorCode::malformed, "missing chunk padding");
            ++pos;
        }
    }
    if (!format || !samples || format_size < 16)
        return fail(ErrorCode::malformed, "missing or incomplete format/data chunk");
    const auto tag = le16(format);
    if (tag != 1 && tag != 0xfffe) return fail(ErrorCode::unsupported, "only PCM formats are supported");
    if (le16(format + 14) != 16) return fail(ErrorCode::unsupported, "only PCM16 samples are supported");
    if (format_size != 16 && (format_size < 18 || le16(format + 16) > format_size - 18))
        return fail(ErrorCode::malformed, "invalid format extension length");
    std::uint32_t channel_mask = 0;
    if (tag == 0xfffe) {
        static constexpr std::array<std::uint8_t, 16> pcm_guid{
            1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        if (format_size < 40 || le16(format + 16) < 22)
            return fail(ErrorCode::malformed, "incomplete extensible format");
        if (le16(format + 18) != 16 || std::memcmp(format + 24, pcm_guid.data(), pcm_guid.size()))
            return fail(ErrorCode::unsupported, "extensible format must contain PCM16");
        channel_mask = le32(format + 20);
    }
    const auto channels = le16(format + 2);
    const auto rate = le32(format + 4);
    const auto alignment = std::uint32_t(channels) * 2;
    if (!channels || !rate || le16(format + 12) != alignment ||
        std::uint64_t(rate) * alignment != le32(format + 8))
        return fail(ErrorCode::malformed, "invalid PCM channels, rate or alignment");
    if (sample_size % alignment) return fail(ErrorCode::size_mismatch, "partial PCM sample frame");
    if (sample_size > max_pcm_bytes) return fail(ErrorCode::limit, "PCM exceeds configured budget");
    try {
        Pcm16Clip clip;
        clip.channels = channels;
        clip.sample_rate = rate;
        clip.channel_mask = channel_mask;
        clip.bytes.assign(samples, samples + sample_size);
        return {std::move(clip), {}};
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::limit, "PCM allocation failed");
    }
}
} // namespace albion::data
