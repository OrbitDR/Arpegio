// What a device consumes. The ring buffer holds these bytes, interleaved and little endian.
#pragma once

#include <cstddef>
#include <cstdint>

namespace arpegio {

enum class WireEncoding : uint8_t {
    S16,         // 2 bytes
    S24,         // 3 bytes, packed
    S32,         // 4 bytes, left-justified
    S32Right24,  // 4 bytes, right-aligned with sign extension (ASIO Int32LSB24)
    S32Right20,
    S32Right18,
    S32Right16,
    F32,
    F64,
    DsdMsb,  // one byte per channel, first bit in the MSB
    DsdLsb,  // one byte per channel, first bit in the LSB
};

constexpr size_t wire_sample_bytes(WireEncoding encoding) {
    switch (encoding) {
    case WireEncoding::S16: return 2;
    case WireEncoding::S24: return 3;
    case WireEncoding::F64: return 8;
    case WireEncoding::DsdMsb:
    case WireEncoding::DsdLsb: return 1;
    default: return 4;
    }
}

constexpr bool wire_is_dsd(WireEncoding encoding) {
    return encoding == WireEncoding::DsdMsb || encoding == WireEncoding::DsdLsb;
}

// Bits a sample can carry without loss.
constexpr int wire_resolution(WireEncoding encoding) {
    switch (encoding) {
    case WireEncoding::S16:
    case WireEncoding::S32Right16: return 16;
    case WireEncoding::S32Right18: return 18;
    case WireEncoding::S32Right20: return 20;
    case WireEncoding::S24:
    case WireEncoding::S32Right24:
    case WireEncoding::F32: return 24;
    case WireEncoding::S32: return 32;
    case WireEncoding::F64: return 53;
    default: return 1;
    }
}

constexpr const char* wire_name(WireEncoding encoding) {
    switch (encoding) {
    case WireEncoding::S16: return "S16";
    case WireEncoding::S24: return "S24";
    case WireEncoding::S32: return "S32";
    case WireEncoding::S32Right24: return "S32R24";
    case WireEncoding::S32Right20: return "S32R20";
    case WireEncoding::S32Right18: return "S32R18";
    case WireEncoding::S32Right16: return "S32R16";
    case WireEncoding::F32: return "F32";
    case WireEncoding::F64: return "F64";
    case WireEncoding::DsdMsb: return "DSD-MSB";
    case WireEncoding::DsdLsb: return "DSD-LSB";
    }
    return "?";
}

struct DeviceFormat {
    WireEncoding encoding = WireEncoding::S32;
    uint32_t rate = 0;   // frames per second; 1-bit samples per second for DSD
    int channels = 0;
    int valid_bits = 0;  // significant bits in the container, reported to WASAPI

    size_t frame_bytes() const { return wire_sample_bytes(encoding) * channels; }

    // Ring frames per second. DSD moves one byte per channel for every 8 samples.
    uint32_t ring_frame_rate() const { return wire_is_dsd(encoding) ? rate / 8 : rate; }

    bool operator==(const DeviceFormat&) const = default;
};

}  // namespace arpegio
