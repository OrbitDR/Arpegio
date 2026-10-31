#include "formats/wav_writer.h"

#include <cstring>
#include <stdexcept>

namespace arpegio {

namespace {

constexpr size_t kHeaderSize = 44;

void put16(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t* p, uint32_t v) {
    put16(p, v & 0xffff);
    put16(p + 2, v >> 16);
}

}  // namespace

WavWriter::WavWriter(const std::filesystem::path& path, uint32_t sample_rate, int channels)
    : sample_rate_(sample_rate), channels_(channels) {
    file_.reset(_wfopen(path.c_str(), L"wb"));
    if (!file_) throw std::runtime_error("WAV: cannot create file");
    uint8_t blank[kHeaderSize] = {};
    std::fwrite(blank, 1, sizeof(blank), file_.get());
}

WavWriter::~WavWriter() {
    if (!file_) return;
    const uint32_t bytes_per_frame = 8u * channels_;
    const uint64_t data_bytes = frames_ * bytes_per_frame;

    uint8_t h[kHeaderSize] = {};
    std::memcpy(h, "RIFF", 4);
    put32(h + 4, static_cast<uint32_t>(data_bytes + kHeaderSize - 8));
    std::memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 3);  // IEEE float
    put16(h + 22, channels_);
    put32(h + 24, sample_rate_);
    put32(h + 28, sample_rate_ * bytes_per_frame);
    put16(h + 32, bytes_per_frame);
    put16(h + 34, 64);
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, static_cast<uint32_t>(data_bytes));

    std::fseek(file_.get(), 0, SEEK_SET);
    std::fwrite(h, 1, sizeof(h), file_.get());
}

void WavWriter::write(const double* interleaved, size_t frames) {
    std::fwrite(interleaved, sizeof(double) * channels_, frames, file_.get());
    frames_ += frames;
}

}  // namespace arpegio
