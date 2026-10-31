#include "formats/dsf_reader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace arpegio {

namespace {

template <typename T>
T read_le(const uint8_t* p) {
    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) value |= static_cast<T>(p[i]) << (8 * i);
    return value;
}

constexpr std::array<uint8_t, 256> make_bit_reverse() {
    std::array<uint8_t, 256> table{};
    for (int i = 0; i < 256; ++i) {
        int r = 0;
        for (int b = 0; b < 8; ++b) r |= ((i >> b) & 1) << (7 - b);
        table[i] = static_cast<uint8_t>(r);
    }
    return table;
}

constexpr auto kBitReverse = make_bit_reverse();

}  // namespace

DsfReader::DsfReader(const std::filesystem::path& path) {
    file_.reset(_wfopen(path.c_str(), L"rb"));
    if (!file_) throw std::runtime_error("DSF: cannot open file");

    uint8_t head[28 + 52 + 12];
    if (std::fread(head, 1, sizeof(head), file_.get()) != sizeof(head)) {
        throw std::runtime_error("DSF: file too short");
    }
    if (std::memcmp(head, "DSD ", 4) != 0 || std::memcmp(head + 28, "fmt ", 4) != 0) {
        throw std::runtime_error("DSF: bad header");
    }
    const uint8_t* fmt = head + 28;
    const uint64_t fmt_size = read_le<uint64_t>(fmt + 4);
    if (fmt_size != 52 || std::memcmp(head + 80, "data", 4) != 0) {
        throw std::runtime_error("DSF: unexpected chunk layout");
    }
    channels_ = static_cast<int>(read_le<uint32_t>(fmt + 24));
    sample_rate_ = read_le<uint32_t>(fmt + 28);
    bits_per_sample_ = read_le<uint32_t>(fmt + 32);
    sample_count_ = read_le<uint64_t>(fmt + 36);
    block_size_ = read_le<uint32_t>(fmt + 44);
    data_offset_ = 28 + 52 + 12;

    if (channels_ < 1 || channels_ > 6 || block_size_ == 0 ||
        (bits_per_sample_ != 1 && bits_per_sample_ != 8)) {
        throw std::runtime_error("DSF: unsupported format");
    }
    block_.resize(static_cast<size_t>(block_size_) * channels_);
}

bool DsfReader::refill() {
    const size_t got = std::fread(block_.data(), 1, block_.size(), file_.get());
    block_pos_ = 0;
    block_len_ = got / channels_;
    if (bits_per_sample_ == 1) {
        for (size_t i = 0; i < got; ++i) block_[i] = kBitReverse[block_[i]];
    }
    return block_len_ > 0;
}

size_t DsfReader::read(uint8_t* const* planar, size_t bytes_per_channel) {
    const uint64_t remaining = total_bytes_per_channel() - position_;
    size_t want = static_cast<size_t>(std::min<uint64_t>(bytes_per_channel, remaining));
    size_t done = 0;
    while (done < want) {
        if (block_pos_ >= block_len_ && !refill()) break;
        const size_t n = std::min(want - done, block_len_ - block_pos_);
        for (int ch = 0; ch < channels_; ++ch) {
            std::memcpy(planar[ch] + done, block_.data() + block_size_ * ch + block_pos_, n);
        }
        block_pos_ += n;
        done += n;
    }
    position_ += done;
    return done;
}

void DsfReader::seek_bytes(uint64_t bytes_per_channel) {
    position_ = std::min(bytes_per_channel, total_bytes_per_channel());
    const uint64_t block_index = position_ / block_size_;
    const uint64_t offset = data_offset_ + block_index * block_size_ * channels_;
    _fseeki64(file_.get(), static_cast<int64_t>(offset), SEEK_SET);
    block_pos_ = 0;
    block_len_ = 0;
    if (position_ % block_size_ != 0 && refill()) {
        block_pos_ = static_cast<size_t>(position_ % block_size_);
    }
}

}  // namespace arpegio
