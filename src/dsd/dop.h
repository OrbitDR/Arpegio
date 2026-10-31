// DSD over PCM. Each 24-bit PCM sample carries two DSD bytes under a marker byte.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "audio/device_format.h"

namespace arpegio {

constexpr uint8_t kDopMarkerA = 0x05;
constexpr uint8_t kDopMarkerB = 0xFA;

class DopPacker {
public:
    // `encoding` is the PCM container, S32 or S24.
    DopPacker(WireEncoding encoding, int channels);

    // Takes planar MSB-first DSD bytes and writes interleaved container samples. The marker byte
    // stays zero: the real-time side stamps it, which keeps the sequence continuous across seeks
    // and silence. Returns the frames written. An odd trailing byte waits for the next call.
    size_t pack(const uint8_t* const* planar, size_t bytes, uint8_t* out);

    void reset();

private:
    WireEncoding encoding_;
    int channels_;
    bool has_carry_ = false;
    std::vector<uint8_t> carry_;
};

// Writes the alternating marker into `frames` frames. `phase` persists between calls.
void dop_stamp_markers(uint8_t* frames, size_t count, int channels, size_t sample_bytes, uint32_t& phase) noexcept;

}  // namespace arpegio
