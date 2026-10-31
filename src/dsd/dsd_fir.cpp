#include "dsd/dsd_fir.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace arpegio {

namespace {

constexpr double kPi = std::numbers::pi;

// Modified Bessel function of order zero. The series converges fast for the betas used here.
double bessel_i0(double x) {
    const double half_sq = x * x * 0.25;
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k < 200; ++k) {
        term *= half_sq / (static_cast<double>(k) * k);
        sum += term;
        if (term < sum * 1e-18) break;
    }
    return sum;
}

double kaiser_beta(double atten_db) {
    return 0.1102 * (atten_db - 8.7);
}

std::vector<double> kaiser_window(size_t length, double beta) {
    std::vector<double> window(length);
    const double center = (static_cast<double>(length) - 1.0) * 0.5;
    const double i0_beta = bessel_i0(beta);
    for (size_t n = 0; n < length; ++n) {
        const double r = (static_cast<double>(n) - center) / (center + 0.5);
        window[n] = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0_beta;
    }
    return window;
}

std::vector<double> windowed_sinc(std::span<const double> window, double cutoff_hz, double rate) {
    std::vector<double> taps(window.size());
    const double center = (static_cast<double>(window.size()) - 1.0) * 0.5;
    const double wc = 2.0 * cutoff_hz / rate;
    double sum = 0.0;
    for (size_t n = 0; n < window.size(); ++n) {
        const double x = static_cast<double>(n) - center;
        const double sinc = x == 0.0 ? 1.0 : std::sin(kPi * wc * x) / (kPi * wc * x);
        taps[n] = sinc * window[n];
        sum += taps[n];
    }
    for (double& t : taps) t /= sum;
    return taps;
}

}  // namespace

double fir_magnitude(std::span<const double> taps, double sample_rate, double hz) {
    const double w = 2.0 * kPi * hz / sample_rate;
    double re = 0.0;
    double im = 0.0;
    for (size_t n = 0; n < taps.size(); ++n) {
        re += taps[n] * std::cos(w * static_cast<double>(n));
        im += taps[n] * std::sin(w * static_cast<double>(n));
    }
    return std::hypot(re, im);
}

std::vector<double> design_dsd_fir(double dsd_rate, const DsdFilterSpec& spec) {
    // The Kaiser length formula runs slightly short at this attenuation, hence the margin.
    constexpr double kTransitionMargin = 0.92;
    const double transition = kTransitionMargin * (spec.stop_hz - spec.pass_hz) / dsd_rate;
    const double estimate = (spec.atten_db - 7.95) / (14.36 * transition) + 1.0;
    const size_t length = (static_cast<size_t>(estimate) / 8 + 1) * 8;
    const auto window = kaiser_window(length, kaiser_beta(spec.atten_db));

    // Bisect the sinc cut-off so the -3 dB point lands exactly on f3.
    const double target = std::sqrt(0.5);
    double low = spec.pass_hz;
    double high = spec.stop_hz;
    for (int i = 0; i < 48; ++i) {
        const double mid = (low + high) * 0.5;
        const auto taps = windowed_sinc(window, mid, dsd_rate);
        if (fir_magnitude(taps, dsd_rate, spec.f3_hz) < target) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return windowed_sinc(window, (low + high) * 0.5, dsd_rate);
}

DsdToPcm::DsdToPcm(std::vector<double> taps, uint32_t divider, int channels)
    : taps_len_(taps.size()),
      divider_(divider),
      step_(divider / 8),
      groups_((taps.size() + 7) / 8),
      channels_(channels) {
    if (divider == 0 || divider % 8 != 0 || taps.empty() || channels <= 0) {
        throw std::invalid_argument("DsdToPcm: divider must be a multiple of 8");
    }
    taps.resize(groups_ * 8, 0.0);

    // table_[g * 256 + b] is the sum of 8 taps for byte b. Byte MSB is the oldest bit.
    table_.resize(groups_ * 256);
    for (size_t g = 0; g < groups_; ++g) {
        for (int b = 0; b < 256; ++b) {
            double sum = 0.0;
            for (int bit = 0; bit < 8; ++bit) {
                const double value = ((b >> bit) & 1) ? 1.0 : -1.0;
                sum += value * taps[g * 8 + bit];
            }
            table_[g * 256 + b] = sum;
        }
    }
    history_.assign(channels, std::vector<uint8_t>(groups_, kDsdSilenceByte));
}

void DsdToPcm::reset() {
    pending_ = 0;
    for (auto& channel : history_) std::fill(channel.begin(), channel.end(), kDsdSilenceByte);
}

double DsdToPcm::filter_one(const uint8_t* newest) const {
    double sum = 0.0;
    const double* row = table_.data();
    for (size_t g = 0; g < groups_; ++g, row += 256) {
        sum += row[*(newest - g)];
    }
    return sum;
}

size_t DsdToPcm::process(const uint8_t* const* planar, size_t bytes, double* interleaved) {
    if (bytes == 0) return 0;
    const size_t frames = frames_for(bytes);
    const size_t first = step_ - pending_;  // new bytes consumed by the first output frame

    std::vector<uint8_t> work(groups_ + bytes);
    for (int ch = 0; ch < channels_; ++ch) {
        std::copy(history_[ch].begin(), history_[ch].end(), work.begin());
        std::copy_n(planar[ch], bytes, work.begin() + groups_);

        double* out = interleaved + ch;
        for (size_t frame = 0; frame < frames; ++frame) {
            const size_t newest = groups_ - 1 + first + frame * step_;
            *out = filter_one(work.data() + newest);
            out += channels_;
        }
        std::copy(work.end() - groups_, work.end(), history_[ch].begin());
    }
    pending_ = (pending_ + bytes) % step_;
    return frames;
}

}  // namespace arpegio
