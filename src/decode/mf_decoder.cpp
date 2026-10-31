// Media Foundation: AAC, ALAC and WMA, the codecs Windows licenses and ships itself.
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "decode/decoder.h"

using Microsoft::WRL::ComPtr;

namespace arpegio {

namespace {

constexpr DWORD kAudioStream = MF_SOURCE_READER_FIRST_AUDIO_STREAM;
constexpr double kHnsPerSecond = 10000000.0;

void ensure_started() {
    static std::once_flag once;
    std::call_once(once, [] { MFStartup(MF_VERSION, MFSTARTUP_LITE); });
}

class MfDecoder final : public Decoder {
public:
    explicit MfDecoder(const std::filesystem::path& path) {
        ensure_started();
        HRESULT result = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader_);
        if (FAILED(result)) throw std::runtime_error("this format has no decoder on this system");
        reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader_->SetStreamSelection(kAudioStream, TRUE);

        ComPtr<IMFMediaType> native;
        result = reader_->GetNativeMediaType(kAudioStream, 0, &native);
        if (FAILED(result)) throw std::runtime_error("no audio stream found");
        GUID subtype{};
        native->GetGUID(MF_MT_SUBTYPE, &subtype);
        UINT32 rate = 0;
        UINT32 channels = 0;
        UINT32 bits = 0;
        native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        native->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);

        // Lossless codecs must come out as integers of their own word length to stay exact.
        lossless_ = subtype == MFAudioFormat_ALAC || subtype == MFAudioFormat_FLAC || subtype == MFAudioFormat_WMAudio_Lossless;
        const UINT32 output_bits = !lossless_ ? 32 : bits <= 16 ? 16 : bits <= 24 ? 24 : 32;

        ComPtr<IMFMediaType> wanted;
        MFCreateMediaType(&wanted);
        wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        wanted->SetGUID(MF_MT_SUBTYPE, lossless_ ? MFAudioFormat_PCM : MFAudioFormat_Float);
        wanted->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, output_bits);
        result = reader_->SetCurrentMediaType(kAudioStream, nullptr, wanted.Get());
        if (FAILED(result)) throw std::runtime_error("this format has no decoder on this system");

        ComPtr<IMFMediaType> current;
        reader_->GetCurrentMediaType(kAudioStream, &current);
        UINT32 block = 0;
        current->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        current->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        current->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &sample_bits_);
        current->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &block);
        if (rate == 0 || channels == 0) throw std::runtime_error("no audio stream found");

        format_.rate = rate;
        format_.channels = static_cast<int>(channels);
        format_.bits = lossless_ ? (bits ? static_cast<int>(bits) : static_cast<int>(sample_bits_)) : 16;

        PROPVARIANT duration;
        PropVariantInit(&duration);
        if (SUCCEEDED(reader_->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration))) {
            format_.length = static_cast<uint64_t>(duration.uhVal.QuadPart / kHnsPerSecond * rate);
        }
        PropVariantClear(&duration);
    }

    const SourceFormat& format() const override { return format_; }

    size_t read_pcm(double* interleaved, size_t frames) override {
        const size_t wanted = frames * format_.channels;
        size_t done = 0;
        while (done < wanted) {
            if (pending_position_ < pending_.size()) {
                const size_t count = std::min(wanted - done, pending_.size() - pending_position_);
                std::copy_n(pending_.data() + pending_position_, count, interleaved + done);
                pending_position_ += count;
                done += count;
                continue;
            }
            if (!read_sample()) break;
        }
        return done / format_.channels;
    }

    void seek(uint64_t position) override {
        PROPVARIANT target;
        InitPropVariantFromInt64(static_cast<LONGLONG>(position * kHnsPerSecond / format_.rate), &target);
        reader_->SetCurrentPosition(GUID_NULL, target);
        PropVariantClear(&target);
        pending_.clear();
        pending_position_ = 0;
        ended_ = false;
    }

private:
    bool read_sample() {
        if (ended_) return false;
        for (;;) {
            DWORD flags = 0;
            ComPtr<IMFSample> sample;
            const HRESULT result = reader_->ReadSample(kAudioStream, 0, nullptr, &flags, nullptr, &sample);
            if (FAILED(result) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
                ended_ = true;
                return false;
            }
            if (!sample) continue;

            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;
            BYTE* data = nullptr;
            DWORD length = 0;
            if (FAILED(buffer->Lock(&data, nullptr, &length))) continue;
            convert(data, length);
            buffer->Unlock();
            return true;
        }
    }

    void convert(const BYTE* data, DWORD length) {
        pending_position_ = 0;
        if (!lossless_) {
            pending_.resize(length / 4);
            for (size_t i = 0; i < pending_.size(); ++i) {
                float value;
                std::memcpy(&value, data + i * 4, 4);
                pending_[i] = value;
            }
            return;
        }
        const size_t step = sample_bits_ / 8;
        pending_.resize(length / step);
        for (size_t i = 0; i < pending_.size(); ++i) {
            uint32_t value = 0;
            std::memcpy(reinterpret_cast<uint8_t*>(&value) + (4 - step), data + i * step, step);
            pending_[i] = static_cast<int32_t>(value) / 2147483648.0;
        }
    }

    ComPtr<IMFSourceReader> reader_;
    SourceFormat format_;
    UINT32 sample_bits_ = 32;
    bool lossless_ = false;
    bool ended_ = false;
    std::vector<double> pending_;
    size_t pending_position_ = 0;
};

}  // namespace

std::unique_ptr<Decoder> open_media_foundation_decoder(const std::filesystem::path& path) {
    return std::make_unique<MfDecoder>(path);
}

}  // namespace arpegio
