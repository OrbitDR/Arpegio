// MP3 through minimp3, which honours the LAME gapless information.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3_ex.h"

#include <memory>
#include <stdexcept>
#include <vector>

#include "decode/decoder.h"

namespace arpegio {

namespace {

class Mp3Decoder final : public Decoder {
public:
    explicit Mp3Decoder(const std::filesystem::path& path) {
        if (mp3dec_ex_open_w(&decoder_, path.c_str(), MP3D_SEEK_TO_SAMPLE) != 0) {
            throw std::runtime_error("MP3: cannot open file");
        }
        format_.rate = static_cast<uint32_t>(decoder_.info.hz);
        format_.channels = decoder_.info.channels;
        format_.bits = 16;
        format_.length = decoder_.samples / decoder_.info.channels;
    }

    ~Mp3Decoder() override { mp3dec_ex_close(&decoder_); }

    const SourceFormat& format() const override { return format_; }

    size_t read_pcm(double* interleaved, size_t frames) override {
        const size_t samples = frames * format_.channels;
        buffer_.resize(samples);
        const size_t got = mp3dec_ex_read(&decoder_, buffer_.data(), samples);
        for (size_t i = 0; i < got; ++i) interleaved[i] = buffer_[i];
        return got / format_.channels;
    }

    void seek(uint64_t position) override { mp3dec_ex_seek(&decoder_, position * format_.channels); }

private:
    mp3dec_ex_t decoder_{};
    SourceFormat format_;
    std::vector<mp3d_sample_t> buffer_;
};

}  // namespace

std::unique_ptr<Decoder> open_mp3_decoder(const std::filesystem::path& path) {
    return std::make_unique<Mp3Decoder>(path);
}

}  // namespace arpegio
