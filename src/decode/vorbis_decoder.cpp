// Ogg Vorbis through libvorbisfile.
#include <vorbis/vorbisfile.h>

#include <cstdio>
#include <memory>
#include <stdexcept>

#include "decode/decoder.h"

namespace arpegio {

namespace {

size_t read_callback(void* buffer, size_t size, size_t count, void* file) {
    return std::fread(buffer, size, count, static_cast<std::FILE*>(file));
}

int seek_callback(void* file, ogg_int64_t offset, int origin) {
    return _fseeki64(static_cast<std::FILE*>(file), offset, origin);
}

int close_callback(void* file) {
    return std::fclose(static_cast<std::FILE*>(file));
}

long tell_callback(void* file) {
    return static_cast<long>(_ftelli64(static_cast<std::FILE*>(file)));
}

class VorbisDecoder final : public Decoder {
public:
    explicit VorbisDecoder(const std::filesystem::path& path) {
        std::FILE* file = _wfopen(path.c_str(), L"rb");
        if (!file) throw std::runtime_error("Vorbis: cannot open file");
        const ov_callbacks callbacks = {read_callback, seek_callback, close_callback, tell_callback};
        if (ov_open_callbacks(file, &vorbis_, nullptr, 0, callbacks) != 0) {
            std::fclose(file);
            throw std::runtime_error("Vorbis: not a Vorbis stream");
        }
        const vorbis_info* info = ov_info(&vorbis_, -1);
        format_.rate = static_cast<uint32_t>(info->rate);
        format_.channels = info->channels;
        format_.bits = 16;
        format_.length = static_cast<uint64_t>(ov_pcm_total(&vorbis_, -1));
    }

    ~VorbisDecoder() override { ov_clear(&vorbis_); }

    const SourceFormat& format() const override { return format_; }

    size_t read_pcm(double* interleaved, size_t frames) override {
        const int channels = format_.channels;
        size_t done = 0;
        while (done < frames) {
            float** pcm = nullptr;
            int section = 0;
            const long got = ov_read_float(&vorbis_, &pcm, static_cast<int>(frames - done), &section);
            if (got == OV_HOLE) continue;
            if (got <= 0) break;
            for (int ch = 0; ch < channels; ++ch) {
                double* out = interleaved + done * channels + ch;
                for (long i = 0; i < got; ++i, out += channels) *out = pcm[ch][i];
            }
            done += static_cast<size_t>(got);
        }
        return done;
    }

    void seek(uint64_t position) override { ov_pcm_seek(&vorbis_, static_cast<ogg_int64_t>(position)); }

private:
    OggVorbis_File vorbis_{};
    SourceFormat format_;
};

}  // namespace

std::unique_ptr<Decoder> open_vorbis_decoder(const std::filesystem::path& path) {
    return std::make_unique<VorbisDecoder>(path);
}

}  // namespace arpegio
