#include "dsd/dsd_pcm_plan.h"

#include <algorithm>

namespace arpegio {

namespace {

constexpr double kFlatBandHz = 24000.0;
constexpr double kStopBandDb = 160.0;

// A Kaiser-windowed sinc this gentle rolls off like a Gaussian, without ringing. The -3 dB point
// sits at this fraction of the transition band, which lets the stop edge follow from f3.
constexpr double kF3Position = 0.463;

// The integer divider stops at twice the family base. Lower rates go through the resampler,
// because one FIR sharp enough for 44.1 kHz would need tens of thousands of taps at DSD512.
constexpr uint32_t kMinFirMultiplier = 2;
constexpr uint32_t kResamplerFeedMultiplier = 4;

bool supports(std::span<const uint32_t> rates, uint32_t rate) {
    return rates.empty() || std::find(rates.begin(), rates.end(), rate) != rates.end();
}

// Target rate as a multiple of the family base: 176.4 kHz for DSD64, 352.8 kHz above it.
uint32_t preferred_multiplier(uint32_t dsd_multiple) {
    return dsd_multiple <= 64 ? 4u : 8u;
}

double source_f3_hz(uint32_t dsd_multiple) {
    if (dsd_multiple <= 64) return 50000.0;
    if (dsd_multiple <= 128) return 70000.0;
    return 90000.0;
}

// Rate that feeds the resampler: the lowest family rate at or above the target, never below
// 176.4 kHz, so the resampler and not this FIR sets the final band edge.
uint32_t fir_rate_for_target(uint32_t base, uint32_t multiplier_max, uint32_t target) {
    uint32_t multiplier = kResamplerFeedMultiplier;
    while (multiplier < multiplier_max && base * multiplier < target) multiplier *= 2;
    return base * multiplier;
}

DsdPcmPlan make_plan(const DsdRate& source, uint32_t dsd_rate, uint32_t fir_rate, uint32_t output_rate) {
    DsdPcmPlan plan{};
    plan.fir_rate = fir_rate;
    plan.divider = dsd_rate / fir_rate;
    plan.output_rate = output_rate;
    plan.needs_resampler = output_rate != fir_rate;
    plan.filter = dsd_filter_for(source, fir_rate);
    return plan;
}

}  // namespace

std::optional<DsdRate> classify_dsd_rate(uint32_t rate) {
    for (uint32_t base : {44100u, 48000u}) {
        for (uint32_t multiple : {64u, 128u, 256u, 512u, 1024u}) {
            if (rate == base * multiple) return DsdRate{base, multiple};
        }
    }
    return std::nullopt;
}

DsdFilterSpec dsd_filter_for(const DsdRate& source, uint32_t fir_rate) {
    // The filter may not reach past what the output rate can carry.
    double f3 = source_f3_hz(source.multiple);
    f3 = std::min(f3, fir_rate * 0.4);

    DsdFilterSpec spec{};
    spec.f3_hz = f3;
    spec.pass_hz = kFlatBandHz;
    spec.stop_hz = kFlatBandHz + (f3 - kFlatBandHz) / kF3Position;
    spec.atten_db = kStopBandDb;
    return spec;
}

std::optional<DsdPcmPlan> plan_dsd_to_pcm(uint32_t dsd_rate, std::span<const uint32_t> device_rates) {
    const auto source = classify_dsd_rate(dsd_rate);
    if (!source) return std::nullopt;

    const uint32_t base = source->base;
    const uint32_t preferred = preferred_multiplier(source->multiple);

    // Walk down the family: integer ratios need no sample-rate conversion at all.
    for (uint32_t multiplier = preferred; multiplier >= kMinFirMultiplier; multiplier /= 2) {
        if (supports(device_rates, base * multiplier)) {
            return make_plan(*source, dsd_rate, base * multiplier, base * multiplier);
        }
    }
    if (supports(device_rates, base)) {
        return make_plan(*source, dsd_rate, fir_rate_for_target(base, preferred, base), base);
    }

    // Other family only: the best device rate that does not exceed the preferred one.
    uint32_t best = 0;
    for (uint32_t rate : device_rates) {
        if (rate <= base * preferred && rate > best) best = rate;
    }
    if (best == 0) {
        for (uint32_t rate : device_rates) {
            if (best == 0 || rate < best) best = rate;
        }
    }
    if (best == 0) return std::nullopt;
    return make_plan(*source, dsd_rate, fir_rate_for_target(base, preferred, best), best);
}

}  // namespace arpegio
