// WAV and RF64: PCM 8/16/24/32 and IEEE float 32/64, plain or extensible.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "decode/decoder.h"

namespace arpegio {

namespace {

constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatFloat = 3;
constexpr uint16_t kFormatExtensible = 0xFFFE;

uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t le64(const uint8_t* p) {
    return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32);
}

class WavDecoder final : public Decoder {
public:
    explicit WavDecoder(const std::filesystem::path& path) {
        file_.reset(_wfopen(path.c_str(), L"rb"));
        if (!file_) throw std::runtime_error("WAV: cannot open file");

        uint8_t riff[12];
        if (std::fread(riff, 1, 12, file_.get()) != 12 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
            throw std::runtime_error("WAV: not a RIFF/WAVE file");
        }
        const bool rf64 = std::memcmp(riff, "RF64", 4) == 0;
        if (!rf64 && std::memcmp(riff, "RIFF", 4) != 0) throw std::runtime_error("WAV: not a RIFF/WAVE file");

        uint64_t rf64_data_size = 0;
        bool have_format = false;
        for (;;) {
            uint8_t chunk[8];
            if (std::fread(chunk, 1, 8, file_.get()) != 8) break;
            uint64_t size = le32(chunk + 4);
            if (std::memcmp(chunk, "ds64", 4) == 0) {
                uint8_t ds64[28];
                if (std::fread(ds64, 1, 28, file_.get()) != 28) throw std::runtime_error("WAV: truncated ds64");
                rf64_data_size = le64(ds64 + 8);
                _fseeki64(file_.get(), static_cast<int64_t>(size + (size & 1)) - 28, SEEK_CUR);
                continue;
            }
            if (std::memcmp(chunk, "fmt ", 4) == 0) {
                read_format(size);
                have_format = true;
                continue;
            }
            if (std::memcmp(chunk, "data", 4) == 0) {
                if (!have_format) throw std::runtime_error("WAV: data before fmt");
                if (rf64 && size == 0xFFFFFFFF) size = rf64_data_size;
                data_offset_ = _ftelli64(file_.get());
                format_.length = size / block_align_;
                break;
            }
            _fseeki64(file_.get(), static_cast<int64_t>(size + (size & 1)), SEEK_CUR);
        }
        if (data_offset_ == 0) throw std::runtime_error("WAV: no data chunk");
        raw_.resize(1 << 16);
    }

    const SourceFormat& format() const override { return format_; }

    size_t read_pcm(double* interleaved, size_t frames) override {
        const uint64_t remaining = format_.length - position_;
        frames = static_cast<size_t>(std::min<uint64_t>(frames, remaining));
        if (raw_.size() < frames * block_align_) raw_.resize(frames * block_align_);
        const size_t got = std::fread(raw_.data(), block_align_, frames, file_.get());
        const size_t samples = got * format_.channels;
        const uint8_t* p = raw_.data();
        const size_t step = sample_bytes_;
        if (is_float_ && step == 8) {
            std::memcpy(interleaved, p, samples * 8);
        } else if (is_float_) {
            for (size_t i = 0; i < samples; ++i, p += 4) {
                float value;
                std::memcpy(&value, p, 4);
                interleaved[i] = value;
            }
        } else if (step == 1) {
            for (size_t i = 0; i < samples; ++i) interleaved[i] = (static_cast<int>(p[i]) - 128) / 128.0;
        } else if (step == 2) {
            for (size_t i = 0; i < samples; ++i, p += 2) {
                interleaved[i] = static_cast<int16_t>(le16(p)) / 32768.0;
            }
        } else if (step == 3) {
            for (size_t i = 0; i < samples; ++i, p += 3) {
                const int32_t value = static_cast<int32_t>(static_cast<uint32_t>(p[0]) << 8 |
                                                           static_cast<uint32_t>(p[1]) << 16 |
                                                           static_cast<uint32_t>(p[2]) << 24);
                interleaved[i] = value / 2147483648.0;
            }
        } else {
            for (size_t i = 0; i < samples; ++i, p += 4) {
                interleaved[i] = static_cast<int32_t>(le32(p)) / 2147483648.0;
            }
        }
        position_ += got;
        return got;
    }

    void seek(uint64_t position) override {
        position_ = std::min(position, format_.length);
        _fseeki64(file_.get(), static_cast<int64_t>(data_offset_ + position_ * block_align_), SEEK_SET);
    }

private:
    void read_format(uint64_t size) {
        if (size < 16 || size > 256) throw std::runtime_error("WAV: bad fmt chunk");
        uint8_t fmt[256];
        const size_t padded = static_cast<size_t>(size + (size & 1));
        if (std::fread(fmt, 1, padded, file_.get()) != padded) throw std::runtime_error("WAV: truncated fmt chunk");

        uint16_t tag = le16(fmt);
        format_.channels = le16(fmt + 2);
        format_.rate = le32(fmt + 4);
        block_align_ = le16(fmt + 12);
        const int container_bits = le16(fmt + 14);
        format_.bits = container_bits;
        if (tag == kFormatExtensible && size >= 40) {
            const int valid = le16(fmt + 18);
            if (valid > 0 && valid <= container_bits) format_.bits = valid;
            tag = le16(fmt + 24);  // first two bytes of the SubFormat GUID
        }
        is_float_ = tag == kFormatFloat;
        sample_bytes_ = (container_bits + 7) / 8;
        const bool known_pcm = tag == kFormatPcm && sample_bytes_ >= 1 && sample_bytes_ <= 4;
        const bool known_float = is_float_ && (container_bits == 32 || container_bits == 64);
        if ((!known_pcm && !known_float) || format_.channels < 1 ||
            block_align_ != sample_bytes_ * format_.channels) {
            throw std::runtime_error("WAV: unsupported format");
        }
    }

    struct FileCloser {
        void operator()(std::FILE* f) const { std::fclose(f); }
    };

    std::unique_ptr<std::FILE, FileCloser> file_;
    SourceFormat format_;
    uint64_t data_offset_ = 0;
    uint64_t position_ = 0;
    size_t block_align_ = 0;
    size_t sample_bytes_ = 0;
    bool is_float_ = false;
    std::vector<uint8_t> raw_;
};

}  // namespace

std::unique_ptr<Decoder> open_wav_decoder(const std::filesystem::path& path) {
    return std::make_unique<WavDecoder>(path);
}

}  // namespace arpegio
