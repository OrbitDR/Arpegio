// Decides how one source reaches one device.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "audio/device_format.h"
#include "decode/decoder.h"
#include "dsd/dsd_pcm_plan.h"
#include "engine/resampler.h"
#include "output/output.h"

namespace arpegio {

enum class DsdPolicy { Auto, Native, Dop, Pcm };

struct PlaybackPolicy {
    DsdPolicy dsd = DsdPolicy::Auto;
    bool dop_confirmed = false;  // the user verified that this DAC decodes DoP
    double dsd_to_pcm_gain_db = 3.0;
    int dither_bits = 24;
    bool attenuated = false;  // the software volume is down, which DSD cannot follow while it stays DSD
    ResamplerKind resampler = ResamplerKind::R8brain;
};

enum class RenderPath { Pcm, DsdNative, DsdDop, DsdToPcm };

constexpr const char* render_path_name(RenderPath path) {
    switch (path) {
    case RenderPath::Pcm: return "PCM";
    case RenderPath::DsdNative: return "DSD native";
    case RenderPath::DsdDop: return "DoP";
    case RenderPath::DsdToPcm: return "DSD to PCM";
    }
    return "?";
}

struct RenderPlan {
    RenderPath path = RenderPath::Pcm;
    DeviceFormat device;
    bool bit_perfect = false;
    DsdPcmPlan dsd{};           // valid for DsdToPcm
    uint32_t resample_from = 0; // input rate of the resampler, 0 when not resampling
    ResamplerKind resampler = ResamplerKind::R8brain;
    bool mix_channels = false;  // source and device channel counts differ
    double gain = 1.0;
    int dither_bits = 0;
    int gain_dither_bits = 0;  // once a volume or a track gain rewrites the samples

    // DSD that stays DSD has no level to scale.
    bool accepts_gain() const { return path == RenderPath::Pcm || path == RenderPath::DsdToPcm; }

    bool operator==(const RenderPlan& other) const {
        return path == other.path && device == other.device && resample_from == other.resample_from &&
               resampler == other.resampler && mix_channels == other.mix_channels && gain == other.gain &&
               dsd.divider == other.dsd.divider;
    }
};

// Throws std::runtime_error when the source cannot reach this device.
RenderPlan make_render_plan(const SourceFormat& source, const DeviceInfo& device, const PlaybackPolicy& policy);

std::string describe_plan(const RenderPlan& plan, const SourceFormat& source);

}  // namespace arpegio
