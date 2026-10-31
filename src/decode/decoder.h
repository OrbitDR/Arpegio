// Source of audio data, one per file or track.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace arpegio {

struct SourceFormat {
    bool dsd = false;
    uint32_t rate = 0;    // frames per second; 1-bit samples per second for DSD
    int channels = 0;
    int bits = 0;         // source word length, 1 for DSD
    uint64_t length = 0;  // total frames; bytes per channel for DSD

    bool operator==(const SourceFormat&) const = default;

    double seconds() const { return dsd ? length * 8.0 / rate : static_cast<double>(length) / rate; }
};

class Decoder {
public:
    virtual ~Decoder() = default;

    virtual const SourceFormat& format() const = 0;

    // PCM sources. Interleaved doubles where an N-bit integer v reads as v / 2^(N-1). A double
    // holds every 32-bit integer exactly, so this path keeps bit-perfect samples intact.
    // Returns frames read, 0 at the end.
    virtual size_t read_pcm(double* /*interleaved*/, size_t /*frames*/) { return 0; }

    // DSD sources. Planar MSB-first bytes. Returns bytes per channel, 0 at the end.
    virtual size_t read_dsd(uint8_t* const* /*planar*/, size_t /*bytes*/) { return 0; }

    // Position in frames, or bytes per channel for DSD.
    virtual void seek(uint64_t position) = 0;
};

// Picks the decoder from the file contents and extension. Throws std::runtime_error.
std::unique_ptr<Decoder> open_decoder(const std::filesystem::path& path);

}  // namespace arpegio
