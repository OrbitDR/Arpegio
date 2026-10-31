#include "decode/range_decoder.h"

#include <algorithm>
#include <stdexcept>

namespace arpegio {

namespace {

class RangeDecoder final : public Decoder {
public:
    RangeDecoder(std::unique_ptr<Decoder> inner, uint64_t start, uint64_t end) : inner_(std::move(inner)), start_(start) {
        format_ = inner_->format();
        if (start >= end || end > format_.length) throw std::runtime_error("the track does not fit in its file");
        format_.length = end - start;
        inner_->seek(start_);
    }

    const SourceFormat& format() const override { return format_; }

    size_t read_pcm(double* interleaved, size_t frames) override {
        const size_t wanted = remaining(frames);
        const size_t got = wanted ? inner_->read_pcm(interleaved, wanted) : 0;
        position_ += got;
        return got;
    }

    size_t read_dsd(uint8_t* const* planar, size_t bytes) override {
        const size_t wanted = remaining(bytes);
        const size_t got = wanted ? inner_->read_dsd(planar, wanted) : 0;
        position_ += got;
        return got;
    }

    void seek(uint64_t position) override {
        position_ = std::min(position, format_.length);
        inner_->seek(start_ + position_);
    }

private:
    size_t remaining(size_t wanted) const {
        return static_cast<size_t>(std::min<uint64_t>(wanted, format_.length - position_));
    }

    std::unique_ptr<Decoder> inner_;
    SourceFormat format_;
    uint64_t start_;
    uint64_t position_ = 0;
};

}  // namespace

std::unique_ptr<Decoder> make_range_decoder(std::unique_ptr<Decoder> inner, uint64_t start, uint64_t end) {
    return std::make_unique<RangeDecoder>(std::move(inner), start, end);
}

}  // namespace arpegio
