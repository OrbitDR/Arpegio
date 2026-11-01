#include "engine/resampler.h"

#include <CDSPResampler.h>
#include <soxr.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace arpegio {

namespace {

// Audirvana's stored r8brain profile: 99.5 % of Nyquist, 218 dB, linear phase.
constexpr double kTransitionBandPercent = 1.0;
constexpr double kStopBandDb = 218.0;
constexpr int kBlockFrames = 1 << 14;

class R8brainResampler final : public Resampler {
public:
    R8brainResampler(uint32_t from, uint32_t to, int channels) : channels_(channels), input_(kBlockFrames), output_(channels) {
        for (int ch = 0; ch < channels; ++ch) {
            filters_.push_back(std::make_unique<r8b::CDSPResampler>(static_cast<double>(from), static_cast<double>(to),
                                                                    kBlockFrames, kTransitionBandPercent, kStopBandDb,
                                                                    r8b::fprLinearPhase));
        }
    }

    void process(const double* interleaved, size_t frames, std::vector<double>& out) override {
        for (size_t offset = 0; offset < frames; offset += kBlockFrames) {
            const int block = static_cast<int>(std::min<size_t>(kBlockFrames, frames - offset));
            int produced = 0;
            for (int ch = 0; ch < channels_; ++ch) {
                if (interleaved) {
                    for (int i = 0; i < block; ++i) input_[i] = interleaved[(offset + i) * channels_ + ch];
                } else {
                    std::fill_n(input_.data(), block, 0.0);
                }
                double* result = nullptr;
                produced = filters_[ch]->process(input_.data(), block, result);
                output_[ch] = result;
            }
            const size_t base = out.size();
            out.resize(base + static_cast<size_t>(produced) * channels_);
            double* write = out.data() + base;
            for (int i = 0; i < produced; ++i) {
                for (int ch = 0; ch < channels_; ++ch) *write++ = output_[ch][i];
            }
        }
    }

    void flush(std::vector<double>& out) override {
        process(nullptr, static_cast<size_t>(filters_[0]->getInLenBeforeOutPos(0)), out);
    }

    void clear() override {
        for (auto& filter : filters_) filter->clear();
    }

private:
    int channels_;
    std::vector<double> input_;
    std::vector<double*> output_;
    std::vector<std::unique_ptr<r8b::CDSPResampler>> filters_;
};

class SoxrResampler final : public Resampler {
public:
    SoxrResampler(uint32_t from, uint32_t to, int channels) : channels_(channels), ratio_(static_cast<double>(to) / from) {
        // 32 bits is the most it offers, about 190 dB of rejection. Steep keeps the audible band as wide as
        // r8brain's, with a passband up to 99.5 % of Nyquist.
        const soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
        const soxr_quality_spec_t quality = soxr_quality_spec(SOXR_32_BITQ | SOXR_LINEAR_PHASE | SOXR_STEEP_FILTER, 0);
        const soxr_runtime_spec_t runtime = soxr_runtime_spec(1);
        soxr_error_t error = nullptr;
        handle_ = soxr_create(from, to, static_cast<unsigned>(channels), &error, &io, &quality, &runtime);
        if (error) throw std::runtime_error(std::string("resampler: ") + error);
    }

    ~SoxrResampler() override { soxr_delete(handle_); }

    void process(const double* interleaved, size_t frames, std::vector<double>& out) override {
        const size_t room = static_cast<size_t>(static_cast<double>(frames) * ratio_) + kSpareFrames;
        size_t done = 0;
        while (done < frames) {
            size_t used = 0;
            size_t made = 0;
            const size_t base = out.size();
            out.resize(base + room * channels_);
            check(soxr_process(handle_, interleaved + done * channels_, frames - done, &used, out.data() + base, room, &made));
            out.resize(base + made * channels_);
            done += used;
        }
    }

    void flush(std::vector<double>& out) override {
        size_t made = 0;
        do {
            const size_t base = out.size();
            out.resize(base + kSpareFrames * channels_);
            check(soxr_process(handle_, nullptr, 0, nullptr, out.data() + base, kSpareFrames, &made));
            out.resize(base + made * channels_);
        } while (made > 0);
    }

    void clear() override { check(soxr_clear(handle_)); }

private:
    static constexpr size_t kSpareFrames = 4096;

    static void check(soxr_error_t error) {
        if (error) throw std::runtime_error(std::string("resampler: ") + error);
    }

    int channels_;
    double ratio_;
    soxr_t handle_ = nullptr;
};

}  // namespace

std::unique_ptr<Resampler> make_resampler(ResamplerKind kind, uint32_t from, uint32_t to, int channels) {
    if (kind == ResamplerKind::Soxr) return std::make_unique<SoxrResampler>(from, to, channels);
    return std::make_unique<R8brainResampler>(from, to, channels);
}

}  // namespace arpegio
