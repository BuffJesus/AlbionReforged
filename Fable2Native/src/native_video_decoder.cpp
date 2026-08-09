#include "f2/native_video_decoder.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace f2 {
namespace {

IMFSourceReader* reader_from(void* value) {
    return static_cast<IMFSourceReader*>(value);
}

bool fail(std::string& error, const char* message, HRESULT result = S_OK) {
    error = message;
    if (FAILED(result)) error += " (HRESULT 0x" + std::to_string(static_cast<unsigned long>(result)) + ")";
    return false;
}

}  // namespace

bool start_native_video_runtime(std::string& error) {
    const auto result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    return SUCCEEDED(result) || fail(error, "Media Foundation could not start", result);
}

void stop_native_video_runtime() {
    MFShutdown();
}

NativeVideoDecoder::~NativeVideoDecoder() {
    close();
}

bool NativeVideoDecoder::open(const std::filesystem::path& path, std::string& error) {
    close();
    ComPtr<IMFSourceReader> reader;
    auto result = MFCreateSourceReaderFromURL(path.wstring().c_str(), nullptr, &reader);
    if (FAILED(result)) return fail(error, "Media Foundation could not open the cooked video", result);

    struct OutputFormat {
        const GUID* subtype;
        PixelFormat format;
    };
    constexpr OutputFormat output_formats[] = {
        {&MFVideoFormat_ARGB32, PixelFormat::Argb32},
        {&MFVideoFormat_RGB32, PixelFormat::Rgb32},
        {&MFVideoFormat_YUY2, PixelFormat::Yuy2},
    };
    HRESULT format_result = E_FAIL;
    PixelFormat pixel_format = PixelFormat::Argb32;
    for (const auto& output : output_formats) {
        ComPtr<IMFMediaType> type;
        result = MFCreateMediaType(&type);
        if (FAILED(result)) return fail(error, "Media Foundation could not create a video type", result);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, *output.subtype);
        format_result = reader->SetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get());
        if (SUCCEEDED(format_result)) {
            pixel_format = output.format;
            break;
        }
    }
    if (FAILED(format_result)) {
        return fail(error, "Media Foundation could not select a supported video output", format_result);
    }

    ComPtr<IMFMediaType> current_type;
    result = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current_type);
    if (FAILED(result)) return fail(error, "Media Foundation could not query video dimensions", result);
    UINT width = 0;
    UINT height = 0;
    result = MFGetAttributeSize(current_type.Get(), MF_MT_FRAME_SIZE, &width, &height);
    if (FAILED(result) || width == 0 || height == 0) {
        return fail(error, "Media Foundation returned invalid video dimensions", result);
    }
    LONG stride = 0;
    if (FAILED(current_type->GetUINT32(MF_MT_DEFAULT_STRIDE,
                                      reinterpret_cast<UINT32*>(&stride)))) {
        stride = static_cast<LONG>(pixel_format == PixelFormat::Yuy2 ? width * 2 : width * 4);
    }

    double frame_duration_seconds = 1.0 / 30.0;
    UINT32 frame_rate_numerator = 0;
    UINT32 frame_rate_denominator = 0;
    if (SUCCEEDED(MFGetAttributeRatio(current_type.Get(), MF_MT_FRAME_RATE,
                                      &frame_rate_numerator, &frame_rate_denominator)) &&
        frame_rate_numerator != 0 && frame_rate_denominator != 0) {
        frame_duration_seconds = static_cast<double>(frame_rate_denominator) /
                                 static_cast<double>(frame_rate_numerator);
    }

    // Decode the whole first audio stream to interleaved 16-bit PCM. Best-effort: a missing or
    // undecodable audio stream just leaves the clip silent (as it was before), never fails open().
    audio_pcm_.clear();
    audio_channels_ = 0;
    audio_sample_rate_ = 0;
    decode_audio_stream(reader.Get());

    reader_ = reader.Detach();
    path_ = path;
    width_ = width;
    height_ = height;
    stride_ = stride;
    pixel_format_ = pixel_format;
    frame_duration_seconds_ = frame_duration_seconds;
    serial_ = 0;
    return true;
}

void NativeVideoDecoder::decode_audio_stream(void* reader_value) {
    auto* reader = reader_from(reader_value);

    ComPtr<IMFMediaType> pcm_type;
    if (FAILED(MFCreateMediaType(&pcm_type))) return;
    pcm_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pcm_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    pcm_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr,
                                           pcm_type.Get()))) {
        return;  // no audio stream, or PCM output unsupported
    }

    ComPtr<IMFMediaType> actual_type;
    if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual_type))) return;
    UINT32 channels = 0;
    UINT32 sample_rate = 0;
    if (FAILED(actual_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) || channels == 0 ||
        FAILED(actual_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sample_rate)) ||
        sample_rate == 0) {
        return;
    }

    std::vector<std::uint8_t> pcm;
    while (true) {
        DWORD stream_index = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &stream_index, &flags,
                                      &timestamp, &sample))) {
            return;  // give up on audio; leave the clip silent
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) break;
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length))) continue;
        pcm.insert(pcm.end(), data, data + length);
        buffer->Unlock();
    }

    if (pcm.empty()) return;
    audio_pcm_ = std::move(pcm);
    audio_channels_ = static_cast<std::uint16_t>(channels);
    audio_sample_rate_ = sample_rate;
}

bool NativeVideoDecoder::read_next_frame(NativeVideoFrame& frame, std::string& error) {
    if (!reader_) return fail(error, "No cooked video is open");
    while (true) {
        DWORD stream_index = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        const auto result = reader_from(reader_)->ReadSample(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream_index, &flags,
            &timestamp, &sample);
        if (FAILED(result)) return fail(error, "Media Foundation could not decode a video frame", result);
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) return false;
        if (!sample) {
            if ((flags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED) != 0) continue;
            continue;
        }

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
            return fail(error, "Media Foundation could not access a video frame");
        }
        BYTE* source = nullptr;
        DWORD max_length = 0;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&source, &max_length, &length))) {
            return fail(error, "Media Foundation could not lock a video frame");
        }
        NativeVideoFrame decoded;
        decoded.width = width_;
        decoded.height = height_;
        decoded.serial = ++serial_;
        decoded.rgba8.resize(static_cast<std::size_t>(width_) * height_ * 4);
        const auto source_stride = std::abs(stride_) > 0 ? std::abs(stride_) : width_ * 4;
        for (std::uint32_t row = 0; row < height_; ++row) {
            const auto source_row = stride_ >= 0 ? row : height_ - row - 1;
            const auto* source_pixels = source + static_cast<std::size_t>(source_row) * source_stride;
            auto* destination = decoded.rgba8.data() + static_cast<std::size_t>(row) * width_ * 4;
            if (pixel_format_ != PixelFormat::Yuy2) {
                for (std::uint32_t column = 0; column < width_; ++column) {
                    // ARGB32/RGB32 are BGRA/BGRX in little-endian memory on Windows.
                    destination[column * 4 + 0] = source_pixels[column * 4 + 2];
                    destination[column * 4 + 1] = source_pixels[column * 4 + 1];
                    destination[column * 4 + 2] = source_pixels[column * 4 + 0];
                    destination[column * 4 + 3] = 255;
                }
            } else {
                const auto clamp = [](float value) {
                    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 255.0f));
                };
                for (std::uint32_t column = 0; column < width_; column += 2) {
                    const auto* yuy2 = source_pixels + column * 2;
                    const float y0 = static_cast<float>(yuy2[0]) - 16.0f;
                    const float u = static_cast<float>(yuy2[1]) - 128.0f;
                    const float y1 = static_cast<float>(yuy2[2]) - 16.0f;
                    const float v = static_cast<float>(yuy2[3]) - 128.0f;
                    const auto write_pixel = [&](std::uint32_t x, float y) {
                        const float c = y > 0.0f ? y : 0.0f;
                        destination[x * 4 + 0] = clamp(1.164f * c + 1.596f * v);
                        destination[x * 4 + 1] = clamp(1.164f * c - 0.392f * u - 0.813f * v);
                        destination[x * 4 + 2] = clamp(1.164f * c + 2.017f * u);
                        destination[x * 4 + 3] = 255;
                    };
                    write_pixel(column, y0);
                    if (column + 1 < width_) write_pixel(column + 1, y1);
                }
            }
        }
        buffer->Unlock();
        frame = std::move(decoded);
        return true;
    }
}

void NativeVideoDecoder::close() {
    if (reader_) reader_from(reader_)->Release();
    reader_ = nullptr;
    path_.clear();
    width_ = 0;
    height_ = 0;
    stride_ = 0;
    pixel_format_ = PixelFormat::Argb32;
    frame_duration_seconds_ = 1.0 / 30.0;
    serial_ = 0;
    audio_pcm_.clear();
    audio_channels_ = 0;
    audio_sample_rate_ = 0;
}

}  // namespace f2
