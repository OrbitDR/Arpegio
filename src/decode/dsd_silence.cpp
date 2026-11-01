#include "decode/dsd_silence.h"

#include <algorithm>
#include <cstring>

#include "dsd/dsd_fir.h"

namespace arpegio {

namespace {

class DsdSilence final : public Decoder {
public:
    DsdSilence(uint32_t rate, int channels, double seconds) {
        format_ = {true, rate, channels, 1, static_cast<uint64_t>(seconds * rate / 8.0)};
    }

    const SourceFormat& format() const override { return format_; }

    size_t read_dsd(uint8_t* const* planar, size_t bytes) override {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(bytes, format_.length - position_));
        for (int channel = 0; channel < format_.channels; ++channel) std::memset(planar[channel], kDsdSilenceByte, count);
        position_ += count;
        return count;
    }

    void seek(uint64_t position) override { position_ = std::min(position, format_.length); }

private:
    SourceFormat format_;
    uint64_t position_ = 0;
};

}  // namespace

std::unique_ptr<Decoder> make_dsd_silence(uint32_t rate, int channels, double seconds) {
    return std::make_unique<DsdSilence>(rate, channels, seconds);
}

}  // namespace arpegio
