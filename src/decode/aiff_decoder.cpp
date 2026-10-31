// AIFF and AIFF-C: big-endian PCM, plus the 'sowt', 'fl32' and 'fl64' compression types.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "decode/decoder.h"

namespace arpegio {

namespace {

uint32_t be32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

uint32_t be16(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 8 | p[1];
}

// 80-bit extended float, the rate field of COMM.
uint32_t extended_to_rate(const uint8_t* p) {
    const int exponent = static_cast<int>(be16(p) & 0x7FFF) - 16383 - 63;
    uint64_t mantissa = 0;
    for (int i = 0; i < 8; ++i) mantissa = (mantissa << 8) | p[2 + i];
    return static_cast<uint32_t>(std::ldexp(static_cast<double>(mantissa), exponent) + 0.5);
}

class AiffDecoder final : public Decoder {
public:
    explicit AiffDecoder(const std::filesystem::path& path) {
        file_.reset(_wfopen(path.c_str(), L"rb"));
        if (!file_) throw std::runtime_error("AIFF: cannot open file");
        uint8_t form[12];
        if (std::fread(form, 1, 12, file_.get()) != 12 || std::memcmp(form, "FORM", 4) != 0) {
            throw std::runtime_error("AIFF: bad header");
        }
        const bool compressed_form = std::memcmp(form + 8, "AIFC", 4) == 0;
        if (!compressed_form && std::memcmp(form + 8, "AIFF", 4) != 0) throw std::runtime_error("AIFF: bad header");

        for (;;) {
            uint8_t chunk[8];
            if (std::fread(chunk, 1, 8, file_.get()) != 8) break;
            const uint64_t size = be32(chunk + 4);
            const int64_t next = _ftelli64(file_.get()) + static_cast<int64_t>(size + (size & 1));
            if (std::memcmp(chunk, "COMM", 4) == 0) {
                uint8_t comm[64] = {};
                std::fread(comm, 1, std::min<uint64_t>(size, sizeof(comm)), file_.get());
                format_.channels = static_cast<int>(be16(comm));
                format_.length = be32(comm + 2);
                format_.bits = static_cast<int>(be16(comm + 6));
                format_.rate = extended_to_rate(comm + 8);
                if (compressed_form) {
                    little_endian_ = std::memcmp(comm + 18, "sowt", 4) == 0;
                    is_float_ = std::memcmp(comm + 18, "fl32", 4) == 0 || std::memcmp(comm + 18, "FL32", 4) == 0 ||
                                std::memcmp(comm + 18, "fl64", 4) == 0 || std::memcmp(comm + 18, "FL64", 4) == 0;
                    const bool plain = std::memcmp(comm + 18, "NONE", 4) == 0 || std::memcmp(comm + 18, "twos", 4) == 0;
                    if (!little_endian_ && !is_float_ && !plain) throw std::runtime_error("AIFF: unsupported compression");
                }
            } else if (std::memcmp(chunk, "SSND", 4) == 0) {
                uint8_t ssnd[8];
                std::fread(ssnd, 1, 8, file_.get());
                data_offset_ = _ftelli64(file_.get()) + be32(ssnd);
            }
            _fseeki64(file_.get(), next, SEEK_SET);
        }
        sample_bytes_ = (format_.bits + 7) / 8;
        block_align_ = sample_bytes_ * format_.channels;
        if (data_offset_ == 0 || format_.rate == 0 || block_align_ == 0 || sample_bytes_ > 8) {
            throw std::runtime_error("AIFF: missing stream information");
        }
        _fseeki64(file_.get(), data_offset_, SEEK_SET);
    }

    const SourceFormat& format() const override { return format_; }

    size_t read_pcm(double* interleaved, size_t frames) override {
        frames = static_cast<size_t>(std::min<uint64_t>(frames, format_.length - position_));
        raw_.resize(frames * block_align_);
        const size_t got = std::fread(raw_.data(), block_align_, frames, file_.get());
        const size_t samples = got * format_.channels;
        const uint8_t* p = raw_.data();
        for (size_t i = 0; i < samples; ++i, p += sample_bytes_) {
            uint64_t value = 0;
            for (size_t b = 0; b < sample_bytes_; ++b) {
                const size_t index = little_endian_ ? sample_bytes_ - 1 - b : b;
                value = (value << 8) | p[index];
            }
            if (is_float_ && sample_bytes_ == 4) {
                interleaved[i] = std::bit_cast<float>(static_cast<uint32_t>(value));
            } else if (is_float_) {
                interleaved[i] = std::bit_cast<double>(value);
            } else {
                const auto aligned = static_cast<int64_t>(value << (64 - sample_bytes_ * 8));
                interleaved[i] = static_cast<double>(aligned >> 32) / 2147483648.0;
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
    struct FileCloser {
        void operator()(std::FILE* f) const { std::fclose(f); }
    };

    std::unique_ptr<std::FILE, FileCloser> file_;
    SourceFormat format_;
    int64_t data_offset_ = 0;
    uint64_t position_ = 0;
    size_t sample_bytes_ = 0;
    size_t block_align_ = 0;
    bool little_endian_ = false;
    bool is_float_ = false;
    std::vector<uint8_t> raw_;
};

}  // namespace

std::unique_ptr<Decoder> open_aiff_decoder(const std::filesystem::path& path) {
    return std::make_unique<AiffDecoder>(path);
}

}  // namespace arpegio
