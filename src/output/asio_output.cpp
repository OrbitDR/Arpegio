// ASIO through the Steinberg SDK interfaces (asio.h, asiosys.h, iasiodrv.h).
// The SDK's host samples are not compiled in: asiodrivers.cpp unloads the driver when another
// one is queried and its ANSI registry code truncates names, so drivers load directly by CLSID.
#include <windows.h>

#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "asiosys.h"

#include "asio.h"
#include "iasiodrv.h"

#include "output/output.h"
#include "output/sta_thread.h"

namespace arpegio {

namespace {

constexpr uint32_t kPcmRates[] = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000};
constexpr uint32_t kDsdRates[] = {2822400, 3072000, 5644800, 6144000, 11289600, 12288000, 22579200, 24576000};

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::string ansi_to_utf8(const char* text) {
    const int size = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    std::wstring wide(size > 0 ? size - 1 : 0, L'\0');
    if (size > 0) MultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), size);
    return to_utf8(wide);
}

bool wire_from_asio(ASIOSampleType type, WireEncoding& encoding) {
    switch (type) {
    case ASIOSTInt16LSB: encoding = WireEncoding::S16; return true;
    case ASIOSTInt24LSB: encoding = WireEncoding::S24; return true;
    case ASIOSTInt32LSB: encoding = WireEncoding::S32; return true;
    case ASIOSTInt32LSB24: encoding = WireEncoding::S32Right24; return true;
    case ASIOSTInt32LSB20: encoding = WireEncoding::S32Right20; return true;
    case ASIOSTInt32LSB18: encoding = WireEncoding::S32Right18; return true;
    case ASIOSTInt32LSB16: encoding = WireEncoding::S32Right16; return true;
    case ASIOSTFloat32LSB: encoding = WireEncoding::F32; return true;
    case ASIOSTFloat64LSB: encoding = WireEncoding::F64; return true;
    case ASIOSTDSDInt8MSB1: encoding = WireEncoding::DsdMsb; return true;
    case ASIOSTDSDInt8LSB1: encoding = WireEncoding::DsdLsb; return true;
    default: return false;
    }
}

const char* asio_error_name(long code) {
    switch (code) {
    case ASE_OK: return "ASE_OK";
    case ASE_SUCCESS: return "ASE_SUCCESS";
    case ASE_NotPresent: return "ASE_NotPresent";
    case ASE_HWMalfunction: return "ASE_HWMalfunction";
    case ASE_InvalidParameter: return "ASE_InvalidParameter";
    case ASE_InvalidMode: return "ASE_InvalidMode";
    case ASE_SPNotAdvancing: return "ASE_SPNotAdvancing";
    case ASE_NoClock: return "ASE_NoClock";
    case ASE_NoMemory: return "ASE_NoMemory";
    default: return "unknown ASIO error";
    }
}

[[noreturn]] void fail(IASIO* driver, const std::string& what, long code = ASE_OK) {
    std::string text = what;
    if (code != ASE_OK) text += std::string(" (") + asio_error_name(code) + ")";
    if (driver) {
        char message[128] = {};
        driver->getErrorMessage(message);
        if (message[0]) text += ": " + ansi_to_utf8(message);
    }
    throw std::runtime_error(text);
}

struct DriverEntry {
    std::string name;
    std::wstring clsid;
};

std::vector<DriverEntry> read_registry() {
    std::vector<DriverEntry> drivers;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS) return drivers;
    for (DWORD index = 0;; ++index) {
        wchar_t name[256];
        DWORD length = 256;
        if (RegEnumKeyExW(root, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t clsid[64] = {};
        DWORD size = sizeof(clsid);
        if (RegGetValueW(root, name, L"CLSID", RRF_RT_REG_SZ, nullptr, clsid, &size) == ERROR_SUCCESS) {
            drivers.push_back({to_utf8(name), clsid});
        }
    }
    RegCloseKey(root);
    return drivers;
}

IASIO* load_driver(const std::wstring& clsid_text, HWND window) {
    CLSID clsid{};
    if (FAILED(CLSIDFromString(clsid_text.c_str(), &clsid))) throw std::runtime_error("invalid driver CLSID");
    IASIO* driver = nullptr;
    // ASIO drivers use their CLSID as the interface id too.
    const HRESULT result =
        CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&driver));
    if (FAILED(result) || !driver) throw std::runtime_error("the ASIO driver could not be created");
    if (!driver->init(window)) {
        std::string text = "the ASIO driver refused to start";
        char message[128] = {};
        driver->getErrorMessage(message);
        if (message[0]) text += ": " + ansi_to_utf8(message);
        driver->Release();
        throw std::runtime_error(text);
    }
    return driver;
}

DeviceInfo probe_driver(const DriverEntry& entry, HWND window) {
    DeviceInfo info;
    info.kind = OutputKind::Asio;
    info.id = entry.clsid;
    info.name = entry.name;

    IASIO* driver = load_driver(entry.clsid, window);
    std::unique_ptr<IASIO, void (*)(IASIO*)> guard(driver, [](IASIO* d) { d->Release(); });

    long inputs = 0;
    long outputs = 0;
    long code = driver->getChannels(&inputs, &outputs);
    if (code != ASE_OK) fail(driver, "getChannels failed", code);
    info.channels = outputs;

    long minimum = 0, maximum = 0, preferred = 0, granularity = 0;
    if (driver->getBufferSize(&minimum, &maximum, &preferred, &granularity) == ASE_OK) {
        info.buffer_min = minimum;
        info.buffer_max = maximum;
        info.buffer_preferred = preferred;
    }

    WireEncoding encoding = WireEncoding::S32;
    for (long channel = 0; channel < outputs; ++channel) {
        ASIOChannelInfo channel_info{};
        channel_info.channel = channel;
        channel_info.isInput = ASIOFalse;
        if (driver->getChannelInfo(&channel_info) != ASE_OK) continue;
        if (channel == 0 && !wire_from_asio(channel_info.type, encoding)) {
            fail(nullptr, "unsupported ASIO sample type " + std::to_string(channel_info.type));
        }
        info.channel_names.push_back(ansi_to_utf8(channel_info.name));
    }
    for (long channel = 0; channel < inputs; ++channel) {
        ASIOChannelInfo channel_info{};
        channel_info.channel = channel;
        channel_info.isInput = ASIOTrue;
        if (driver->getChannelInfo(&channel_info) == ASE_OK) {
            info.input_channel_names.push_back(ansi_to_utf8(channel_info.name));
        }
    }

    for (uint32_t rate : kPcmRates) {
        if (driver->canSampleRate(rate) == ASE_OK) info.pcm_modes.push_back({rate, encoding, wire_resolution(encoding)});
    }

    ASIOIoFormat format{};
    format.FormatType = kASIODSDFormat;
    if (driver->future(kAsioCanDoIoFormat, &format) == ASE_SUCCESS) {
        for (uint32_t rate : kDsdRates) {
            if (driver->canSampleRate(rate) == ASE_OK) info.dsd_rates.push_back(rate);
        }
    }
    return info;
}

class AsioOutput;
std::atomic<AsioOutput*> g_active{nullptr};  // ASIO callbacks carry no context argument

class AsioOutput final : public Output {
public:
    AsioOutput(DeviceInfo device, OutputOptions options) : device_(std::move(device)), options_(options) {}

    ~AsioOutput() override { close(); }

    DeviceFormat open(const DeviceFormat& requested, FrameSource& source) override {
        close();
        return sta_.run([&] { return open_on_sta(requested, source); });
    }

    void start() override {
        sta_.run([&] {
            if (!driver_) return;
            source_->silence(scratch_.data(), ring_frames_);
            deliver(0);
            deliver(1);
            callbacks_.store(0, std::memory_order_relaxed);
            last_tick_ = 0;
            g_active.store(this, std::memory_order_release);
            running_ = true;
            const long code = driver_->start();
            if (code != ASE_OK) {
                running_ = false;
                g_active.store(nullptr, std::memory_order_release);
                fail(driver_, "start failed", code);
            }
        });
    }

    void stop() override {
        sta_.run([&] {
            if (driver_ && running_) driver_->stop();
            running_ = false;
            g_active.store(nullptr, std::memory_order_release);
        });
    }

    void close() override {
        sta_.run([&] {
            if (!driver_) return;
            if (running_) driver_->stop();
            running_ = false;
            g_active.store(nullptr, std::memory_order_release);
            if (buffers_created_) driver_->disposeBuffers();
            buffers_created_ = false;
            if (format_.encoding == WireEncoding::DsdMsb || format_.encoding == WireEncoding::DsdLsb) {
                ASIOIoFormat pcm{};
                pcm.FormatType = kASIOPCMFormat;
                driver_->future(kAsioSetIoFormat, &pcm);
            }
            driver_->Release();
            driver_ = nullptr;
        });
    }

    bool needs_reopen() const override { return reset_requested_.load(std::memory_order_acquire); }
    std::string fault() const override { return reset_requested_ ? "the ASIO driver asked for a reset" : ""; }

    OutputStats stats() const override {
        OutputStats s;
        s.callbacks = callbacks_.load(std::memory_order_relaxed);
        s.late_callbacks = late_callbacks_.load(std::memory_order_relaxed);
        s.max_gap_us = max_gap_us_.load(std::memory_order_relaxed);
        s.max_service_us = max_service_us_.load(std::memory_order_relaxed);
        s.resyncs = resyncs_.load(std::memory_order_relaxed);
        s.overloads = overloads_.load(std::memory_order_relaxed);
        s.period_frames = static_cast<uint32_t>(ring_frames_);
        s.latency_frames = latency_frames_;
        return s;
    }

    bool has_control_panel() const override { return true; }

    void open_control_panel() override {
        sta_.run([&] {
            if (driver_) {
                driver_->controlPanel();
                return;
            }
            IASIO* driver = load_driver(device_.id, sta_.window());
            driver->controlPanel();
            driver->Release();
        });
    }

private:
    DeviceFormat open_on_sta(const DeviceFormat& requested, FrameSource& source) {
        source_ = &source;
        reset_requested_.store(false, std::memory_order_release);
        driver_ = load_driver(device_.id, sta_.window());
        try {
            return configure(requested);
        } catch (...) {
            if (buffers_created_) driver_->disposeBuffers();
            buffers_created_ = false;
            driver_->Release();
            driver_ = nullptr;
            throw;
        }
    }

    DeviceFormat configure(const DeviceFormat& requested) {
        const bool dsd = wire_is_dsd(requested.encoding);

        // The format must change before the driver reaches the prepared state.
        ASIOIoFormat format{};
        format.FormatType = dsd ? kASIODSDFormat : kASIOPCMFormat;
        const long format_code = driver_->future(kAsioSetIoFormat, &format);
        if (dsd && format_code != ASE_SUCCESS) fail(driver_, "the driver cannot enter DSD mode", format_code);

        long code = driver_->canSampleRate(requested.rate);
        if (code != ASE_OK) fail(driver_, "the driver rejects " + std::to_string(requested.rate) + " Hz", code);
        code = driver_->setSampleRate(requested.rate);
        if (code != ASE_OK) fail(driver_, "setSampleRate failed", code);
        ASIOSampleRate current = 0;
        if (driver_->getSampleRate(&current) == ASE_OK && static_cast<uint32_t>(current) != requested.rate) {
            fail(nullptr, "the driver stayed at " + std::to_string(static_cast<uint32_t>(current)) + " Hz");
        }

        long inputs = 0;
        long outputs = 0;
        code = driver_->getChannels(&inputs, &outputs);
        if (code != ASE_OK) fail(driver_, "getChannels failed", code);
        if (options_.first_channel + requested.channels > outputs) {
            fail(nullptr, "the driver has only " + std::to_string(outputs) + " output channels");
        }

        long minimum = 0, maximum = 0, preferred = 0, granularity = 0;
        code = driver_->getBufferSize(&minimum, &maximum, &preferred, &granularity);
        if (code != ASE_OK) fail(driver_, "getBufferSize failed", code);
        long buffer = options_.buffer_frames > 0 ? options_.buffer_frames : preferred;
        buffer = std::clamp(buffer, minimum, maximum);

        DeviceFormat actual = requested;
        WireEncoding encoding{};
        for (int ch = 0; ch < requested.channels; ++ch) {
            ASIOChannelInfo info{};
            info.channel = options_.first_channel + ch;
            info.isInput = ASIOFalse;
            code = driver_->getChannelInfo(&info);
            if (code != ASE_OK) fail(driver_, "getChannelInfo failed", code);
            WireEncoding this_channel{};
            if (!wire_from_asio(info.type, this_channel) || wire_is_dsd(this_channel) != dsd) {
                fail(nullptr, "unsupported ASIO sample type " + std::to_string(info.type));
            }
            if (ch > 0 && this_channel != encoding) fail(nullptr, "output channels differ in sample type");
            encoding = this_channel;
        }
        actual.encoding = encoding;
        actual.valid_bits = wire_resolution(encoding);

        const int capture_channels = options_.capture_channel >= 0 ? 2 : 0;
        buffers_.assign(requested.channels + capture_channels, ASIOBufferInfo{});
        for (int ch = 0; ch < requested.channels; ++ch) {
            buffers_[ch].isInput = ASIOFalse;
            buffers_[ch].channelNum = options_.first_channel + ch;
        }
        for (int ch = 0; ch < capture_channels; ++ch) {
            buffers_[requested.channels + ch].isInput = ASIOTrue;
            buffers_[requested.channels + ch].channelNum = options_.capture_channel + ch;
        }
        callbacks_table_.bufferSwitch = &AsioOutput::on_buffer_switch;
        callbacks_table_.sampleRateDidChange = &AsioOutput::on_sample_rate_changed;
        callbacks_table_.asioMessage = &AsioOutput::on_message;
        callbacks_table_.bufferSwitchTimeInfo = &AsioOutput::on_buffer_switch_time;

        code = driver_->createBuffers(buffers_.data(), requested.channels + capture_channels, buffer, &callbacks_table_);
        if (code != ASE_OK) fail(driver_, "createBuffers failed", code);
        buffers_created_ = true;

        long input_latency = 0;
        long output_latency = 0;
        driver_->getLatencies(&input_latency, &output_latency);
        latency_frames_ = static_cast<uint32_t>(output_latency);

        output_ready_ = driver_->outputReady() == ASE_OK;

        // Buffer sizes count samples. A DSD sample is one bit, packed 8 to a byte.
        ring_frames_ = dsd ? static_cast<size_t>(buffer) / 8 : static_cast<size_t>(buffer);
        sample_bytes_ = wire_sample_bytes(encoding);
        scratch_.assign(ring_frames_ * sample_bytes_ * requested.channels, 0);
        format_ = actual;
        period_us_ = static_cast<uint64_t>(ring_frames_) * 1000000 / std::max<uint32_t>(actual.ring_frame_rate(), 1);
        return actual;
    }

    template <typename Sample>
    void split(long index) noexcept {
        const int channels = format_.channels;
        for (int ch = 0; ch < channels; ++ch) {
            auto* out = static_cast<Sample*>(buffers_[ch].buffers[index]);
            const auto* in = reinterpret_cast<const Sample*>(scratch_.data()) + ch;
            for (size_t frame = 0; frame < ring_frames_; ++frame) {
                out[frame] = *in;
                in += channels;
            }
        }
    }

    void split_packed24(long index) noexcept {
        const int channels = format_.channels;
        for (int ch = 0; ch < channels; ++ch) {
            auto* out = static_cast<uint8_t*>(buffers_[ch].buffers[index]);
            const uint8_t* in = scratch_.data() + ch * 3;
            for (size_t frame = 0; frame < ring_frames_; ++frame) {
                out[0] = in[0];
                out[1] = in[1];
                out[2] = in[2];
                out += 3;
                in += static_cast<size_t>(channels) * 3;
            }
        }
    }

    void record_input(long index) noexcept {
        const size_t written = options_.capture_written->load(std::memory_order_relaxed);
        const size_t frames = std::min(ring_frames_, options_.capture_frames - written);
        const auto* left = static_cast<const int32_t*>(buffers_[format_.channels].buffers[index]);
        const auto* right = static_cast<const int32_t*>(buffers_[format_.channels + 1].buffers[index]);
        int32_t* out = options_.capture_buffer + written * 2;
        for (size_t frame = 0; frame < frames; ++frame) {
            out[frame * 2] = left[frame];
            out[frame * 2 + 1] = right[frame];
        }
        options_.capture_written->store(written + frames, std::memory_order_release);
    }

    void deliver(long index) noexcept {
        switch (sample_bytes_) {
        case 1: split<uint8_t>(index); break;
        case 2: split<uint16_t>(index); break;
        case 3: split_packed24(index); break;
        case 4: split<uint32_t>(index); break;
        default: split<uint64_t>(index); break;
        }
    }

    void process(long index) noexcept {
        LARGE_INTEGER begin;
        QueryPerformanceCounter(&begin);
        const uint64_t count = callbacks_.fetch_add(1, std::memory_order_relaxed);
        if (last_tick_ != 0 && count > 0) {
            const uint64_t gap = to_microseconds(begin.QuadPart - last_tick_);
            if (gap > max_gap_us_.load(std::memory_order_relaxed)) max_gap_us_.store(gap, std::memory_order_relaxed);
            if (gap > period_us_ + period_us_ / 2) late_callbacks_.fetch_add(1, std::memory_order_relaxed);
        }
        last_tick_ = begin.QuadPart;

        source_->pull(scratch_.data(), ring_frames_);
        deliver(index);
        if (output_ready_) driver_->outputReady();
        if (options_.capture_channel >= 0) record_input(index);

        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        const uint64_t service = to_microseconds(end.QuadPart - begin.QuadPart);
        if (service > max_service_us_.load(std::memory_order_relaxed)) {
            max_service_us_.store(service, std::memory_order_relaxed);
        }
    }

    static uint64_t to_microseconds(int64_t ticks) {
        static const int64_t frequency = [] {
            LARGE_INTEGER value;
            QueryPerformanceFrequency(&value);
            return value.QuadPart;
        }();
        return static_cast<uint64_t>(ticks) * 1000000 / static_cast<uint64_t>(frequency);
    }

    static void on_buffer_switch(long index, ASIOBool) {
        if (AsioOutput* self = g_active.load(std::memory_order_acquire)) self->process(index);
    }

    static ASIOTime* on_buffer_switch_time(ASIOTime* time, long index, ASIOBool direct) {
        on_buffer_switch(index, direct);
        return time;
    }

    static void on_sample_rate_changed(ASIOSampleRate rate) {
        if (AsioOutput* self = g_active.load(std::memory_order_acquire)) {
            if (static_cast<uint32_t>(rate) != self->format_.rate) {
                self->reset_requested_.store(true, std::memory_order_release);
            }
        }
    }

    // May arrive on the driver's thread: it only raises flags, the engine acts on them.
    static long on_message(long selector, long value, void*, double*) {
        AsioOutput* self = g_active.load(std::memory_order_acquire);
        switch (selector) {
        case kAsioSelectorSupported:
            return value == kAsioEngineVersion || value == kAsioResetRequest || value == kAsioBufferSizeChange ||
                           value == kAsioResyncRequest || value == kAsioLatenciesChanged || value == kAsioOverload
                       ? 1
                       : 0;
        case kAsioEngineVersion:
            return 2;
        case kAsioResetRequest:
        case kAsioBufferSizeChange:
            if (self) self->reset_requested_.store(true, std::memory_order_release);
            return 1;
        case kAsioResyncRequest:
            if (self) self->resyncs_.fetch_add(1, std::memory_order_relaxed);
            return 1;
        case kAsioLatenciesChanged:
            return 1;
        case kAsioOverload:
            if (self) self->overloads_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        default:
            return 0;
        }
    }

    DeviceInfo device_;
    OutputOptions options_;
    StaThread sta_;
    IASIO* driver_ = nullptr;
    FrameSource* source_ = nullptr;
    ASIOCallbacks callbacks_table_{};
    std::vector<ASIOBufferInfo> buffers_;
    std::vector<uint8_t> scratch_;
    DeviceFormat format_{};
    size_t ring_frames_ = 0;
    size_t sample_bytes_ = 4;
    uint64_t period_us_ = 0;
    uint32_t latency_frames_ = 0;
    int64_t last_tick_ = 0;
    bool buffers_created_ = false;
    bool running_ = false;
    bool output_ready_ = false;

    std::atomic<bool> reset_requested_{false};
    std::atomic<uint64_t> callbacks_{0};
    std::atomic<uint64_t> late_callbacks_{0};
    std::atomic<uint64_t> max_gap_us_{0};
    std::atomic<uint64_t> max_service_us_{0};
    std::atomic<uint64_t> resyncs_{0};
    std::atomic<uint64_t> overloads_{0};
};

}  // namespace

std::vector<DeviceInfo> list_asio_devices() {
    std::vector<DeviceInfo> devices;
    StaThread sta;
    for (const DriverEntry& entry : read_registry()) {
        DeviceInfo info;
        try {
            info = sta.run([&] { return probe_driver(entry, sta.window()); });
        } catch (const std::exception& error) {
            info.kind = OutputKind::Asio;
            info.id = entry.clsid;
            info.name = entry.name;
            info.error = error.what();
        }
        devices.push_back(std::move(info));
    }
    return devices;
}

std::unique_ptr<Output> make_asio_output(const DeviceInfo& device, const OutputOptions& options) {
    return std::make_unique<AsioOutput>(device, options);
}

}  // namespace arpegio
