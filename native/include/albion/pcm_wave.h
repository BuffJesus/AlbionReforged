#pragma once
#include "albion/result.h"
#include <cstdint>
#include <vector>

namespace albion::data {
struct Pcm16Clip {
    // Owned interleaved little-endian signed PCM16, without WAV/container bytes.
    std::vector<std::uint8_t> bytes;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t channel_mask = 0; // zero means unspecified
    std::uint64_t frames() const { return channels ? bytes.size() / (2ull * channels) : 0; }
};

// Device-free parser extracted from the native frontend's PCM/WAVE boundary.
// Supports PCM and extensible PCM16; validates the declared RIFF extent, chunk
// bounds, complete sample frames and format rates. No resampling, playback,
// source-XMA decoding, or implicit format conversion. Errors carry no partial clip.
Result<Pcm16Clip> DecodePcm16Wave(const std::vector<std::uint8_t>& wave,
                                std::uint64_t max_pcm_bytes = 256ull << 20);
} // namespace albion::data
