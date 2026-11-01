#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

#include "audio/quantizer.h"
#include "audio/volume.h"
#include "test_support.h"

using namespace arpegio;

namespace {

constexpr uint32_t kRate = 48000;
constexpr int kRampFrames = 960;  // 20 ms at that rate

constexpr WireEncoding kPcmEncodings[] = {WireEncoding::S16,        WireEncoding::S24,        WireEncoding::S32,
                                          WireEncoding::S32Right24, WireEncoding::S32Right20, WireEncoding::S32Right18,
                                          WireEncoding::S32Right16, WireEncoding::F32,        WireEncoding::F64};

DeviceFormat stereo(WireEncoding encoding) {
    return {encoding, kRate, 2, 0};
}

// Valid samples of every encoding: bytes that came out of the quantizer, at random levels.
std::vector<uint8_t> random_bytes(WireEncoding encoding, size_t frames) {
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> level(-0.9, 0.9);
    std::vector<double> samples(frames * 2);
    for (double& sample : samples) sample = level(rng);
    std::vector<uint8_t> bytes(samples.size() * wire_sample_bytes(encoding));
    Quantizer(encoding, 0).convert(samples.data(), samples.size(), bytes.data());
    return bytes;
}

std::vector<double> samples_of(const std::vector<uint8_t>& bytes, WireEncoding encoding) {
    std::vector<double> samples(bytes.size() / wire_sample_bytes(encoding));
    read_wire_samples(bytes.data(), encoding, samples.data(), samples.size());
    return samples;
}

void settle(Volume& volume, WireEncoding encoding) {
    std::vector<uint8_t> scratch(size_t{kRampFrames} * 2 * wire_sample_bytes(encoding), 0);
    volume.apply(scratch.data(), kRampFrames);
}

}  // namespace

TEST(wire_samples_read_back_exactly) {
    for (const WireEncoding encoding : kPcmEncodings) {
        const std::vector<uint8_t> bytes = random_bytes(encoding, 4096);
        const std::vector<double> samples = samples_of(bytes, encoding);
        std::vector<uint8_t> again(bytes.size());
        Quantizer(encoding, 0).convert(samples.data(), samples.size(), again.data());
        CHECK(again == bytes);
    }
}

TEST(wire_samples_keep_the_sign) {
    const int16_t s16 = -12345;
    double value = 0.0;
    read_wire_samples(reinterpret_cast<const uint8_t*>(&s16), WireEncoding::S16, &value, 1);
    CHECK(value == -12345 / 32768.0);

    const uint8_t s24[3] = {0xFF, 0xFF, 0xFF};
    read_wire_samples(s24, WireEncoding::S24, &value, 1);
    CHECK(value == -1 / 8388608.0);

    const int32_t right = -1;
    read_wire_samples(reinterpret_cast<const uint8_t*>(&right), WireEncoding::S32Right20, &value, 1);
    CHECK(value == -1 / 524288.0);
}

// This is what the bit-perfect path rests on: at unity the stage must not even read the samples.
TEST(volume_at_unity_touches_no_byte) {
    for (const WireEncoding encoding : kPcmEncodings) {
        const std::atomic<double> gain{1.0};
        Volume volume(stereo(encoding), 24, gain);
        const std::vector<uint8_t> original = random_bytes(encoding, 4096);
        std::vector<uint8_t> bytes = original;
        volume.apply(bytes.data(), 4096);
        CHECK(bytes == original);
    }
}

TEST(volume_at_unity_leaves_bytes_that_are_not_samples_alone) {
    // Not a value the quantizer writes back the same way, such as the bits below 24 in a Right24 word.
    const std::atomic<double> gain{1.0};
    Volume volume(stereo(WireEncoding::S32Right16), 24, gain);
    std::vector<uint8_t> bytes(8 * 1024);
    std::mt19937 rng(3);
    for (uint8_t& byte : bytes) byte = static_cast<uint8_t>(rng());
    const std::vector<uint8_t> original = bytes;
    volume.apply(bytes.data(), 1024);
    CHECK(bytes == original);
}

TEST(volume_scales_every_encoding) {
    for (const WireEncoding encoding : kPcmEncodings) {
        std::atomic<double> gain{1.0};
        Volume volume(stereo(encoding), 0, gain);
        gain = 0.25;
        settle(volume, encoding);

        const std::vector<uint8_t> original = random_bytes(encoding, 4096);
        std::vector<uint8_t> bytes = original;
        volume.apply(bytes.data(), 4096);

        const std::vector<double> before = samples_of(original, encoding);
        const std::vector<double> after = samples_of(bytes, encoding);
        const double tolerance = std::ldexp(1.0, -wire_resolution(encoding));
        for (size_t i = 0; i < before.size(); ++i) {
            if (std::abs(after[i] - before[i] * 0.25) > tolerance) {
                CHECK(false);
                break;
            }
        }
    }
}

TEST(volume_goes_back_to_exact_bytes_at_unity) {
    std::atomic<double> gain{1.0};
    Volume volume(stereo(WireEncoding::S32), 24, gain);
    gain = 0.5;
    settle(volume, WireEncoding::S32);
    gain = 1.0;
    settle(volume, WireEncoding::S32);

    const std::vector<uint8_t> original = random_bytes(WireEncoding::S32, 4096);
    std::vector<uint8_t> bytes = original;
    volume.apply(bytes.data(), 4096);
    CHECK(bytes == original);
}

TEST(volume_opens_at_the_level_it_was_left_at) {
    const std::atomic<double> gain{0.5};
    Volume volume(stereo(WireEncoding::S32), 0, gain);
    const double in = 0.5;
    std::vector<uint8_t> bytes(8);
    const double frame[2] = {in, in};
    Quantizer(WireEncoding::S32, 0).convert(frame, 2, bytes.data());
    volume.apply(bytes.data(), 1);
    CHECK_NEAR(samples_of(bytes, WireEncoding::S32)[0], 0.25, 1e-9);
}

TEST(volume_changes_without_a_step) {
    std::atomic<double> gain{1.0};
    Volume volume(stereo(WireEncoding::S32), 0, gain);
    std::vector<double> level(kRampFrames * 4, 0.8);
    std::vector<uint8_t> bytes(level.size() * 4);
    Quantizer(WireEncoding::S32, 0).convert(level.data(), level.size(), bytes.data());

    gain = 0.25;
    size_t done = 0;
    while (done < level.size() / 2) {  // blocks of the size a driver would ask for
        const size_t frames = std::min<size_t>(256, level.size() / 2 - done);
        volume.apply(bytes.data() + done * 8, frames);
        done += frames;
    }

    const std::vector<double> out = samples_of(bytes, WireEncoding::S32);
    const double largest_step = 0.8 * 0.75 / kRampFrames * 1.01;
    bool smooth = true;
    for (size_t frame = 1; frame < out.size() / 2 && smooth; ++frame) {
        smooth = std::abs(out[frame * 2] - out[(frame - 1) * 2]) <= largest_step && out[frame * 2] == out[frame * 2 + 1];
    }
    CHECK(smooth);
    CHECK_NEAR(out[0], 0.8, 0.001);
    CHECK_NEAR(out[size_t{kRampFrames} * 2], 0.2, 1e-9);
    CHECK_NEAR(out.back(), 0.2, 1e-9);
}

TEST(volume_at_zero_is_digital_silence) {
    std::atomic<double> gain{1.0};
    Volume volume(stereo(WireEncoding::S32), 24, gain);
    gain = 0.0;
    settle(volume, WireEncoding::S32);

    std::vector<uint8_t> bytes = random_bytes(WireEncoding::S32, 4096);
    volume.apply(bytes.data(), 4096);
    CHECK(std::all_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte == 0; }));
}

TEST(volume_below_unity_is_dithered_at_the_word_length_given) {
    std::atomic<double> gain{1.0};
    Volume volume(stereo(WireEncoding::S32), 24, gain);
    gain = 0.5;
    settle(volume, WireEncoding::S32);

    constexpr size_t kFrames = 1 << 16;
    std::vector<uint8_t> bytes(kFrames * 8, 0);  // silence stays at the dither's noise floor
    volume.apply(bytes.data(), kFrames);
    const std::vector<double> out = samples_of(bytes, WireEncoding::S32);

    double energy = 0.0;
    double largest = 0.0;
    for (const double sample : out) {
        energy += sample * sample;
        largest = std::max(largest, std::abs(sample));
    }
    const double lsb = std::ldexp(1.0, -23);
    const double rms = std::sqrt(energy / out.size());
    CHECK(largest <= lsb);                        // triangular: never beyond +/- 1 LSB
    CHECK_NEAR(rms / lsb, 1.0 / std::sqrt(6.0), 0.01);  // and the spread of that density
}

TEST(volume_at_the_end_of_a_ramp_lands_exactly_on_the_target) {
    // Adding the step up 960 times would leave a rounding error, and unity would never be reached.
    std::atomic<double> gain{1.0};
    Volume volume(stereo(WireEncoding::S32), 0, gain);
    for (const double level : {0.1, 1.0, 0.9999999, 1.0}) {
        gain = level;
        settle(volume, WireEncoding::S32);
    }
    const std::vector<uint8_t> original = random_bytes(WireEncoding::S32, 256);
    std::vector<uint8_t> bytes = original;
    volume.apply(bytes.data(), 256);
    CHECK(bytes == original);
}
