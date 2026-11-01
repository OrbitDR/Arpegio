// Changes the sample rate of interleaved doubles. Runs on the loader thread, and neither implementation adds
// delay to what it returns: the first output frame lines up with the first input frame.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace arpegio {

enum class ResamplerKind { R8brain, Soxr };

constexpr const char* resampler_name(ResamplerKind kind) {
    return kind == ResamplerKind::Soxr ? "soxr" : "r8brain";
}

class Resampler {
public:
    virtual ~Resampler() = default;

    // Both append to `out`.
    virtual void process(const double* interleaved, size_t frames, std::vector<double>& out) = 0;
    virtual void flush(std::vector<double>& out) = 0;  // what the filters still hold, once the input has ended

    virtual void clear() = 0;  // forgets everything, as after a seek
};

// Throws std::runtime_error when the rates cannot be handled.
std::unique_ptr<Resampler> make_resampler(ResamplerKind kind, uint32_t from, uint32_t to, int channels);

}  // namespace arpegio
