// Monkey's Audio through the official SDK.
#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "All.h"
#include "MACLib.h"

#include "decode/decoder.h"

namespace arpegio {

namespace {

class ApeDecoder final : public Decoder {
public:
    explicit ApeDecoder(const std::filesystem::path& path) {
        int error = 0;
        decoder_.reset(CreateIAPEDecompress(path.c_str(), &error, true, false, false));
        if (!decoder_) throw std::runtime_error("APE: cannot open file");
        using Info = APE::IAPEDecompress;
        format_.rate = static_cast<uint32_t>(decoder_->GetInfo(Info::APE_INFO_SAMPLE_RATE));
        format_.channels = static_cast<int>(decoder_->GetInfo(Info::APE_INFO_CHANNELS));
        format_.bits = static_cast<int>(decoder_->GetInfo(Info::APE_INFO_BITS_PER_SAMPLE));
        format_.length = static_cast<uint64_t>(decoder_->GetInfo(Info::APE_DECOMPRESS_TOTAL_BLOCKS));
        sample_bytes_ = static_cast<size_t>(decoder_->GetInfo(Info::APE_INFO_BYTES_PER_SAMPLE));
        block_align_ = static_cast<size_t>(decoder_->GetInfo(Info::APE_INFO_BLOCK_ALIGN));
        if (sample_bytes_ < 1 || sample_bytes_ > 4 || block_align_ == 0) throw std::runtime_error("APE: unsupported format");
    }

    const SourceFormat& format() const override { return format_; }

    size_t read_pcm(double* interleaved, size_t frames) override {
        raw_.resize(frames * block_align_);
        APE::int64 retrieved = 0;
        if (decoder_->GetData(raw_.data(), static_cast<APE::int64>(frames), &retrieved) != ERROR_SUCCESS) return 0;

        const size_t samples = static_cast<size_t>(retrieved) * format_.channels;
        const uint8_t* p = raw_.data();
        for (size_t i = 0; i < samples; ++i, p += sample_bytes_) {
            if (sample_bytes_ == 1) {
                interleaved[i] = (static_cast<int>(*p) - 128) / 128.0;
            } else {
                uint32_t value = 0;
                std::memcpy(reinterpret_cast<uint8_t*>(&value) + (4 - sample_bytes_), p, sample_bytes_);
                interleaved[i] = static_cast<int32_t>(value) / 2147483648.0;
            }
        }
        return static_cast<size_t>(retrieved);
    }

    void seek(uint64_t position) override { decoder_->Seek(static_cast<APE::int64>(position)); }

private:
    std::unique_ptr<APE::IAPEDecompress> decoder_;
    SourceFormat format_;
    size_t sample_bytes_ = 0;
    size_t block_align_ = 0;
    std::vector<uint8_t> raw_;
};

}  // namespace

std::unique_ptr<Decoder> open_ape_decoder(const std::filesystem::path& path) {
    return std::make_unique<ApeDecoder>(path);
}

}  // namespace arpegio
