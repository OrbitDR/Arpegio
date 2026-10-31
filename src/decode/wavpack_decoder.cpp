// WavPack through its reference library, including DSD stored in WavPack.
#include <windows.h>

#include <wavpack.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "decode/decoder.h"

namespace arpegio {

namespace {

constexpr std::array<uint8_t, 256> make_bit_reverse() {
    std::array<uint8_t, 256> table{};
    for (int i = 0; i < 256; ++i) {
        int r = 0;
        for (int b = 0; b < 8; ++b) r |= ((i >> b) & 1) << (7 - b);
        table[i] = static_cast<uint8_t>(r);
    }
    return table;
}

constexpr auto kBitReverse = make_bit_reverse();

std::string to_utf8(const std::wstring& text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
    return out;
}

class WavpackDecoder final : public Decoder {
public:
    explicit WavpackDecoder(const std::filesystem::path& path) {
        char error[80] = {};
        const int flags = OPEN_WVC | OPEN_DSD_NATIVE | OPEN_ALT_TYPES | OPEN_FILE_UTF8;
        context_ = WavpackOpenFileInput(to_utf8(path.wstring()).c_str(), error, flags, 0);
        if (!context_) throw std::runtime_error(std::string("WavPack: ") + error);

        const int qualify = WavpackGetQualifyMode(context_);
        format_.channels = WavpackGetNumChannels(context_);
        format_.length = static_cast<uint64_t>(WavpackGetNumSamples64(context_));
        format_.dsd = (qualify & QMODE_DSD_AUDIO) != 0;
        if (format_.dsd) {
            // In native mode one "sample" is one DSD byte per channel.
            format_.rate = WavpackGetNativeSampleRate(context_);
            format_.bits = 1;
            reverse_bits_ = (qualify & QMODE_DSD_LSB_FIRST) != 0;
        } else {
            format_.rate = WavpackGetSampleRate(context_);
            format_.bits = WavpackGetBitsPerSample(context_);
            is_float_ = (WavpackGetMode(context_) & MODE_FLOAT) != 0;
            scale_ = 1.0 / static_cast<double>(1ull << (WavpackGetBytesPerSample(context_) * 8 - 1));
        }
    }

    ~WavpackDecoder() override { WavpackCloseFile(context_); }

    const SourceFormat& format() const override { return format_; }

    size_t read_pcm(double* interleaved, size_t frames) override {
        const int channels = format_.channels;
        buffer_.resize(frames * channels);
        const uint32_t got = WavpackUnpackSamples(context_, buffer_.data(), static_cast<uint32_t>(frames));
        const size_t samples = static_cast<size_t>(got) * channels;
        for (size_t i = 0; i < samples; ++i) {
            if (is_float_) {
                float value;
                std::memcpy(&value, &buffer_[i], 4);
                interleaved[i] = value;
            } else {
                interleaved[i] = buffer_[i] * scale_;
            }
        }
        return got;
    }

    size_t read_dsd(uint8_t* const* planar, size_t bytes) override {
        const int channels = format_.channels;
        buffer_.resize(bytes * channels);
        const uint32_t got = WavpackUnpackSamples(context_, buffer_.data(), static_cast<uint32_t>(bytes));
        for (int ch = 0; ch < channels; ++ch) {
            const int32_t* in = buffer_.data() + ch;
            for (uint32_t i = 0; i < got; ++i, in += channels) {
                const auto byte = static_cast<uint8_t>(*in);
                planar[ch][i] = reverse_bits_ ? kBitReverse[byte] : byte;
            }
        }
        return got;
    }

    void seek(uint64_t position) override { WavpackSeekSample64(context_, static_cast<int64_t>(position)); }

private:
    WavpackContext* context_ = nullptr;
    SourceFormat format_;
    std::vector<int32_t> buffer_;
    double scale_ = 1.0;
    bool is_float_ = false;
    bool reverse_bits_ = false;
};

}  // namespace

std::unique_ptr<Decoder> open_wavpack_decoder(const std::filesystem::path& path) {
    return std::make_unique<WavpackDecoder>(path);
}

}  // namespace arpegio
