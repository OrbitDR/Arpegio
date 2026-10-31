// DSD to PCM decimator: a linear-phase FIR evaluated one byte at a time through lookup tables.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "dsd/dsd_pcm_plan.h"

namespace arpegio {

constexpr uint8_t kDsdSilenceByte = 0x69;

// Kaiser-windowed sinc with unity DC gain. The length is a multiple of 8.
std::vector<double> design_dsd_fir(double dsd_rate, const DsdFilterSpec& spec);

// Magnitude of an FIR at `hz`, for design and tests.
double fir_magnitude(std::span<const double> taps, double sample_rate, double hz);

class DsdToPcm {
public:
    // `divider` must be a multiple of 8.
    DsdToPcm(std::vector<double> taps, uint32_t divider, int channels);

    // Takes `bytes` MSB-first DSD bytes per channel and writes interleaved samples where a
    // full-scale 1-bit stream of ones maps to +1.0. Returns the frames written, which never
    // exceed frames_for(bytes). Keeps the state, so calls can be any size.
    size_t process(const uint8_t* const* planar, size_t bytes, double* interleaved);

    size_t frames_for(size_t bytes) const { return (bytes + pending_) / step_; }

    // Clears the filter history, as after a seek.
    void reset();

    // Filter delay in output frames, rounded down.
    size_t delay_frames() const { return (taps_len_ - 1) / 2 / divider_; }

    int channels() const { return channels_; }

private:
    double filter_one(const uint8_t* newest) const;

    size_t taps_len_;
    uint32_t divider_;
    size_t step_;    // bytes per output frame
    size_t groups_;  // table rows, one per 8 taps
    int channels_;
    size_t pending_ = 0;  // bytes already held towards the next output frame
    std::vector<double> table_;
    std::vector<std::vector<uint8_t>> history_;  // newest byte last, `groups_` long
};

}  // namespace arpegio
