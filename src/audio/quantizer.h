// Turns doubles into device bytes. With dither off, values that came from integers of the same
// or shorter word length come back exactly, which is what keeps the bit-perfect path honest.
#pragma once

#include <cstddef>
#include <cstdint>

#include "audio/device_format.h"

namespace arpegio {

class Quantizer {
public:
    // `dither_bits` 0 disables dither. Otherwise TPDF dither sized for that word length.
    Quantizer(WireEncoding encoding, int dither_bits);

    void convert(const double* samples, size_t count, uint8_t* out);

    // Takes effect from the next sample, so a stream can keep its random sequence across tracks.
    void set_dither_bits(int dither_bits);

    uint64_t clipped() const { return clipped_; }

private:
    double next_dither();

    WireEncoding encoding_;
    double dither_scale_ = 0.0;
    uint64_t rng_ = 0x9E3779B97F4A7C15ull;
    uint64_t clipped_ = 0;
};

// The inverse of Quantizer::convert for PCM encodings. Every value comes back exactly.
void read_wire_samples(const uint8_t* bytes, WireEncoding encoding, double* samples, size_t count);

}  // namespace arpegio
