#include "dsd/dop.h"

namespace arpegio {

DopPacker::DopPacker(WireEncoding encoding, int channels)
    : encoding_(encoding), channels_(channels), carry_(channels) {}

void DopPacker::reset() {
    has_carry_ = false;
}

size_t DopPacker::pack(const uint8_t* const* planar, size_t bytes, uint8_t* out) {
    if (bytes == 0) return 0;
    const size_t sample_bytes = wire_sample_bytes(encoding_);
    const size_t low_offset = sample_bytes - 3;  // S32 keeps its lowest byte at zero
    const size_t total = bytes + (has_carry_ ? 1 : 0);
    const size_t frames = total / 2;

    for (int ch = 0; ch < channels_; ++ch) {
        const uint8_t* in = planar[ch];
        uint8_t* sample = out + ch * sample_bytes;
        size_t position = 0;
        for (size_t frame = 0; frame < frames; ++frame) {
            const uint8_t older = (frame == 0 && has_carry_) ? carry_[ch] : in[position++];
            const uint8_t newer = in[position++];
            sample[0] = 0;
            sample[low_offset] = newer;
            sample[low_offset + 1] = older;
            sample[low_offset + 2] = 0;
            sample += sample_bytes * channels_;
        }
        if (total % 2 != 0) carry_[ch] = in[position];
    }
    has_carry_ = total % 2 != 0;
    return frames;
}

void dop_stamp_markers(uint8_t* frames, size_t count, int channels, size_t sample_bytes, uint32_t& phase) noexcept {
    uint8_t* marker = frames + sample_bytes - 1;
    for (size_t frame = 0; frame < count; ++frame) {
        const uint8_t value = (phase & 1) ? kDopMarkerB : kDopMarkerA;
        for (int ch = 0; ch < channels; ++ch) {
            *marker = value;
            marker += sample_bytes;
        }
        ++phase;
    }
}

}  // namespace arpegio
