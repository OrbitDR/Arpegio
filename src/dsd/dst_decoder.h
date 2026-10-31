// Direct Stream Transfer decoder, the lossless compression used by SACD and DSDIFF.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace arpegio {

class DstDecoder {
public:
    DstDecoder(int channels, uint32_t dsd_rate);
    ~DstDecoder();

    // Bytes per channel in one frame. A frame lasts 1/75 s.
    size_t frame_bytes() const { return frame_bytes_; }

    // Decodes one frame into interleaved MSB-first bytes: frame_bytes() * channels of them.
    // Returns false when the frame is damaged or uses an unsupported layout.
    bool decode(const uint8_t* data, size_t size, uint8_t* interleaved);

private:
    struct State;
    std::unique_ptr<State> state_;
    int channels_;
    size_t frame_bytes_;
};

}  // namespace arpegio
