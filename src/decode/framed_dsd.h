// Base for DSD sources stored as 1/75 s frames, compressed with DST or not.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "decode/decoder.h"
#include "dsd/dst_decoder.h"

namespace arpegio {

class FramedDsdDecoder : public Decoder {
public:
    const SourceFormat& format() const override { return format_; }
    size_t read_dsd(uint8_t* const* planar, size_t bytes) override;
    void seek(uint64_t position) override;

    uint64_t damaged_frames() const { return damaged_frames_; }

    // The next frame as stored, for checking the decoder against another implementation.
    bool read_raw_frame(std::vector<uint8_t>& data, bool& compressed) { return next_frame(data, compressed); }

protected:
    // Call from the derived constructor once the stream layout is known.
    void set_stream(uint32_t rate, int channels, uint64_t total_frames);

    // Delivers the next frame's raw bytes. `compressed` tells whether they are DST.
    virtual bool next_frame(std::vector<uint8_t>& data, bool& compressed) = 0;
    virtual void seek_frame(uint64_t frame) = 0;

private:
    bool load_frame();

    SourceFormat format_;
    std::unique_ptr<DstDecoder> dst_;
    size_t frame_bytes_ = 0;
    std::vector<uint8_t> raw_;
    std::vector<uint8_t> frame_;  // interleaved
    size_t position_ = 0;         // bytes per channel consumed from frame_
    size_t skip_ = 0;             // bytes per channel to drop after a seek
    uint64_t damaged_frames_ = 0;
};

}  // namespace arpegio
