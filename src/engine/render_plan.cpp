#include "engine/render_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace arpegio {

namespace {

constexpr uint32_t kDopDivider = 16;
constexpr double kNeutralGainDb = 0.01;

// A hardware volume away from 0 dB can rescale samples after they leave the program, which voids
// bit-perfect output and hides the DoP markers from the DAC.
bool volume_is_neutral(const DeviceInfo& device) {
    return std::abs(device.hardware_gain_db) < kNeutralGainDb;
}

const PcmMode* find_mode(const DeviceInfo& device, uint32_t rate) {
    for (const PcmMode& mode : device.pcm_modes) {
        if (mode.rate == rate) return &mode;
    }
    return nullptr;
}

bool can_carry_dop(const PcmMode* mode) {
    return mode && (mode->encoding == WireEncoding::S32 || mode->encoding == WireEncoding::S24);
}

int output_channels(const DeviceInfo& device, int source_channels) {
    // WASAPI shared must match the Windows mix format exactly.
    if (device.kind == OutputKind::WasapiShared) return device.channels;
    return std::min(source_channels, device.channels);
}

// For a PCM rate the device lacks: an integer multiple of the source first, then the nearest
// rate above, then the highest it has.
const PcmMode& pick_pcm_mode(const DeviceInfo& device, uint32_t rate) {
    const PcmMode* best = nullptr;
    for (const PcmMode& mode : device.pcm_modes) {
        if (mode.rate > rate && mode.rate % rate == 0 && (!best || mode.rate < best->rate)) best = &mode;
    }
    if (best) return *best;
    for (const PcmMode& mode : device.pcm_modes) {
        if (mode.rate > rate && (!best || mode.rate < best->rate)) best = &mode;
    }
    if (best) return *best;
    for (const PcmMode& mode : device.pcm_modes) {
        if (!best || mode.rate > best->rate) best = &mode;
    }
    return *best;
}

DeviceFormat pcm_device_format(const PcmMode& mode, int channels) {
    DeviceFormat format;
    format.encoding = mode.encoding;
    format.rate = mode.rate;
    format.channels = channels;
    format.valid_bits = mode.valid_bits;
    return format;
}

RenderPlan plan_pcm(const SourceFormat& source, const DeviceInfo& device, const PlaybackPolicy& policy) {
    if (device.pcm_modes.empty()) throw std::runtime_error("the device reports no PCM format");
    const PcmMode* exact = find_mode(device, source.rate);
    const PcmMode& mode = exact ? *exact : pick_pcm_mode(device, source.rate);

    RenderPlan plan;
    plan.path = RenderPath::Pcm;
    plan.device = pcm_device_format(mode, output_channels(device, source.channels));
    plan.resample_from = exact ? 0 : source.rate;
    plan.mix_channels = plan.device.channels != source.channels;
    const bool reduces_depth = wire_resolution(mode.encoding) < source.bits;
    // Mono copied to both channels keeps every sample, so it stays bit-perfect.
    const bool mono_copy = source.channels == 1 && plan.device.channels == 2;
    const bool processed = plan.resample_from != 0 || (plan.mix_channels && !mono_copy);
    plan.bit_perfect = !processed && !reduces_depth && device.kind != OutputKind::WasapiShared && volume_is_neutral(device);
    plan.gain_dither_bits = std::min(policy.dither_bits, wire_resolution(mode.encoding));
    plan.dither_bits = (processed || reduces_depth) ? plan.gain_dither_bits : 0;
    return plan;
}

RenderPlan plan_dsd_native(const SourceFormat& source) {
    RenderPlan plan;
    plan.path = RenderPath::DsdNative;
    plan.device.encoding = WireEncoding::DsdMsb;
    plan.device.rate = source.rate;
    plan.device.channels = source.channels;
    plan.device.valid_bits = 1;
    plan.bit_perfect = true;
    return plan;
}

RenderPlan plan_dop(const SourceFormat& source, const DeviceInfo& device, const PcmMode& mode) {
    RenderPlan plan;
    plan.path = RenderPath::DsdDop;
    plan.device = pcm_device_format(mode, source.channels);
    plan.bit_perfect = volume_is_neutral(device);
    return plan;
}

RenderPlan plan_conversion(const SourceFormat& source, const DeviceInfo& device, const PlaybackPolicy& policy) {
    std::vector<uint32_t> rates;
    for (const PcmMode& mode : device.pcm_modes) rates.push_back(mode.rate);
    const auto dsd_plan = plan_dsd_to_pcm(source.rate, rates);
    if (!dsd_plan) throw std::runtime_error("no PCM rate available for this DSD stream");

    const PcmMode* mode = find_mode(device, dsd_plan->output_rate);
    RenderPlan plan;
    plan.path = RenderPath::DsdToPcm;
    plan.device = pcm_device_format(*mode, output_channels(device, source.channels));
    plan.dsd = *dsd_plan;
    plan.resample_from = dsd_plan->needs_resampler ? dsd_plan->fir_rate : 0;
    plan.mix_channels = plan.device.channels != source.channels;
    plan.gain = std::pow(10.0, policy.dsd_to_pcm_gain_db / 20.0);
    plan.dither_bits = std::min(policy.dither_bits, wire_resolution(mode->encoding));
    plan.gain_dither_bits = plan.dither_bits;
    return plan;
}

}  // namespace

RenderPlan make_render_plan(const SourceFormat& source, const DeviceInfo& device, const PlaybackPolicy& policy) {
    if (!source.dsd) return plan_pcm(source, device, policy);

    // Otherwise a DSD track after attenuated PCM would jump to full level, whatever mode was chosen.
    if (policy.attenuated) return plan_conversion(source, device, policy);

    const bool channels_fit = source.channels <= device.channels;
    const bool native_ok = device.kind == OutputKind::Asio && channels_fit &&
                           std::find(device.dsd_rates.begin(), device.dsd_rates.end(), source.rate) != device.dsd_rates.end();
    const PcmMode* dop_mode = source.rate % kDopDivider == 0 ? find_mode(device, source.rate / kDopDivider) : nullptr;
    const bool dop_ok = device.kind != OutputKind::WasapiShared && channels_fit && can_carry_dop(dop_mode);

    switch (policy.dsd) {
    case DsdPolicy::Native:
        if (!native_ok) throw std::runtime_error("this output cannot take this DSD rate natively");
        return plan_dsd_native(source);
    case DsdPolicy::Dop:
        if (!dop_ok) throw std::runtime_error("this output cannot carry this DSD rate as DoP");
        return plan_dop(source, device, *dop_mode);
    case DsdPolicy::Pcm:
        return plan_conversion(source, device, policy);
    case DsdPolicy::Auto:
        break;
    }
    if (native_ok) return plan_dsd_native(source);
    if (dop_ok && policy.dop_confirmed && volume_is_neutral(device)) return plan_dop(source, device, *dop_mode);
    return plan_conversion(source, device, policy);
}

std::string describe_plan(const RenderPlan& plan, const SourceFormat& source) {
    char text[256];
    const char* quality = plan.bit_perfect ? "bit-perfect" : "processed";
    std::snprintf(text, sizeof(text), "%s %u Hz %d ch -> %s | device %s %u Hz %d ch%s%s | %s",
                  source.dsd ? "DSD" : "PCM", source.rate, source.channels, render_path_name(plan.path),
                  wire_name(plan.device.encoding), plan.device.rate, plan.device.channels,
                  plan.resample_from ? " | resampled" : "", plan.mix_channels ? " | channels mixed" : "", quality);
    return text;
}

}  // namespace arpegio
