// Minimal 64-bit float WAV writer, used by the measurement tools.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>

namespace arpegio {

class WavWriter {
public:
    WavWriter(const std::filesystem::path& path, uint32_t sample_rate, int channels);
    ~WavWriter();

    void write(const double* interleaved, size_t frames);

private:
    struct FileCloser {
        void operator()(std::FILE* f) const { std::fclose(f); }
    };

    std::unique_ptr<std::FILE, FileCloser> file_;
    uint32_t sample_rate_;
    int channels_;
    uint64_t frames_ = 0;
};

}  // namespace arpegio
