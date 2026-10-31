// FLAC and Ogg FLAC through the reference libFLAC, with its MD5 check of the decoded audio.
#include <FLAC/stream_decoder.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "decode/decoder.h"

namespace arpegio {

namespace {

class FlacDecoder final : public Decoder {
public:
    FlacDecoder(const std::filesystem::path& path, bool ogg) {
        decoder_ = FLAC__stream_decoder_new();
        if (!decoder_) throw std::runtime_error("FLAC: cannot create decoder");
        FLAC__stream_decoder_set_md5_checking(decoder_, false);

        file_ = _wfopen(path.c_str(), L"rb");
        if (!file_) {
            FLAC__stream_decoder_delete(decoder_);
            throw std::runtime_error("FLAC: cannot open file");
        }
        // libFLAC takes ownership of the FILE and closes it on finish.
        const auto status = ogg ? FLAC__stream_decoder_init_ogg_FILE(decoder_, file_, &on_write, &on_metadata, &on_error, this)
                                : FLAC__stream_decoder_init_FILE(decoder_, file_, &on_write, &on_metadata, &on_error, this);
        if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK ||
            !FLAC__stream_decoder_process_until_end_of_metadata(decoder_) || format_.rate == 0) {
            FLAC__stream_decoder_finish(decoder_);
            FLAC__stream_decoder_delete(decoder_);
            throw std::runtime_error("FLAC: cannot read stream header");
        }
        if (format_.length == 0) measure_length();
    }

    ~FlacDecoder() override {
        FLAC__stream_decoder_finish(decoder_);
        FLAC__stream_decoder_delete(decoder_);
    }

    const SourceFormat& format() const override { return format_; }

    size_t read_pcm(double* interleaved, size_t frames) override {
        size_t done = 0;
        while (done < frames) {
            const size_t available = (pending_.size() - pending_pos_) / format_.channels;
            if (available == 0) {
                pending_.clear();
                pending_pos_ = 0;
                if (FLAC__stream_decoder_get_state(decoder_) == FLAC__STREAM_DECODER_END_OF_STREAM) break;
                if (!FLAC__stream_decoder_process_single(decoder_)) break;
                continue;
            }
            const size_t take = std::min(available, frames - done);
            std::memcpy(interleaved + done * format_.channels, pending_.data() + pending_pos_,
                        take * format_.channels * sizeof(double));
            pending_pos_ += take * format_.channels;
            done += take;
        }
        return done;
    }

    void seek(uint64_t position) override {
        pending_.clear();
        pending_pos_ = 0;
        if (FLAC__stream_decoder_get_state(decoder_) == FLAC__STREAM_DECODER_END_OF_STREAM) {
            FLAC__stream_decoder_flush(decoder_);
        }
        if (position >= format_.length) position = format_.length ? format_.length - 1 : 0;
        if (!FLAC__stream_decoder_seek_absolute(decoder_, position)) {
            FLAC__stream_decoder_flush(decoder_);
            FLAC__stream_decoder_reset(decoder_);
        }
    }

private:
    // Encoders writing to a pipe leave the total out of the header. Duration and seeking need it, and
    // decoding is the only way to get it.
    void measure_length() {
        counting_ = true;
        FLAC__stream_decoder_process_until_end_of_stream(decoder_);
        counting_ = false;
        FLAC__stream_decoder_reset(decoder_);
        FLAC__stream_decoder_process_until_end_of_metadata(decoder_);
        format_.length = counted_;
    }

    static FLAC__StreamDecoderWriteStatus on_write(const FLAC__StreamDecoder*, const FLAC__Frame* frame,
                                                   const FLAC__int32* const buffer[], void* context) {
        auto* self = static_cast<FlacDecoder*>(context);
        const unsigned blocksize = frame->header.blocksize;
        const unsigned channels = frame->header.channels;
        if (self->counting_) {
            self->counted_ += blocksize;
            return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
        }
        if (static_cast<int>(channels) != self->format_.channels) return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        const double scale = 1.0 / static_cast<double>(1ull << (frame->header.bits_per_sample - 1));
        const size_t base = self->pending_.size();
        self->pending_.resize(base + static_cast<size_t>(blocksize) * channels);
        double* out = self->pending_.data() + base;
        for (unsigned i = 0; i < blocksize; ++i) {
            for (unsigned ch = 0; ch < channels; ++ch) *out++ = buffer[ch][i] * scale;
        }
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    static void on_metadata(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* metadata, void* context) {
        if (metadata->type != FLAC__METADATA_TYPE_STREAMINFO) return;
        auto* self = static_cast<FlacDecoder*>(context);
        const auto& info = metadata->data.stream_info;
        self->format_.rate = info.sample_rate;
        self->format_.channels = static_cast<int>(info.channels);
        self->format_.bits = static_cast<int>(info.bits_per_sample);
        self->format_.length = info.total_samples;
    }

    static void on_error(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void*) {}

    FLAC__StreamDecoder* decoder_ = nullptr;
    std::FILE* file_ = nullptr;
    SourceFormat format_;
    std::vector<double> pending_;
    size_t pending_pos_ = 0;
    bool counting_ = false;
    uint64_t counted_ = 0;
};

}  // namespace

std::unique_ptr<Decoder> open_flac_decoder(const std::filesystem::path& path, bool ogg) {
    return std::make_unique<FlacDecoder>(path, ogg);
}

}  // namespace arpegio
