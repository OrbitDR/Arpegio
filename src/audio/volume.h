// The software volume, applied on the real-time thread to bytes already in device format. The ring can
// hold 90 s of audio, so anything earlier would be heard a minute and a half late. At unity the bytes are
// not touched at all, which keeps the bit-perfect path exactly what it was.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "audio/device_format.h"
#include "audio/quantizer.h"

namespace arpegio {

class Volume {
public:
    // `gain` is read from here on every call, and can change from any thread. It must not exceed 1.
    // `dither_bits` as in Quantizer, for samples this rewrites.
    Volume(const DeviceFormat& format, int dither_bits, const std::atomic<double>& gain);

    // Scales whole frames in place. Must not block, allocate or log.
    void apply(uint8_t* bytes, size_t frames) noexcept;

private:
    const std::atomic<double>& target_;
    WireEncoding encoding_;
    int channels_;
    size_t frame_bytes_;
    uint32_t ramp_frames_;
    Quantizer quantizer_;
    double gain_ = 1.0;
    double destination_ = 1.0;
    double ramp_step_ = 0.0;
    uint32_t ramp_left_ = 0;
};

}  // namespace arpegio
