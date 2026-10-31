#include "decode/framed_dsd.h"

#include <algorithm>
#include <cstring>

#include "dsd/dsd_fir.h"

namespace arpegio {

void FramedDsdDecoder::set_stream(uint32_t rate, int channels, uint64_t total_frames) {
    dst_ = std::make_unique<DstDecoder>(channels, rate);
    frame_bytes_ = dst_->frame_bytes();
    format_.dsd = true;
    format_.rate = rate;
    format_.channels = channels;
    format_.bits = 1;
    format_.length = total_frames * frame_bytes_;
    frame_.resize(frame_bytes_ * channels);
    position_ = frame_bytes_;  // nothing loaded yet
}

bool FramedDsdDecoder::load_frame() {
    bool compressed = false;
    if (!next_frame(raw_, compressed)) return false;
    const size_t total = frame_bytes_ * format_.channels;
    bool good = true;
    if (compressed) {
        good = dst_->decode(raw_.data(), raw_.size(), frame_.data());
    } else if (raw_.size() >= total) {
        std::memcpy(frame_.data(), raw_.data(), total);
    } else {
        good = false;
    }
    if (!good) {
        // A damaged frame becomes silence: the stream keeps its length and timing.
        std::memset(frame_.data(), kDsdSilenceByte, total);
        ++damaged_frames_;
    }
    position_ = 0;
    return true;
}

size_t FramedDsdDecoder::read_dsd(uint8_t* const* planar, size_t bytes) {
    const int channels = format_.channels;
    size_t done = 0;
    while (done < bytes) {
        if (position_ >= frame_bytes_ && !load_frame()) break;
        if (skip_ > 0) {
            const size_t drop = std::min(skip_, frame_bytes_ - position_);
            position_ += drop;
            skip_ -= drop;
            continue;
        }
        const size_t count = std::min(bytes - done, frame_bytes_ - position_);
        for (int ch = 0; ch < channels; ++ch) {
            const uint8_t* in = frame_.data() + position_ * channels + ch;
            uint8_t* out = planar[ch] + done;
            for (size_t i = 0; i < count; ++i, in += channels) out[i] = *in;
        }
        position_ += count;
        done += count;
    }
    return done;
}

void FramedDsdDecoder::seek(uint64_t position) {
    seek_frame(position / frame_bytes_);
    position_ = frame_bytes_;
    skip_ = static_cast<size_t>(position % frame_bytes_);
}

}  // namespace arpegio
