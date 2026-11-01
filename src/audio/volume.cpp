#include "audio/volume.h"

#include <algorithm>
#include <cstring>

namespace arpegio {

namespace {

constexpr double kRampSeconds = 0.02;  // long enough that a change has no click, too short to hear as a fade
constexpr size_t kBlockSamples = 2048;

}  // namespace

Volume::Volume(const DeviceFormat& format, int dither_bits, const std::atomic<double>& gain)
    : target_(gain),
      encoding_(format.encoding),
      channels_(format.channels),
      frame_bytes_(format.frame_bytes()),
      ramp_frames_(std::max<uint32_t>(static_cast<uint32_t>(format.rate * kRampSeconds), 1)),
      quantizer_(format.encoding, dither_bits) {
    // A stream that opens with the volume already down starts there, without a ramp from full level.
    gain_ = destination_ = target_.load(std::memory_order_relaxed);
}

void Volume::apply(uint8_t* bytes, size_t frames) noexcept {
    const double target = target_.load(std::memory_order_relaxed);
    if (target != destination_) {
        destination_ = target;
        ramp_step_ = (destination_ - gain_) / ramp_frames_;
        ramp_left_ = ramp_frames_;
    }

    double block[kBlockSamples];
    const size_t block_frames = kBlockSamples / channels_;
    while (frames > 0) {
        // Checked again after each piece, so the frames after a ramp are exact from the very first one.
        if (ramp_left_ == 0 && gain_ == 1.0) return;
        if (ramp_left_ == 0 && gain_ == 0.0) {
            // Exact digital silence: dither alone would still be noise.
            std::memset(bytes, 0, frames * frame_bytes_);
            return;
        }

        const size_t count = std::min({frames, block_frames, ramp_left_ > 0 ? size_t{ramp_left_} : frames});
        read_wire_samples(bytes, encoding_, block, count * channels_);
        double* sample = block;
        for (size_t frame = 0; frame < count; ++frame) {
            if (ramp_left_ > 0) gain_ = --ramp_left_ == 0 ? destination_ : gain_ + ramp_step_;
            for (int channel = 0; channel < channels_; ++channel) *sample++ *= gain_;
        }
        quantizer_.convert(block, count * channels_, bytes);
        bytes += count * frame_bytes_;
        frames -= count;
    }
}

}  // namespace arpegio
