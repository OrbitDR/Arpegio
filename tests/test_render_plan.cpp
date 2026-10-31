#include <vector>

#include "engine/render_plan.h"
#include "test_support.h"

using namespace arpegio;

namespace {

constexpr uint32_t kDsd64 = 2822400;
constexpr uint32_t kDsd128 = 5644800;
constexpr uint32_t kDsd256 = 11289600;
constexpr uint32_t kDsd512 = 22579200;

std::vector<PcmMode> s32_modes(std::vector<uint32_t> rates) {
    std::vector<PcmMode> modes;
    for (uint32_t rate : rates) modes.push_back({rate, WireEncoding::S32, 32});
    return modes;
}

DeviceInfo topping_asio() {
    DeviceInfo device;
    device.kind = OutputKind::Asio;
    device.channels = 2;
    device.pcm_modes = s32_modes({44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000});
    device.dsd_rates = {2822400, 3072000, 5644800, 6144000, 11289600, 12288000};
    return device;
}

DeviceInfo topping_exclusive() {
    DeviceInfo device = topping_asio();
    device.kind = OutputKind::WasapiExclusive;
    device.dsd_rates.clear();
    return device;
}

DeviceInfo minifuse_asio() {
    DeviceInfo device;
    device.kind = OutputKind::Asio;
    device.channels = 8;
    device.pcm_modes = s32_modes({44100, 48000, 88200, 96000, 176400, 192000});
    return device;
}

DeviceInfo shared_48k() {
    DeviceInfo device;
    device.kind = OutputKind::WasapiShared;
    device.channels = 2;
    device.pcm_modes = {{48000, WireEncoding::F32, 32}};
    return device;
}

SourceFormat dsd(uint32_t rate, int channels = 2) {
    return {true, rate, channels, 1, 1000000};
}

SourceFormat pcm(uint32_t rate, int bits = 24, int channels = 2) {
    return {false, rate, channels, bits, 1000000};
}

}  // namespace

TEST(topping_plays_dsd_natively_through_dsd256) {
    for (uint32_t rate : {kDsd64, kDsd128, kDsd256}) {
        const auto plan = make_render_plan(dsd(rate), topping_asio(), {});
        CHECK(plan.path == RenderPath::DsdNative);
        CHECK(plan.bit_perfect);
    }
}

TEST(topping_converts_dsd512_to_3528) {
    const auto plan = make_render_plan(dsd(kDsd512), topping_asio(), {});
    CHECK(plan.path == RenderPath::DsdToPcm);
    CHECK(plan.device.rate == 352800);
    CHECK(!plan.bit_perfect);
}

TEST(exclusive_needs_dop_confirmation) {
    CHECK(make_render_plan(dsd(kDsd64), topping_exclusive(), {}).path == RenderPath::DsdToPcm);
    PlaybackPolicy policy;
    policy.dop_confirmed = true;
    const auto plan = make_render_plan(dsd(kDsd64), topping_exclusive(), policy);
    CHECK(plan.path == RenderPath::DsdDop);
    CHECK(plan.device.rate == 176400);
    CHECK(plan.device.encoding == WireEncoding::S32);
}

TEST(dop_stops_where_the_pcm_rates_stop) {
    PlaybackPolicy policy;
    policy.dop_confirmed = true;
    CHECK(make_render_plan(dsd(kDsd128), topping_exclusive(), policy).path == RenderPath::DsdDop);
    CHECK(make_render_plan(dsd(kDsd256), topping_exclusive(), policy).path == RenderPath::DsdToPcm);
}

TEST(minifuse_always_converts_to_1764) {
    for (uint32_t rate : {kDsd64, kDsd128, kDsd256, kDsd512}) {
        const auto plan = make_render_plan(dsd(rate), minifuse_asio(), {});
        CHECK(plan.path == RenderPath::DsdToPcm);
        CHECK(plan.device.rate == 176400);
        CHECK(plan.resample_from == 0);
    }
}

TEST(shared_converts_and_resamples) {
    const auto plan = make_render_plan(dsd(kDsd128), shared_48k(), {});
    CHECK(plan.path == RenderPath::DsdToPcm);
    CHECK(plan.device.rate == 48000);
    CHECK(plan.resample_from == 176400);
    CHECK(plan.device.encoding == WireEncoding::F32);
}

TEST(forced_policies_fail_loudly) {
    PlaybackPolicy native;
    native.dsd = DsdPolicy::Native;
    bool thrown = false;
    try {
        make_render_plan(dsd(kDsd64), minifuse_asio(), native);
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    CHECK(thrown);
}

TEST(multichannel_dsd_on_stereo_device_converts_and_mixes) {
    const auto plan = make_render_plan(dsd(kDsd64, 6), topping_asio(), {});
    CHECK(plan.path == RenderPath::DsdToPcm);
    CHECK(plan.device.channels == 2);
    CHECK(plan.mix_channels);
}

TEST(pcm_matching_rate_is_bit_perfect) {
    const auto plan = make_render_plan(pcm(96000), topping_asio(), {});
    CHECK(plan.bit_perfect);
    CHECK(plan.device.rate == 96000);
    CHECK(plan.dither_bits == 0);
    CHECK(plan.gain == 1.0);
}

TEST(pcm_shared_is_never_bit_perfect) {
    const auto plan = make_render_plan(pcm(44100), shared_48k(), {});
    CHECK(!plan.bit_perfect);
    CHECK(plan.resample_from == 44100);
}

TEST(pcm_rate_above_device_goes_to_highest) {
    const auto plan = make_render_plan(pcm(768000), minifuse_asio(), {});
    CHECK(plan.device.rate == 192000);
    CHECK(plan.resample_from == 768000);
}

TEST(pcm_prefers_integer_multiple) {
    DeviceInfo device = minifuse_asio();
    device.pcm_modes = s32_modes({48000, 88200, 96000});
    const auto plan = make_render_plan(pcm(44100), device, {});
    CHECK(plan.device.rate == 88200);
}

TEST(mono_on_stereo_stays_bit_perfect) {
    const auto plan = make_render_plan(pcm(44100, 16, 1), topping_asio(), {});
    CHECK(plan.device.channels == 1);
    CHECK(plan.bit_perfect);
}
