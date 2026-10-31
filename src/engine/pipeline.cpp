#include "engine/pipeline.h"

#include <CDSPResampler.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace arpegio {

namespace {

constexpr uint32_t kChunksPerSecond = 4;

// Audirvana's stored r8brain profile: 99.5 % of Nyquist, 218 dB, linear phase.
constexpr double kTransitionBandPercent = 1.0;
constexpr double kStopBandDb = 218.0;
constexpr int kResamplerBlock = 1 << 14;

constexpr double kMinus3Db = 0.70710678118654752;

constexpr std::array<uint8_t, 256> make_bit_reverse() {
    std::array<uint8_t, 256> table{};
    for (int i = 0; i < 256; ++i) {
        int r = 0;
        for (int b = 0; b < 8; ++b) r |= ((i >> b) & 1) << (7 - b);
        table[i] = static_cast<uint8_t>(r);
    }
    return table;
}

constexpr auto kBitReverse = make_bit_reverse();

}  // namespace

class Resampler {
public:
    Resampler(uint32_t from, uint32_t to, int channels) : channels_(channels), input_(kResamplerBlock), output_(channels) {
        for (int ch = 0; ch < channels; ++ch) {
            filters_.push_back(std::make_unique<r8b::CDSPResampler>(static_cast<double>(from), static_cast<double>(to),
                                                                    kResamplerBlock, kTransitionBandPercent, kStopBandDb,
                                                                    r8b::fprLinearPhase));
        }
    }

    // Appends resampled interleaved frames to `out`.
    void process(const double* interleaved, size_t frames, std::vector<double>& out) {
        for (size_t offset = 0; offset < frames; offset += kResamplerBlock) {
            const int block = static_cast<int>(std::min<size_t>(kResamplerBlock, frames - offset));
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

    size_t tail_frames() const { return static_cast<size_t>(filters_[0]->getInLenBeforeOutPos(0)); }

    void clear() {
        for (auto& filter : filters_) filter->clear();
    }

private:
    int channels_;
    std::vector<double> input_;
    std::vector<double*> output_;
    std::vector<std::unique_ptr<r8b::CDSPResampler>> filters_;
};

Pipeline::Pipeline(const SourceFormat& source, const RenderPlan& plan) : source_(source), plan_(plan) {
    if (source.dsd) {
        chunk_ = source.rate / 8 / kChunksPerSecond;
        planes_.assign(source.channels, std::vector<uint8_t>(chunk_));
        for (auto& plane : planes_) plane_pointers_.push_back(plane.data());
    } else {
        chunk_ = source.rate / kChunksPerSecond;
        pcm_.resize(chunk_ * source.channels);
    }

    if (plan.path == RenderPath::DsdToPcm) {
        fir_ = std::make_unique<DsdToPcm>(design_dsd_fir(source.rate, plan.dsd.filter), plan.dsd.divider, source.channels);
        pcm_.resize((fir_->frames_for(chunk_) + 8) * source.channels);
        drop_frames_ = fir_->delay_frames();
    }
    if (plan.path == RenderPath::DsdDop) dop_ = std::make_unique<DopPacker>(plan.device.encoding, source.channels);
    if (plan.path == RenderPath::Pcm || plan.path == RenderPath::DsdToPcm) {
        quantizer_ = std::make_unique<Quantizer>(plan.device.encoding, plan.dither_bits);
    }
    if (plan.resample_from != 0) {
        resampler_ = std::make_unique<Resampler>(plan.resample_from, plan.device.rate, plan.device.channels);
    }
}

Pipeline::~Pipeline() = default;

void Pipeline::mix_channels(const double* samples, size_t frames) {
    const int in = source_.channels;
    const int out = plan_.device.channels;
    mixed_.assign(frames * out, 0.0);

    if (in == 1) {
        for (size_t i = 0; i < frames; ++i) {
            mixed_[i * out] = samples[i];
            if (out > 1) mixed_[i * out + 1] = samples[i];
        }
        return;
    }
    if (out >= in) {
        for (size_t i = 0; i < frames; ++i) std::copy_n(samples + i * in, in, mixed_.begin() + i * out);
        return;
    }

    // ITU-style fold-down to stereo, scaled so three full-scale channels cannot clip. LFE is
    // dropped. Layouts: 3 L R C; 4 L R Ls Rs; 5 L R C Ls Rs; 6 L R C LFE Ls Rs.
    const double scale = 1.0 / (1.0 + 2.0 * kMinus3Db);
    const int center = (in == 3 || in == 5 || in == 6) ? 2 : -1;
    const int back_left = in == 4 ? 2 : in == 5 ? 3 : in == 6 ? 4 : -1;
    for (size_t i = 0; i < frames; ++i) {
        const double* frame = samples + i * in;
        double left = frame[0];
        double right = frame[1];
        if (center >= 0) {
            left += kMinus3Db * frame[center];
            right += kMinus3Db * frame[center];
        }
        if (back_left >= 0) {
            left += kMinus3Db * frame[back_left];
            right += kMinus3Db * frame[back_left + 1];
        }
        mixed_[i * out] = left * scale;
        if (out > 1) mixed_[i * out + 1] = right * scale;
    }
}

void Pipeline::emit_pcm(double* samples, size_t frames, std::vector<uint8_t>& out) {
    const size_t skip = std::min(drop_frames_, frames);
    drop_frames_ -= skip;
    samples += skip * source_.channels;
    frames -= skip;
    if (frames == 0) return;

    double* current = samples;
    if (plan_.mix_channels) {
        mix_channels(samples, frames);
        current = mixed_.data();
    }

    const int channels = plan_.device.channels;
    std::vector<double> resampled;
    if (resampler_) {
        resampler_->process(current, frames, resampled);
        current = resampled.data();
        frames = resampled.size() / channels;
        if (frames == 0) return;
    }

    const size_t count = frames * channels;
    if (plan_.gain != 1.0) {
        for (size_t i = 0; i < count; ++i) current[i] *= plan_.gain;
    }
    const size_t base = out.size();
    out.resize(base + frames * plan_.device.frame_bytes());
    quantizer_->convert(current, count, out.data() + base);
}

bool Pipeline::produce(Decoder& decoder, std::vector<uint8_t>& out) {
    out.clear();
    if (!source_.dsd) {
        const size_t frames = decoder.read_pcm(pcm_.data(), chunk_);
        if (frames == 0) return false;
        emit_pcm(pcm_.data(), frames, out);
        return true;
    }

    const size_t bytes = decoder.read_dsd(plane_pointers_.data(), chunk_);
    if (bytes == 0) return false;
    const auto planes = reinterpret_cast<const uint8_t* const*>(plane_pointers_.data());
    const int channels = source_.channels;
    switch (plan_.path) {
    case RenderPath::DsdNative: {
        out.resize(bytes * channels);
        const bool reverse = plan_.device.encoding == WireEncoding::DsdLsb;
        for (int ch = 0; ch < channels; ++ch) {
            uint8_t* write = out.data() + ch;
            for (size_t i = 0; i < bytes; ++i, write += channels) {
                *write = reverse ? kBitReverse[planes[ch][i]] : planes[ch][i];
            }
        }
        break;
    }
    case RenderPath::DsdDop: {
        out.resize((bytes / 2 + 1) * plan_.device.frame_bytes());
        const size_t frames = dop_->pack(planes, bytes, out.data());
        out.resize(frames * plan_.device.frame_bytes());
        break;
    }
    default: {
        const size_t frames = fir_->process(planes, bytes, pcm_.data());
        emit_pcm(pcm_.data(), frames, out);
        break;
    }
    }
    return true;
}

void Pipeline::flush(std::vector<uint8_t>& out) {
    out.clear();
    if (fir_) {
        // The FIR still holds half its length: push silence through to get the tail out.
        const size_t bytes = std::min(fir_->delay_frames() * (plan_.dsd.divider / 8) + plan_.dsd.divider / 8, chunk_);
        for (auto& plane : planes_) std::fill_n(plane.begin(), bytes, kDsdSilenceByte);
        const auto planes = reinterpret_cast<const uint8_t* const*>(plane_pointers_.data());
        const size_t frames = fir_->process(planes, bytes, pcm_.data());
        emit_pcm(pcm_.data(), frames, out);
    }
    if (resampler_) {
        const int channels = plan_.device.channels;
        std::vector<double> tail;
        resampler_->process(nullptr, resampler_->tail_frames(), tail);
        if (!tail.empty() && plan_.gain != 1.0) {
            for (double& sample : tail) sample *= plan_.gain;
        }
        const size_t base = out.size();
        out.resize(base + tail.size() / channels * plan_.device.frame_bytes());
        quantizer_->convert(tail.data(), tail.size(), out.data() + base);
    }
}

void Pipeline::reset() {
    if (fir_) {
        fir_->reset();
        drop_frames_ = fir_->delay_frames();
    }
    if (dop_) dop_->reset();
    if (resampler_) resampler_->clear();
}

}  // namespace arpegio
