// Decides how a DSD stream becomes PCM: which rate, which divider and which filter.
#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace arpegio {

struct DsdRate {
    uint32_t base;      // 44100 or 48000
    uint32_t multiple;  // 64, 128, 256, 512, 1024
};

// Returns nothing for rates outside the DSD64..DSD1024 families.
std::optional<DsdRate> classify_dsd_rate(uint32_t rate);

struct DsdFilterSpec {
    double f3_hz;    // -3 dB point
    double pass_hz;  // end of the flat band
    double stop_hz;  // start of the stop band
    double atten_db;
};

struct DsdPcmPlan {
    uint32_t fir_rate;     // rate after the integer divider
    uint32_t divider;      // dsd_rate / fir_rate, always a multiple of 8
    uint32_t output_rate;  // rate delivered to the device
    bool needs_resampler;  // true when output_rate differs from fir_rate
    DsdFilterSpec filter;
};

// `device_rates` lists the PCM rates the output accepts. Empty means no restriction.
std::optional<DsdPcmPlan> plan_dsd_to_pcm(uint32_t dsd_rate, std::span<const uint32_t> device_rates);

// Same filter rules for a forced PCM rate that divides the DSD rate exactly.
DsdFilterSpec dsd_filter_for(const DsdRate& source, uint32_t fir_rate);

}  // namespace arpegio
