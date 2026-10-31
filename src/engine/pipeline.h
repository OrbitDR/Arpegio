// Turns decoder output into device bytes. Runs on the loader thread, never in the callback.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "audio/quantizer.h"
#include "decode/decoder.h"
#include "dsd/dop.h"
#include "dsd/dsd_fir.h"
#include "engine/render_plan.h"

namespace arpegio {

class Resampler;

class Pipeline {
public:
    Pipeline(const SourceFormat& source, const RenderPlan& plan);
    ~Pipeline();

    // Decodes one chunk and replaces `out` with device bytes, which may be empty while filters
    // fill up. Returns false once the decoder has nothing left.
    bool produce(Decoder& decoder, std::vector<uint8_t>& out);

    // Replaces `out` with what the filters still hold. Call after the last track of a stream.
    void flush(std::vector<uint8_t>& out);

    // Forgets filter state, as after a seek.
    void reset();

    uint64_t clipped() const { return quantizer_ ? quantizer_->clipped() : 0; }

private:
    void emit_pcm(double* samples, size_t frames, std::vector<uint8_t>& out);
    void mix_channels(const double* samples, size_t frames);

    SourceFormat source_;
    RenderPlan plan_;
    size_t chunk_ = 0;  // frames, or bytes per channel for DSD
    size_t drop_frames_ = 0;

    std::vector<std::vector<uint8_t>> planes_;
    std::vector<uint8_t*> plane_pointers_;
    std::vector<double> pcm_;
    std::vector<double> mixed_;

    std::unique_ptr<DsdToPcm> fir_;
    std::unique_ptr<DopPacker> dop_;
    std::unique_ptr<Quantizer> quantizer_;
    std::unique_ptr<Resampler> resampler_;
};

}  // namespace arpegio
