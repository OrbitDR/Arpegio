// Reader for Sony DSF files. Delivers planar MSB-first bytes whatever the file stores.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

namespace arpegio {

class DsfReader {
public:
    explicit DsfReader(const std::filesystem::path& path);

    uint32_t sample_rate() const { return sample_rate_; }  // 1-bit samples per second
    int channels() const { return channels_; }
    uint64_t total_bytes_per_channel() const { return sample_count_ / 8; }

    // Fills one buffer per channel. Returns the bytes read per channel, 0 at the end.
    size_t read(uint8_t* const* planar, size_t bytes_per_channel);

    void seek_bytes(uint64_t bytes_per_channel);

private:
    bool refill();

    struct FileCloser {
        void operator()(std::FILE* f) const { std::fclose(f); }
    };

    std::unique_ptr<std::FILE, FileCloser> file_;
    uint32_t sample_rate_ = 0;
    int channels_ = 0;
    uint32_t bits_per_sample_ = 0;
    uint64_t sample_count_ = 0;
    uint32_t block_size_ = 0;
    uint64_t data_offset_ = 0;
    uint64_t position_ = 0;  // bytes per channel already delivered
    std::vector<uint8_t> block_;
    size_t block_pos_ = 0;
    size_t block_len_ = 0;
};

}  // namespace arpegio
