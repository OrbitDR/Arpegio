// WASAPI shared and exclusive output.
//
// Exclusive streams are event-driven: two endpoint buffers play ping-pong, and each event hands
// the client one entire buffer. GetCurrentPadding has no meaning there, so it is never consulted.
#include <windows.h>

#include <initguid.h>

#include <audioclient.h>
#include <avrt.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <propkeydef.h>
#include <wrl/client.h>

#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "output/output.h"

using Microsoft::WRL::ComPtr;

namespace arpegio {

namespace {

constexpr GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

constexpr uint32_t kRates[] = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000};

constexpr REFERENCE_TIME kHnsPerSecond = 10000000;
constexpr REFERENCE_TIME kHnsPerMs = 10000;
constexpr uint32_t kDefaultPeriodMs = 40;
constexpr REFERENCE_TIME kSharedBuffer = 200 * kHnsPerMs;
constexpr DWORD kStallTimeoutMs = 2000;

struct Candidate {
    WireEncoding encoding;
    int valid_bits;
};

// Best first. Integer containers come before float so samples stay exact.
constexpr Candidate kCandidates[] = {
    {WireEncoding::S32, 32},
    {WireEncoding::S32, 24},
    {WireEncoding::S24, 24},
    {WireEncoding::S16, 16},
};

std::string to_utf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(size > 0 ? size - 1 : 0, '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::string hresult_text(HRESULT result) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(result));
    return text;
}

DWORD channel_mask(int channels) {
    switch (channels) {
    case 1: return SPEAKER_FRONT_CENTER;
    case 2: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    case 6:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
               SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    case 8:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
               SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    default: return 0;
    }
}

WAVEFORMATEXTENSIBLE make_wave_format(const DeviceFormat& format) {
    WAVEFORMATEXTENSIBLE wave{};
    const auto sample_bytes = static_cast<WORD>(wire_sample_bytes(format.encoding));
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = static_cast<WORD>(format.channels);
    wave.Format.nSamplesPerSec = format.rate;
    wave.Format.wBitsPerSample = static_cast<WORD>(sample_bytes * 8);
    wave.Format.nBlockAlign = static_cast<WORD>(sample_bytes * format.channels);
    wave.Format.nAvgBytesPerSec = format.rate * wave.Format.nBlockAlign;
    wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wave.Samples.wValidBitsPerSample = static_cast<WORD>(format.valid_bits);
    wave.dwChannelMask = channel_mask(format.channels);
    const bool is_float = format.encoding == WireEncoding::F32 || format.encoding == WireEncoding::F64;
    wave.SubFormat = is_float ? kSubtypeFloat : kSubtypePcm;
    return wave;
}

// Maps the shared-mode mix format to a wire encoding.
bool mix_format_to_device(const WAVEFORMATEX* mix, DeviceFormat& format) {
    format.rate = mix->nSamplesPerSec;
    format.channels = mix->nChannels;
    bool is_float = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool is_pcm = mix->wFormatTag == WAVE_FORMAT_PCM;
    format.valid_bits = mix->wBitsPerSample;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
        is_float = extensible->SubFormat == kSubtypeFloat;
        is_pcm = extensible->SubFormat == kSubtypePcm;
        format.valid_bits = extensible->Samples.wValidBitsPerSample;
    }
    if (is_float && mix->wBitsPerSample == 32) format.encoding = WireEncoding::F32;
    else if (is_float && mix->wBitsPerSample == 64) format.encoding = WireEncoding::F64;
    else if (is_pcm && mix->wBitsPerSample == 32) format.encoding = WireEncoding::S32;
    else if (is_pcm && mix->wBitsPerSample == 24) format.encoding = WireEncoding::S24;
    else if (is_pcm && mix->wBitsPerSample == 16) format.encoding = WireEncoding::S16;
    else return false;
    return true;
}

ComPtr<IMMDevice> find_device(const std::wstring& id) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) throw std::runtime_error("MMDeviceEnumerator failed: " + hresult_text(result));
    ComPtr<IMMDevice> device;
    result = enumerator->GetDevice(id.c_str(), &device);
    if (FAILED(result)) throw std::runtime_error("audio device not found: " + hresult_text(result));
    return device;
}

class WasapiOutput final : public Output {
public:
    WasapiOutput(DeviceInfo device, OutputOptions options)
        : device_info_(std::move(device)), options_(options), exclusive_(device_info_.kind == OutputKind::WasapiExclusive) {
        stop_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        buffer_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    ~WasapiOutput() override {
        close();
        CloseHandle(stop_event_);
        CloseHandle(buffer_event_);
    }

    DeviceFormat open(const DeviceFormat& requested, FrameSource& source) override {
        close();
        source_ = &source;
        format_ = requested;
        faulted_.store(false, std::memory_order_release);
        {
            std::lock_guard lock(fault_mutex_);
            fault_.clear();
        }
        device_ = find_device(device_info_.id);
        if (exclusive_) initialize_exclusive();
        else initialize_shared();

        HRESULT result = client_->SetEventHandle(buffer_event_);
        if (FAILED(result)) throw std::runtime_error("SetEventHandle failed: " + hresult_text(result));
        result = client_->GetBufferSize(&buffer_frames_);
        if (FAILED(result)) throw std::runtime_error("GetBufferSize failed: " + hresult_text(result));
        result = client_->GetService(IID_PPV_ARGS(&render_));
        if (FAILED(result)) throw std::runtime_error("GetService failed: " + hresult_text(result));

        period_us_ = static_cast<uint64_t>(buffer_frames_) * 1000000 / format_.rate;
        return format_;
    }

    void start() override {
        if (!client_ || running_) return;

        // Microsoft's exclusive sample fills the first buffer before Start.
        BYTE* data = nullptr;
        HRESULT result = render_->GetBuffer(buffer_frames_, &data);
        if (FAILED(result)) throw std::runtime_error("GetBuffer failed: " + hresult_text(result));
        source_->pull(data, buffer_frames_);
        result = render_->ReleaseBuffer(buffer_frames_, 0);
        if (FAILED(result)) throw std::runtime_error("ReleaseBuffer failed: " + hresult_text(result));

        ResetEvent(stop_event_);
        result = client_->Start();
        if (FAILED(result)) throw std::runtime_error("IAudioClient::Start failed: " + hresult_text(result));
        running_ = true;
        thread_ = std::thread([this] { render_thread(); });
    }

    void stop() override {
        if (!running_) return;
        SetEvent(stop_event_);
        thread_.join();
        client_->Stop();
        client_->Reset();
        running_ = false;
    }

    void close() override {
        stop();
        render_.Reset();
        client_.Reset();
        device_.Reset();
    }

    bool needs_reopen() const override { return faulted_.load(std::memory_order_acquire); }

    std::string fault() const override {
        std::lock_guard lock(fault_mutex_);
        return fault_;
    }

    OutputStats stats() const override {
        OutputStats s;
        s.callbacks = callbacks_.load(std::memory_order_relaxed);
        s.late_callbacks = late_callbacks_.load(std::memory_order_relaxed);
        s.max_gap_us = max_gap_us_.load(std::memory_order_relaxed);
        s.max_service_us = max_service_us_.load(std::memory_order_relaxed);
        s.period_frames = buffer_frames_;
        return s;
    }

private:
    void activate() {
        client_.Reset();
        const HRESULT result = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
        if (FAILED(result)) throw std::runtime_error("IMMDevice::Activate failed: " + hresult_text(result));
    }

    void initialize_exclusive() {
        activate();
        REFERENCE_TIME default_period = 0;
        REFERENCE_TIME minimum_period = 0;
        client_->GetDevicePeriod(&default_period, &minimum_period);

        const uint32_t period_ms = options_.period_ms ? options_.period_ms : kDefaultPeriodMs;
        REFERENCE_TIME period = std::max<REFERENCE_TIME>(period_ms * kHnsPerMs, minimum_period);

        const WAVEFORMATEXTENSIBLE wave = make_wave_format(format_);
        HRESULT result = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period,
                                             period, &wave.Format, nullptr);
        if (result == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            // Documented recovery: take the aligned size, then start over with a new client.
            UINT32 aligned_frames = 0;
            client_->GetBufferSize(&aligned_frames);
            period = static_cast<REFERENCE_TIME>(static_cast<double>(kHnsPerSecond) / format_.rate * aligned_frames + 0.5);
            activate();
            result = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                         &wave.Format, nullptr);
        }
        if (FAILED(result)) {
            if (result == AUDCLNT_E_DEVICE_IN_USE) {
                throw std::runtime_error("another application is using the device in exclusive mode");
            }
            throw std::runtime_error("exclusive Initialize failed: " + hresult_text(result));
        }
    }

    void initialize_shared() {
        activate();
        WAVEFORMATEX* mix = nullptr;
        HRESULT result = client_->GetMixFormat(&mix);
        if (FAILED(result)) throw std::runtime_error("GetMixFormat failed: " + hresult_text(result));
        DeviceFormat actual{};
        const bool known = mix_format_to_device(mix, actual);
        if (known) {
            result = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kSharedBuffer, 0, mix,
                                         nullptr);
        }
        CoTaskMemFree(mix);
        if (!known) throw std::runtime_error("unsupported Windows mix format");
        if (FAILED(result)) throw std::runtime_error("shared Initialize failed: " + hresult_text(result));
        format_ = actual;
    }

    void set_fault(const std::string& text) {
        {
            std::lock_guard lock(fault_mutex_);
            fault_ = text;
        }
        faulted_.store(true, std::memory_order_release);
    }

    void note_callback(int64_t now) noexcept {
        const uint64_t count = callbacks_.fetch_add(1, std::memory_order_relaxed);
        if (count > 0) {
            const uint64_t gap = to_microseconds(now - last_tick_);
            if (gap > max_gap_us_.load(std::memory_order_relaxed)) max_gap_us_.store(gap, std::memory_order_relaxed);
            if (gap > period_us_ + period_us_ / 2) late_callbacks_.fetch_add(1, std::memory_order_relaxed);
        }
        last_tick_ = now;
    }

    static uint64_t to_microseconds(int64_t ticks) {
        static const int64_t frequency = [] {
            LARGE_INTEGER value;
            QueryPerformanceFrequency(&value);
            return value.QuadPart;
        }();
        return static_cast<uint64_t>(ticks) * 1000000 / static_cast<uint64_t>(frequency);
    }

    // Returns false when the stream can no longer continue.
    bool render_one(UINT32 frames) noexcept {
        BYTE* data = nullptr;
        HRESULT result = render_->GetBuffer(frames, &data);
        if (SUCCEEDED(result)) {
            source_->pull(data, frames);
            result = render_->ReleaseBuffer(frames, 0);
        }
        if (FAILED(result)) {
            set_fault(result == AUDCLNT_E_DEVICE_INVALIDATED ? "the device was disconnected"
                                                             : "render failed: " + hresult_text(result));
            return false;
        }
        return true;
    }

    void render_thread() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD task_index = 0;
        HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

        const HANDLE handles[] = {stop_event_, buffer_event_};
        for (;;) {
            const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, kStallTimeoutMs);
            if (wait == WAIT_OBJECT_0) break;
            if (wait != WAIT_OBJECT_0 + 1) {
                set_fault("the device stopped requesting audio");
                break;
            }

            LARGE_INTEGER begin;
            QueryPerformanceCounter(&begin);
            note_callback(begin.QuadPart);

            UINT32 frames = buffer_frames_;
            if (!exclusive_) {
                UINT32 padding = 0;
                const HRESULT result = client_->GetCurrentPadding(&padding);
                if (FAILED(result)) {
                    set_fault(result == AUDCLNT_E_DEVICE_INVALIDATED ? "the device was disconnected"
                                                                     : "GetCurrentPadding failed: " + hresult_text(result));
                    break;
                }
                frames -= padding;
            }
            if (frames > 0 && !render_one(frames)) break;

            LARGE_INTEGER end;
            QueryPerformanceCounter(&end);
            const uint64_t service = to_microseconds(end.QuadPart - begin.QuadPart);
            if (service > max_service_us_.load(std::memory_order_relaxed)) {
                max_service_us_.store(service, std::memory_order_relaxed);
            }
        }

        if (task) AvRevertMmThreadCharacteristics(task);
        if (SUCCEEDED(com)) CoUninitialize();
    }

    DeviceInfo device_info_;
    OutputOptions options_;
    bool exclusive_;
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    FrameSource* source_ = nullptr;
    DeviceFormat format_{};
    UINT32 buffer_frames_ = 0;
    uint64_t period_us_ = 0;
    HANDLE stop_event_ = nullptr;
    HANDLE buffer_event_ = nullptr;
    std::thread thread_;
    bool running_ = false;
    int64_t last_tick_ = 0;

    mutable std::mutex fault_mutex_;
    std::string fault_;
    std::atomic<bool> faulted_{false};
    std::atomic<uint64_t> callbacks_{0};
    std::atomic<uint64_t> late_callbacks_{0};
    std::atomic<uint64_t> max_gap_us_{0};
    std::atomic<uint64_t> max_service_us_{0};
};

// Software volume never reaches an exclusive stream, but hardware volume does: on the Topping DX1 II
// it rescales the samples, and DoP stops being recognized at anything but 0 dB.
double read_hardware_gain_db(IMMDevice* device) {
    ComPtr<IAudioEndpointVolume> volume;
    DWORD support = 0;
    float decibels = 0.0f;
    if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume)) ||
        FAILED(volume->QueryHardwareSupport(&support)) || !(support & ENDPOINT_HARDWARE_SUPPORT_VOLUME) ||
        FAILED(volume->GetMasterVolumeLevel(&decibels))) {
        return 0.0;
    }
    return decibels;
}

}  // namespace

std::vector<DeviceInfo> list_wasapi_devices(OutputKind kind) {
    std::vector<DeviceInfo> devices;
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) return devices;
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) return devices;
    UINT count = 0;
    collection->GetCount(&count);

    std::wstring default_id;
    ComPtr<IMMDevice> default_device;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &default_device))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(default_device->GetId(&id))) {
            default_id = id;
            CoTaskMemFree(id);
        }
    }

    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(index, &device))) continue;

        DeviceInfo info;
        info.kind = kind;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id))) {
            info.id = id;
            info.is_default = info.id == default_id;
            CoTaskMemFree(id);
        }
        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR) {
                info.name = to_utf8(name.pwszVal);
            }
            PropVariantClear(&name);
        }
        info.hardware_gain_db = read_hardware_gain_db(device.Get());

        ComPtr<IAudioClient> client;
        result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
        if (FAILED(result)) {
            info.error = "Activate failed: " + hresult_text(result);
            devices.push_back(std::move(info));
            continue;
        }

        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(client->GetMixFormat(&mix))) {
            DeviceFormat format{};
            if (mix_format_to_device(mix, format)) {
                info.channels = format.channels;
                if (kind == OutputKind::WasapiShared) {
                    info.pcm_modes.push_back({format.rate, format.encoding, format.valid_bits});
                }
            }
            CoTaskMemFree(mix);
        }

        if (kind == OutputKind::WasapiExclusive) {
            const int channels = std::max(info.channels, 2) >= 2 ? 2 : 1;
            for (uint32_t rate : kRates) {
                for (const Candidate& candidate : kCandidates) {
                    DeviceFormat format{candidate.encoding, rate, channels, candidate.valid_bits};
                    const WAVEFORMATEXTENSIBLE wave = make_wave_format(format);
                    if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &wave.Format, nullptr) == S_OK) {
                        info.pcm_modes.push_back({rate, candidate.encoding, candidate.valid_bits});
                        break;
                    }
                }
            }
            info.channels = channels;
        }
        devices.push_back(std::move(info));
    }
    return devices;
}

double current_hardware_gain_db(const DeviceInfo& device) {
    if (device.kind == OutputKind::Asio || device.id.empty()) return device.hardware_gain_db;
    try {
        return read_hardware_gain_db(find_device(device.id).Get());
    } catch (const std::runtime_error&) {
        return device.hardware_gain_db;  // the open that follows reports what is wrong
    }
}

std::unique_ptr<Output> make_wasapi_output(const DeviceInfo& device, const OutputOptions& options) {
    return std::make_unique<WasapiOutput>(device, options);
}

}  // namespace arpegio
