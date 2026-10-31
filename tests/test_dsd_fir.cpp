#include <cmath>
#include <cstdlib>
#include <random>
#include <vector>

#include "dsd/dsd_fir.h"
#include "test_support.h"

using namespace arpegio;

namespace {

double to_db(double magnitude) {
    return 20.0 * std::log10(magnitude);
}

// Direct form over individual bits, the reference for the byte-table implementation.
std::vector<double> brute_force(const std::vector<uint8_t>& bytes, const std::vector<double>& taps, uint32_t divider) {
    std::vector<int> bits;
    const size_t groups = (taps.size() + 7) / 8;
    for (size_t i = 0; i < groups; ++i) {
        for (int b = 7; b >= 0; --b) bits.push_back(((kDsdSilenceByte >> b) & 1) ? 1 : -1);
    }
    for (uint8_t byte : bytes) {
        for (int b = 7; b >= 0; --b) bits.push_back(((byte >> b) & 1) ? 1 : -1);
    }
    std::vector<double> out;
    for (size_t bit = groups * 8 + divider; bit <= bits.size(); bit += divider) {
        double sum = 0.0;
        for (size_t k = 0; k < taps.size(); ++k) sum += taps[k] * bits[bit - 1 - k];
        out.push_back(sum);
    }
    return out;
}

}  // namespace

TEST(design_meets_the_table_for_every_case) {
    for (uint32_t rate : {2822400u, 5644800u, 11289600u, 22579200u, 3072000u, 24576000u}) {
        const auto plan = plan_dsd_to_pcm(rate, {});
        const auto taps = design_dsd_fir(rate, plan->filter);
        CHECK(taps.size() % 8 == 0);
        CHECK_NEAR(to_db(fir_magnitude(taps, rate, 1000.0)), 0.0, 1e-7);
        CHECK_NEAR(to_db(fir_magnitude(taps, rate, 20000.0)), 0.0, 1e-4);
        CHECK_NEAR(to_db(fir_magnitude(taps, rate, plan->filter.f3_hz)), -3.0103, 0.001);
        for (double f = plan->filter.stop_hz; f < rate * 0.5; f += plan->filter.stop_hz * 0.0371) {
            CHECK(to_db(fir_magnitude(taps, rate, f)) < -155.0);
        }
    }
}

TEST(design_is_symmetric_with_unity_gain) {
    const auto plan = plan_dsd_to_pcm(2822400, {});
    const auto taps = design_dsd_fir(2822400, plan->filter);
    double sum = 0.0;
    for (size_t i = 0; i < taps.size(); ++i) {
        CHECK_NEAR(taps[i], taps[taps.size() - 1 - i], 1e-15);
        sum += taps[i];
    }
    CHECK_NEAR(sum, 1.0, 1e-13);
}

TEST(byte_tables_match_direct_form) {
    const auto plan = plan_dsd_to_pcm(2822400, {});
    const auto taps = design_dsd_fir(2822400, plan->filter);

    std::mt19937 rng(1234);
    std::vector<uint8_t> bytes(4000);
    for (auto& b : bytes) b = static_cast<uint8_t>(rng());

    DsdToPcm converter(taps, plan->divider, 1);
    std::vector<double> out(converter.frames_for(bytes.size()));
    const uint8_t* plane = bytes.data();
    const size_t frames = converter.process(&plane, bytes.size(), out.data());

    const auto expected = brute_force(bytes, taps, plan->divider);
    CHECK(frames == expected.size());
    for (size_t i = 0; i < frames; ++i) CHECK_NEAR(out[i], expected[i], 1e-12);
}

TEST(chunk_size_does_not_change_the_output) {
    const auto taps = design_dsd_fir(5644800, plan_dsd_to_pcm(5644800, {})->filter);
    std::mt19937 rng(99);
    std::vector<uint8_t> left(5000), right(5000);
    for (auto& b : left) b = static_cast<uint8_t>(rng());
    for (auto& b : right) b = static_cast<uint8_t>(rng());

    DsdToPcm whole(taps, 16, 2);
    std::vector<double> reference(whole.frames_for(left.size()) * 2);
    const uint8_t* planes[2] = {left.data(), right.data()};
    const size_t reference_frames = whole.process(planes, left.size(), reference.data());

    DsdToPcm pieces(taps, 16, 2);
    std::vector<double> joined;
    size_t pos = 0;
    for (size_t size : {1u, 3u, 2u, 7u, 1024u, 511u, 2u}) {
        for (int repeat = 0; repeat < 4 && pos + size <= left.size(); ++repeat) {
            const uint8_t* p[2] = {left.data() + pos, right.data() + pos};
            std::vector<double> chunk(pieces.frames_for(size) * 2);
            const size_t frames = pieces.process(p, size, chunk.data());
            joined.insert(joined.end(), chunk.begin(), chunk.begin() + frames * 2);
            pos += size;
        }
    }
    const uint8_t* rest[2] = {left.data() + pos, right.data() + pos};
    std::vector<double> chunk(pieces.frames_for(left.size() - pos) * 2);
    const size_t frames = pieces.process(rest, left.size() - pos, chunk.data());
    joined.insert(joined.end(), chunk.begin(), chunk.begin() + frames * 2);

    CHECK(joined.size() == reference_frames * 2);
    for (size_t i = 0; i < joined.size() && i < reference.size(); ++i) CHECK_NEAR(joined[i], reference[i], 1e-15);
}

TEST(all_ones_is_plus_one_and_silence_is_near_zero) {
    const auto taps = design_dsd_fir(2822400, plan_dsd_to_pcm(2822400, {})->filter);
    DsdToPcm converter(taps, 16, 1);

    std::vector<uint8_t> ones(4096, 0xff);
    std::vector<double> out(converter.frames_for(ones.size()));
    const uint8_t* plane = ones.data();
    const size_t frames = converter.process(&plane, ones.size(), out.data());
    CHECK_NEAR(out[frames - 1], 1.0, 1e-12);

    std::vector<uint8_t> silence(4096, kDsdSilenceByte);
    out.assign(converter.frames_for(silence.size()), 1.0);
    plane = silence.data();
    const size_t silent = converter.process(&plane, silence.size(), out.data());
    CHECK(std::abs(out[silent - 1]) < 1e-5);
}
