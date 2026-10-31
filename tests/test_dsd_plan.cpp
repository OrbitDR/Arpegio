#include <cmath>
#include <vector>

#include "dsd/dsd_pcm_plan.h"
#include "test_support.h"

using namespace arpegio;

namespace {

// Rates reported by the drivers on the development machine.
const std::vector<uint32_t> kTopping = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};
const std::vector<uint32_t> kMiniFuse = {44100, 48000, 88200, 96000, 176400, 192000};
const std::vector<uint32_t> kShared48 = {48000};
const std::vector<uint32_t> kShared44 = {44100};
const std::vector<uint32_t> kOnly48Family = {48000, 96000, 192000};

constexpr uint32_t kDsd64 = 2822400;
constexpr uint32_t kDsd128 = 5644800;
constexpr uint32_t kDsd256 = 11289600;
constexpr uint32_t kDsd512 = 22579200;

}  // namespace

TEST(classify_rates) {
    CHECK(classify_dsd_rate(kDsd64)->multiple == 64);
    CHECK(classify_dsd_rate(kDsd64)->base == 44100);
    CHECK(classify_dsd_rate(3072000)->base == 48000);
    CHECK(classify_dsd_rate(24576000)->multiple == 512);
    CHECK(!classify_dsd_rate(176400));
}

TEST(preferred_rates_without_restriction) {
    CHECK(plan_dsd_to_pcm(kDsd64, {})->output_rate == 176400);
    CHECK(plan_dsd_to_pcm(kDsd64, {})->divider == 16);
    CHECK(plan_dsd_to_pcm(kDsd128, {})->output_rate == 352800);
    CHECK(plan_dsd_to_pcm(kDsd128, {})->divider == 16);
    CHECK(plan_dsd_to_pcm(kDsd256, {})->divider == 32);
    CHECK(plan_dsd_to_pcm(kDsd512, {})->divider == 64);
    CHECK(plan_dsd_to_pcm(3072000, {})->output_rate == 192000);
    CHECK(plan_dsd_to_pcm(6144000, {})->output_rate == 384000);
    CHECK(plan_dsd_to_pcm(24576000, {})->output_rate == 384000);
}

TEST(topping_keeps_preferred_rates) {
    CHECK(plan_dsd_to_pcm(kDsd64, kTopping)->output_rate == 176400);
    CHECK(plan_dsd_to_pcm(kDsd512, kTopping)->output_rate == 352800);
    CHECK(!plan_dsd_to_pcm(kDsd512, kTopping)->needs_resampler);
}

TEST(minifuse_steps_down_to_1764) {
    for (uint32_t rate : {kDsd64, kDsd128, kDsd256, kDsd512}) {
        const auto plan = plan_dsd_to_pcm(rate, kMiniFuse);
        CHECK(plan->output_rate == 176400);
        CHECK(!plan->needs_resampler);
        CHECK(plan->divider * 176400ull == rate);
    }
}

TEST(windows_shared_mode_uses_the_resampler) {
    const auto plan = plan_dsd_to_pcm(kDsd64, kShared48);
    CHECK(plan->needs_resampler);
    CHECK(plan->fir_rate == 176400);
    CHECK(plan->output_rate == 48000);

    const auto same_family = plan_dsd_to_pcm(kDsd64, kShared44);
    CHECK(same_family->needs_resampler);
    CHECK(same_family->fir_rate == 176400);
    CHECK(same_family->output_rate == 44100);
}

TEST(other_family_only_device) {
    const auto plan = plan_dsd_to_pcm(kDsd128, kOnly48Family);
    CHECK(plan->needs_resampler);
    CHECK(plan->output_rate == 192000);
    CHECK(plan->fir_rate == 352800);
    CHECK(plan->divider == 16);
}

TEST(filter_table) {
    CHECK_NEAR(plan_dsd_to_pcm(kDsd64, {})->filter.f3_hz, 50000, 1e-6);
    CHECK_NEAR(plan_dsd_to_pcm(kDsd128, {})->filter.f3_hz, 70000, 1e-6);
    CHECK_NEAR(plan_dsd_to_pcm(kDsd256, {})->filter.f3_hz, 90000, 1e-6);
    CHECK_NEAR(plan_dsd_to_pcm(kDsd512, {})->filter.f3_hz, 90000, 1e-6);
    // Limited by the output rate.
    CHECK_NEAR(plan_dsd_to_pcm(kDsd128, kMiniFuse)->filter.f3_hz, 70000, 1e-6);
    CHECK_NEAR(plan_dsd_to_pcm(kDsd512, kMiniFuse)->filter.f3_hz, 70560, 1e-6);
    const std::vector<uint32_t> only_88 = {88200};
    CHECK_NEAR(plan_dsd_to_pcm(kDsd64, only_88)->filter.f3_hz, 35280, 1e-6);
}
