#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <tuple>
#include <utility>
#include <vector>

#include "engine/resampler.h"
#include "test_support.h"

using namespace arpegio;

namespace {

constexpr ResamplerKind kKinds[] = {ResamplerKind::R8brain, ResamplerKind::Soxr};
constexpr std::pair<uint32_t, uint32_t> kRates[] = {{44100, 48000}, {44100, 96000}, {48000, 44100},
                                                    {96000, 44100}, {192000, 48000}, {44100, 352800}};

// How far the output must be from the ends for the filters to have settled.
constexpr double kEdgeSeconds = 0.25;

std::vector<double> tone(double frequency, uint32_t rate, double seconds, double level, double phase = 0.0) {
    const size_t frames = static_cast<size_t>(seconds * rate);
    std::vector<double> out(frames * 2);
    const double step = 2.0 * std::numbers::pi * frequency / rate;
    for (size_t i = 0; i < frames; ++i) {
        const double value = level * std::sin(phase + step * static_cast<double>(i));
        out[i * 2] = value;
        out[i * 2 + 1] = -value;
    }
    return out;
}

std::vector<double> resample(ResamplerKind kind, uint32_t from, uint32_t to, const std::vector<double>& input) {
    const auto resampler = make_resampler(kind, from, to, 2);
    std::vector<double> out;
    resampler->process(input.data(), input.size() / 2, out);
    resampler->flush(out);
    return out;
}

struct Fit {
    double amplitude;
    double phase;      // radians, at frame 0
    double residual;   // what the sine does not explain, in dB below it
};

// Least-squares sine of a known frequency over the left channel, away from the edges.
Fit fit_sine(const std::vector<double>& stereo, double frequency, uint32_t rate) {
    const size_t frames = stereo.size() / 2;
    const size_t edge = static_cast<size_t>(kEdgeSeconds * rate);
    const double step = 2.0 * std::numbers::pi * frequency / rate;
    double ss = 0.0, cc = 0.0, sc = 0.0, sy = 0.0, cy = 0.0;
    for (size_t i = edge; i + edge < frames; ++i) {
        const double s = std::sin(step * static_cast<double>(i));
        const double c = std::cos(step * static_cast<double>(i));
        const double y = stereo[i * 2];
        ss += s * s;
        cc += c * c;
        sc += s * c;
        sy += s * y;
        cy += c * y;
    }
    const double determinant = ss * cc - sc * sc;
    const double a = (sy * cc - cy * sc) / determinant;
    const double b = (cy * ss - sy * sc) / determinant;
    double error = 0.0;
    double signal = 0.0;
    for (size_t i = edge; i + edge < frames; ++i) {
        const double fitted = a * std::sin(step * static_cast<double>(i)) + b * std::cos(step * static_cast<double>(i));
        const double difference = stereo[i * 2] - fitted;
        error += difference * difference;
        signal += fitted * fitted;
    }
    return {std::hypot(a, b), std::atan2(b, a), 10.0 * std::log10(std::max(error, 1e-300) / signal)};
}

double rms_db(const std::vector<double>& stereo, uint32_t rate) {
    const size_t frames = stereo.size() / 2;
    const size_t edge = static_cast<size_t>(kEdgeSeconds * rate);
    double energy = 0.0;
    for (size_t i = edge; i + edge < frames; ++i) energy += stereo[i * 2] * stereo[i * 2];
    return 10.0 * std::log10(std::max(energy / static_cast<double>(frames - 2 * edge), 1e-300));
}

}  // namespace

// What Audirvana's r8brain profile and libsoxr at 32 bits promise, measured through this integration.
TEST(both_resamplers_keep_a_tone_clean_and_in_time) {
    constexpr double kTone = 997.0;
    constexpr double kLevel = std::numbers::sqrt2 / 2.0;
    for (const ResamplerKind kind : kKinds) {
        double worst = -400.0;
        for (const auto [from, to] : kRates) {
            const std::vector<double> out = resample(kind, from, to, tone(kTone, from, 2.0, kLevel));
            const Fit fit = fit_sine(out, kTone, to);
            CHECK_NEAR(fit.amplitude / kLevel, 1.0, 1e-5);
            // In samples of the output: a delay would shift the phase, and a gap between tracks with it.
            const double offset = std::remainder(fit.phase, 2.0 * std::numbers::pi) / (2.0 * std::numbers::pi * kTone / to);
            CHECK_NEAR(offset, 0.0, 0.01);
            worst = std::max(worst, fit.residual);
        }
        std::printf("  %s: residual of a 997 Hz tone at most %.1f dB\n", resampler_name(kind), worst);
        CHECK(worst < -170.0);
    }
}

TEST(both_resamplers_keep_the_top_of_the_audible_band) {
    constexpr double kLevel = 0.5;
    for (const ResamplerKind kind : kKinds) {
        for (const auto [from, to] : kRates) {
            const double tone_hz = 20000.0;
            const std::vector<double> out = resample(kind, from, to, tone(tone_hz, from, 2.0, kLevel));
            const double level_db = 20.0 * std::log10(fit_sine(out, tone_hz, to).amplitude / kLevel);
            CHECK_NEAR(level_db, 0.0, 0.01);
        }
    }
}

TEST(both_resamplers_reject_what_does_not_fit_in_the_lower_rate) {
    for (const ResamplerKind kind : kKinds) {
        double worst = -400.0;
        for (const auto [from, to, tone_hz] : {std::tuple{96000u, 44100u, 30000.0}, std::tuple{192000u, 48000u, 60000.0},
                                               std::tuple{48000u, 44100u, 23000.0}}) {
            const double level = rms_db(resample(kind, from, to, tone(tone_hz, from, 2.0, 0.9)), to);
            worst = std::max(worst, level);
        }
        std::printf("  %s: strongest alias %.1f dBFS\n", resampler_name(kind), worst);
        CHECK(worst < -170.0);
    }
}

TEST(both_resamplers_keep_the_length) {
    for (const ResamplerKind kind : kKinds) {
        for (const auto [from, to] : kRates) {
            const std::vector<double> input = tone(1000.0, from, 1.5, 0.5);
            const size_t frames = resample(kind, from, to, input).size() / 2;
            const double expected = static_cast<double>(input.size() / 2) * to / from;
            // The end of the last track only: between tracks the filters stay full, so nothing is lost there.
            CHECK(frames + 1 >= static_cast<size_t>(expected));
            CHECK(static_cast<double>(frames) <= expected + std::max(2.0, to * 0.001));
        }
    }
}

TEST(both_resamplers_give_the_same_samples_whatever_the_block_size) {
    for (const ResamplerKind kind : kKinds) {
        const std::vector<double> input = tone(1000.0, 44100, 1.0, 0.5);
        const std::vector<double> whole = resample(kind, 44100, 96000, input);

        const auto resampler = make_resampler(kind, 44100, 96000, 2);
        std::vector<double> pieces;
        size_t at = 0;
        size_t size = 1;
        while (at < input.size() / 2) {
            const size_t frames = std::min(size, input.size() / 2 - at);
            resampler->process(input.data() + at * 2, frames, pieces);
            at += frames;
            size = size * 3 % 9973 + 1;
        }
        resampler->flush(pieces);

        CHECK(pieces.size() == whole.size());
        double largest = 0.0;
        for (size_t i = 0; i < std::min(pieces.size(), whole.size()); ++i) {
            largest = std::max(largest, std::abs(pieces[i] - whole[i]));
        }
        CHECK(largest < 1e-9);
    }
}

TEST(both_resamplers_start_clean_after_a_clear) {
    for (const ResamplerKind kind : kKinds) {
        const std::vector<double> input = tone(1000.0, 44100, 1.0, 0.5);
        const std::vector<double> first = resample(kind, 44100, 48000, input);

        const auto resampler = make_resampler(kind, 44100, 48000, 2);
        std::vector<double> ignored;
        const std::vector<double> other = tone(3000.0, 44100, 0.5, 0.9);
        resampler->process(other.data(), other.size() / 2, ignored);
        resampler->clear();

        std::vector<double> again;
        resampler->process(input.data(), input.size() / 2, again);
        resampler->flush(again);
        CHECK(again.size() == first.size());
        double largest = 0.0;
        for (size_t i = 0; i < std::min(again.size(), first.size()); ++i) {
            largest = std::max(largest, std::abs(again[i] - first[i]));
        }
        CHECK(largest < 1e-9);
    }
}

TEST(both_resamplers_keep_the_channels_apart) {
    for (const ResamplerKind kind : kKinds) {
        std::vector<double> input = tone(1000.0, 44100, 1.0, 0.5);
        for (size_t i = 1; i < input.size(); i += 2) input[i] = 0.0;
        const std::vector<double> out = resample(kind, 44100, 96000, input);
        double right = 0.0;
        for (size_t i = 1; i < out.size(); i += 2) right = std::max(right, std::abs(out[i]));
        CHECK(right == 0.0);
    }
}
