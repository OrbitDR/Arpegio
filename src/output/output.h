// Output devices: what they can do and the interface the engine drives.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/device_format.h"

namespace arpegio {

enum class OutputKind { Asio, WasapiExclusive, WasapiShared };

constexpr const char* output_kind_name(OutputKind kind) {
    switch (kind) {
    case OutputKind::Asio: return "ASIO";
    case OutputKind::WasapiExclusive: return "WASAPI exclusive";
    case OutputKind::WasapiShared: return "WASAPI shared";
    }
    return "?";
}

struct PcmMode {
    uint32_t rate = 0;
    WireEncoding encoding = WireEncoding::S32;
    int valid_bits = 0;
};

struct DeviceInfo {
    OutputKind kind = OutputKind::WasapiShared;
    std::wstring id;   // ASIO: driver CLSID. WASAPI: endpoint id.
    std::string name;  // UTF-8
    int channels = 0;
    std::vector<PcmMode> pcm_modes;
    std::vector<uint32_t> dsd_rates;  // native DSD, ASIO only
    std::vector<std::string> channel_names;
    std::vector<std::string> input_channel_names;
    int buffer_min = 0;
    int buffer_max = 0;
    int buffer_preferred = 0;
    double hardware_gain_db = 0.0;  // Windows hardware volume of a WASAPI endpoint, 0 when neutral or unknown
    std::string error;              // why the device could not be queried, empty when it could
};

// Supplies audio from the real-time thread. Must not block, allocate or log.
class FrameSource {
public:
    virtual ~FrameSource() = default;

    // Always delivers `frames` ring frames, padding with the format's silence.
    virtual void pull(uint8_t* destination, size_t frames) noexcept = 0;
    virtual void silence(uint8_t* destination, size_t frames) noexcept = 0;
};

struct OutputStats {
    uint64_t callbacks = 0;
    uint64_t late_callbacks = 0;  // arrived more than 1.5 periods after the previous one
    uint64_t max_gap_us = 0;
    uint64_t max_service_us = 0;
    uint64_t resyncs = 0;
    uint64_t overloads = 0;
    uint32_t period_frames = 0;
    uint32_t latency_frames = 0;
};

struct OutputOptions {
    int first_channel = 0;       // ASIO: index of the first output channel to use
    uint32_t period_ms = 0;      // WASAPI exclusive: 0 picks the default
    int buffer_frames = 0;       // ASIO: 0 keeps the driver's preferred size

    // ASIO loopback verification: records two input channels, interleaved, while playing.
    int capture_channel = -1;
    int32_t* capture_buffer = nullptr;
    size_t capture_frames = 0;
    std::atomic<size_t>* capture_written = nullptr;
};

class Output {
public:
    virtual ~Output() = default;

    // Opens the stream. May change the encoding (ASIO reports its DSD bit order only after
    // switching) and returns what the device will really take. Throws std::runtime_error.
    virtual DeviceFormat open(const DeviceFormat& requested, FrameSource& source) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void close() = 0;

    // True after a driver reset request or a device error. The engine reopens the stream.
    virtual bool needs_reopen() const = 0;
    virtual std::string fault() const = 0;
    virtual OutputStats stats() const = 0;
    virtual bool has_control_panel() const { return false; }
    virtual void open_control_panel() {}
};

std::vector<DeviceInfo> list_asio_devices();
std::vector<DeviceInfo> list_wasapi_devices(OutputKind kind);  // call from an MTA thread

// What a device's Windows volume is right now, which can differ from when it was listed. Call from an
// MTA thread.
double current_hardware_gain_db(const DeviceInfo& device);

std::unique_ptr<Output> make_output(const DeviceInfo& device, const OutputOptions& options = {});

}  // namespace arpegio
