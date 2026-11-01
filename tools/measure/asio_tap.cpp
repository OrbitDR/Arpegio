// A pass-through ASIO driver for comparing players. It takes the place of a real driver through a
// per-user COM override, forwards every call to it and records what the host writes into the
// output buffers, so two programs can be compared byte for byte at the same interface, on the same
// device and with their own settings, without touching either of them.
//
// Everything lives beside the DLL: asio_tap.ini and the captures folder. Each buffer pair gives
// NAME-n.raw, interleaved as the host wrote it, NAME-n.info with its format, and NAME.log with the
// calls the host made. With mute=1 under [tap] the host writes to private buffers and the driver
// plays silence.
#include <windows.h>

#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "asiosys.h"

#include "asio.h"
#include "iasiodrv.h"

#pragma comment(linker, "/EXPORT:DllGetClassObject,PRIVATE")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow,PRIVATE")

namespace {

constexpr size_t kRingBytes = size_t{1} << 28;
constexpr uint64_t kMaxCaptureBytes = uint64_t{4} << 30;  // a forgotten tap must not fill the disk
constexpr uint8_t kDsdSilenceByte = 0x69;
constexpr DWORD kWriterPollMs = 20;

HMODULE g_module = nullptr;

std::filesystem::path tap_folder() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_module, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

bool mute_requested() {
    const std::wstring ini = (tap_folder() / L"asio_tap.ini").wstring();
    return GetPrivateProfileIntW(L"tap", L"mute", 0, ini.c_str()) != 0;
}

std::wstring host_name() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).stem().wstring();
}

// One name per process, shared by its log and its captures.
const std::filesystem::path& session_path() {
    static const std::filesystem::path path = [] {
        const std::filesystem::path folder = tap_folder() / L"captures";
        std::error_code ignored;
        std::filesystem::create_directories(folder, ignored);
        SYSTEMTIME now;
        GetLocalTime(&now);
        wchar_t stamp[64];
        swprintf_s(stamp, L"-%04d%02d%02d-%02d%02d%02d-%lu", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                   now.wSecond, GetCurrentProcessId());
        return folder / (host_name() + stamp);
    }();
    return path;
}

class Log {
public:
    void write(const char* format, ...) {
        std::lock_guard lock(mutex_);
        if (!file_) file_ = _wfopen((session_path().wstring() + L".log").c_str(), L"a");
        if (!file_) return;
        SYSTEMTIME now;
        GetLocalTime(&now);
        std::fprintf(file_, "%02d:%02d:%02d.%03d ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
        va_list arguments;
        va_start(arguments, format);
        std::vfprintf(file_, format, arguments);
        va_end(arguments);
        std::fputc('\n', file_);
        std::fflush(file_);
    }

private:
    std::mutex mutex_;
    std::FILE* file_ = nullptr;
};

Log g_log;

// The callback pushes, a thread writes. Nothing here may block the driver's thread.
class Capture {
public:
    explicit Capture(const std::wstring& path) : ring_(kRingBytes), file_(_wfopen(path.c_str(), L"wb")) {
        writer_ = std::thread([this] { run(); });
    }

    ~Capture() {
        stop_.store(true, std::memory_order_release);
        writer_.join();
        if (file_) std::fclose(file_);
    }

    void push(const uint8_t* data, size_t bytes) noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) + bytes > ring_.size()) {
            dropped_.fetch_add(bytes, std::memory_order_relaxed);
            return;
        }
        const size_t at = static_cast<size_t>(head & (ring_.size() - 1));
        const size_t first = std::min(bytes, ring_.size() - at);
        std::memcpy(ring_.data() + at, data, first);
        std::memcpy(ring_.data(), data + first, bytes - first);
        head_.store(head + bytes, std::memory_order_release);
    }

    uint64_t written() const { return written_; }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    void run() {
        for (;;) {
            const bool stopping = stop_.load(std::memory_order_acquire);
            const uint64_t head = head_.load(std::memory_order_acquire);
            uint64_t tail = tail_.load(std::memory_order_relaxed);
            while (tail != head) {
                const size_t at = static_cast<size_t>(tail & (ring_.size() - 1));
                const size_t chunk = static_cast<size_t>(std::min<uint64_t>(head - tail, ring_.size() - at));
                if (file_ && written_ < kMaxCaptureBytes) {
                    std::fwrite(ring_.data() + at, 1, chunk, file_);
                    written_ += chunk;
                }
                tail += chunk;
                tail_.store(tail, std::memory_order_release);
            }
            if (stopping) return;
            Sleep(kWriterPollMs);
        }
    }

    std::vector<uint8_t> ring_;
    std::FILE* file_;
    std::thread writer_;
    std::atomic<uint64_t> head_{0};
    std::atomic<uint64_t> tail_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<bool> stop_{false};
    uint64_t written_ = 0;
};

size_t sample_bytes(ASIOSampleType type) {
    switch (type) {
    case ASIOSTInt16LSB:
    case ASIOSTInt16MSB: return 2;
    case ASIOSTInt24LSB:
    case ASIOSTInt24MSB: return 3;
    case ASIOSTFloat64LSB:
    case ASIOSTFloat64MSB: return 8;
    case ASIOSTDSDInt8LSB1:
    case ASIOSTDSDInt8MSB1: return 1;
    default: return 4;
    }
}

bool is_dsd(ASIOSampleType type) {
    return type == ASIOSTDSDInt8LSB1 || type == ASIOSTDSDInt8MSB1;
}

std::wstring real_server(REFCLSID clsid) {
    wchar_t text[64];
    StringFromGUID2(clsid, text, 64);
    const std::wstring key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + text + L"\\InprocServer32";
    wchar_t path[MAX_PATH] = {};
    DWORD size = sizeof(path);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS) {
        return {};
    }
    return path;
}

// Bypasses COM, which would find this DLL again.
IASIO* load_real_driver(REFCLSID clsid, HMODULE& module) {
    const std::wstring path = real_server(clsid);
    if (path.empty()) return nullptr;
    module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) return nullptr;
    const auto get_class = reinterpret_cast<HRESULT(__stdcall*)(REFCLSID, REFIID, void**)>(
        reinterpret_cast<void*>(GetProcAddress(module, "DllGetClassObject")));
    IClassFactory* factory = nullptr;
    if (!get_class || FAILED(get_class(clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory)))) return nullptr;
    IASIO* driver = nullptr;
    const HRESULT result = factory->CreateInstance(nullptr, clsid, reinterpret_cast<void**>(&driver));
    factory->Release();
    return SUCCEEDED(result) ? driver : nullptr;
}

class Tap;
std::atomic<Tap*> g_tap{nullptr};  // ASIO callbacks carry no context argument
std::atomic<int> g_captures{0};

class Tap final : public IASIO {
public:
    Tap(REFCLSID clsid, IASIO* real, HMODULE module) : clsid_(clsid), real_(real), module_(module), mute_(mute_requested()) {
        g_log.write("tap loaded, mute=%d", mute_ ? 1 : 0);
    }

    Tap(const Tap&) = delete;
    Tap& operator=(const Tap&) = delete;

    ~Tap() {
        finish_capture();
        real_->Release();
        FreeLibrary(module_);
    }

    HRESULT __stdcall QueryInterface(REFIID iid, void** object) override {
        if (iid == IID_IUnknown || iid == clsid_) {
            AddRef();
            *object = static_cast<IASIO*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG __stdcall AddRef() override { return references_.fetch_add(1) + 1; }

    ULONG __stdcall Release() override {
        const ULONG left = references_.fetch_sub(1) - 1;
        if (left == 0) delete this;
        return left;
    }

    ASIOBool init(void* handle) override {
        const ASIOBool result = real_->init(handle);
        g_log.write("init -> %ld", static_cast<long>(result));
        return result;
    }

    void getDriverName(char* name) override { real_->getDriverName(name); }
    long getDriverVersion() override { return real_->getDriverVersion(); }
    void getErrorMessage(char* text) override { real_->getErrorMessage(text); }

    ASIOError start() override {
        callbacks_ = 0;
        const ASIOError code = real_->start();
        g_log.write("start -> %ld", code);
        return code;
    }

    ASIOError stop() override {
        const ASIOError code = real_->stop();
        g_log.write("stop -> %ld | callbacks %llu, with time info %llu, outputReady %llu, longest host callback %llu us",
                    code, callbacks_, time_callbacks_, ready_calls_, to_microseconds(longest_callback_));
        return code;
    }

    ASIOError getChannels(long* inputs, long* outputs) override { return real_->getChannels(inputs, outputs); }
    ASIOError getLatencies(long* input, long* output) override {
        const ASIOError code = real_->getLatencies(input, output);
        g_log.write("getLatencies -> %ld, input %ld, output %ld", code, *input, *output);
        return code;
    }

    ASIOError getBufferSize(long* minimum, long* maximum, long* preferred, long* granularity) override {
        const ASIOError code = real_->getBufferSize(minimum, maximum, preferred, granularity);
        g_log.write("getBufferSize -> %ld, min %ld, max %ld, preferred %ld, granularity %ld", code, *minimum, *maximum,
                    *preferred, *granularity);
        return code;
    }

    ASIOError canSampleRate(ASIOSampleRate rate) override { return real_->canSampleRate(rate); }
    ASIOError getSampleRate(ASIOSampleRate* rate) override { return real_->getSampleRate(rate); }

    ASIOError setSampleRate(ASIOSampleRate rate) override {
        const ASIOError code = real_->setSampleRate(rate);
        g_log.write("setSampleRate %.0f -> %ld", rate, code);
        rate_ = rate;
        return code;
    }

    ASIOError getClockSources(ASIOClockSource* clocks, long* count) override { return real_->getClockSources(clocks, count); }
    ASIOError setClockSource(long reference) override { return real_->setClockSource(reference); }
    ASIOError getSamplePosition(ASIOSamples* position, ASIOTimeStamp* stamp) override {
        return real_->getSamplePosition(position, stamp);
    }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override { return real_->getChannelInfo(info); }

    ASIOError createBuffers(ASIOBufferInfo* infos, long channels, long size, ASIOCallbacks* callbacks) override {
        host_ = *callbacks;
        wrapped_.bufferSwitch = &Tap::on_switch;
        wrapped_.sampleRateDidChange = &Tap::on_sample_rate;
        wrapped_.asioMessage = &Tap::on_message;
        wrapped_.bufferSwitchTimeInfo = &Tap::on_switch_time;
        g_tap.store(this, std::memory_order_release);
        const ASIOError code = real_->createBuffers(infos, channels, size, &wrapped_);
        g_log.write("createBuffers channels %ld, size %ld -> %ld", channels, size, code);
        if (code == ASE_OK) {
            begin_capture(infos, channels, size);
        } else {
            g_tap.store(nullptr, std::memory_order_release);
        }
        return code;
    }

    ASIOError disposeBuffers() override {
        const ASIOError code = real_->disposeBuffers();
        g_log.write("disposeBuffers -> %ld", code);
        g_tap.store(nullptr, std::memory_order_release);
        finish_capture();
        return code;
    }

    ASIOError controlPanel() override { return real_->controlPanel(); }

    ASIOError future(long selector, void* option) override {
        const ASIOError code = real_->future(selector, option);
        const bool has_format = selector == kAsioCanDoIoFormat || selector == kAsioGetIoFormat || selector == kAsioSetIoFormat;
        g_log.write("future %ld, format %ld -> %ld", selector,
                    has_format ? static_cast<long>(static_cast<ASIOIoFormat*>(option)->FormatType) : -1L, code);
        return code;
    }

    ASIOError outputReady() override {
        ++ready_calls_;
        return real_->outputReady();
    }

private:
    struct OutputChannel {
        uint8_t* host[2] = {};
        std::vector<uint8_t> shadow[2];
    };

    void begin_capture(ASIOBufferInfo* infos, long channels, long size) {
        outputs_.clear();
        ASIOSampleType type = ASIOSTInt32LSB;
        for (long i = 0; i < channels; ++i) {
            if (infos[i].isInput) continue;
            ASIOChannelInfo info{};
            info.channel = infos[i].channelNum;
            info.isInput = ASIOFalse;
            real_->getChannelInfo(&info);
            type = info.type;
        }
        bytes_ = sample_bytes(type);
        // Buffer sizes count samples. A DSD sample is one bit, packed 8 to a byte.
        frames_ = is_dsd(type) ? static_cast<size_t>(size) / 8 : static_cast<size_t>(size);
        const size_t half_bytes = frames_ * bytes_;
        for (long i = 0; i < channels; ++i) {
            if (infos[i].isInput) continue;
            OutputChannel& channel = outputs_.emplace_back();
            for (int half = 0; half < 2; ++half) {
                if (mute_) {
                    std::memset(infos[i].buffers[half], is_dsd(type) ? kDsdSilenceByte : 0, half_bytes);
                    channel.shadow[half].assign(half_bytes, 0);
                    infos[i].buffers[half] = channel.shadow[half].data();
                }
                channel.host[half] = static_cast<uint8_t*>(infos[i].buffers[half]);
            }
        }
        interleaved_.assign(half_bytes * outputs_.size(), 0);

        const std::wstring base = session_path().wstring() + L"-" + std::to_wstring(g_captures.fetch_add(1) + 1);
        capture_ = std::make_unique<Capture>(base + L".raw");
        if (std::FILE* info = _wfopen((base + L".info").c_str(), L"w")) {
            std::fprintf(info, "rate=%.0f\ntype=%ld\nsample_bytes=%zu\nchannels=%zu\nbuffer=%ld\nmute=%d\n", rate_,
                         static_cast<long>(type), bytes_, outputs_.size(), size, mute_ ? 1 : 0);
            std::fclose(info);
        }
        g_log.write("capture started: sample type %ld, %zu bytes, %zu channels, %zu frames per half", static_cast<long>(type),
                    bytes_, outputs_.size(), frames_);
    }

    void finish_capture() {
        if (!capture_) return;
        const uint64_t dropped = capture_->dropped();
        capture_.reset();
        g_log.write("capture closed, %llu bytes dropped", dropped);
    }

    // The half the driver has just taken is the one the host finished last, whether it filled it
    // inside the callback or from a thread of its own.
    void capture_half(long half) noexcept {
        uint8_t* write = interleaved_.data();
        for (size_t frame = 0; frame < frames_; ++frame) {
            for (const OutputChannel& channel : outputs_) {
                std::memcpy(write, channel.host[half] + frame * bytes_, bytes_);
                write += bytes_;
            }
        }
        capture_->push(interleaved_.data(), interleaved_.size());
    }

    void enter(long index) noexcept {
        QueryPerformanceCounter(&entered_);
        ++callbacks_;
        if (capture_) capture_half(1 - index);
    }

    void leave() noexcept {
        LARGE_INTEGER left;
        QueryPerformanceCounter(&left);
        longest_callback_ = std::max<int64_t>(longest_callback_, left.QuadPart - entered_.QuadPart);
    }

    static unsigned long long to_microseconds(int64_t ticks) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return static_cast<unsigned long long>(ticks) * 1000000 / static_cast<unsigned long long>(frequency.QuadPart);
    }

    static void on_switch(long index, ASIOBool direct) {
        if (Tap* self = g_tap.load(std::memory_order_acquire)) {
            self->enter(index);
            self->host_.bufferSwitch(index, direct);
            self->leave();
        }
    }

    static ASIOTime* on_switch_time(ASIOTime* time, long index, ASIOBool direct) {
        Tap* self = g_tap.load(std::memory_order_acquire);
        if (!self) return nullptr;
        self->enter(index);
        ++self->time_callbacks_;
        ASIOTime* result = self->host_.bufferSwitchTimeInfo(time, index, direct);
        self->leave();
        return result;
    }

    static void on_sample_rate(ASIOSampleRate rate) {
        g_log.write("sampleRateDidChange %.0f", rate);
        if (Tap* self = g_tap.load(std::memory_order_acquire)) self->host_.sampleRateDidChange(rate);
    }

    static long on_message(long selector, long value, void* message, double* option) {
        Tap* self = g_tap.load(std::memory_order_acquire);
        const long result = self ? self->host_.asioMessage(selector, value, message, option) : 0;
        g_log.write("asioMessage %ld, value %ld -> %ld", selector, value, result);
        return result;
    }

    const CLSID clsid_;
    IASIO* real_;
    HMODULE module_;
    const bool mute_;
    std::atomic<ULONG> references_{1};
    ASIOCallbacks host_{};
    ASIOCallbacks wrapped_{};
    double rate_ = 0.0;
    std::vector<OutputChannel> outputs_;
    std::vector<uint8_t> interleaved_;
    std::unique_ptr<Capture> capture_;
    size_t frames_ = 0;
    size_t bytes_ = 4;
    LARGE_INTEGER entered_{};
    int64_t longest_callback_ = 0;
    unsigned long long callbacks_ = 0;
    unsigned long long time_callbacks_ = 0;
    unsigned long long ready_calls_ = 0;
};

class Factory final : public IClassFactory {
public:
    explicit Factory(REFCLSID clsid) : clsid_(clsid) {}

    HRESULT __stdcall QueryInterface(REFIID iid, void** object) override {
        if (iid == IID_IUnknown || iid == IID_IClassFactory) {
            AddRef();
            *object = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG __stdcall AddRef() override { return references_.fetch_add(1) + 1; }

    ULONG __stdcall Release() override {
        const ULONG left = references_.fetch_sub(1) - 1;
        if (left == 0) delete this;
        return left;
    }

    HRESULT __stdcall CreateInstance(IUnknown* outer, REFIID iid, void** object) override {
        *object = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        HMODULE module = nullptr;
        IASIO* real = load_real_driver(clsid_, module);
        if (!real) {
            g_log.write("the real driver could not be loaded");
            return E_FAIL;
        }
        Tap* tap = new Tap(clsid_, real, module);
        const HRESULT result = tap->QueryInterface(iid, object);
        tap->Release();
        return result;
    }

    HRESULT __stdcall LockServer(BOOL) override { return S_OK; }

private:
    const CLSID clsid_;
    std::atomic<ULONG> references_{1};
};

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) g_module = module;
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** object) {
    Factory* factory = new Factory(clsid);
    const HRESULT result = factory->QueryInterface(iid, object);
    factory->Release();
    return result;
}

STDAPI DllCanUnloadNow() {
    return S_FALSE;  // a host may still be inside a callback
}
