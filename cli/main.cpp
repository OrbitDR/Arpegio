// Command line front end of the engine.
#include <windows.h>

#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "audio/quantizer.h"
#include "decode/sacd_iso.h"
#include "engine/player.h"
#include "library/library_store.h"
#include "library/track_source.h"
#include "output/output.h"

using namespace arpegio;

namespace {

void print_device(const DeviceInfo& device) {
    printf("%s | %s\n", output_kind_name(device.kind), device.name.c_str());
    if (!device.error.empty()) {
        printf("  unavailable: %s\n", device.error.c_str());
        return;
    }
    if (device.hardware_gain_db != 0.0) printf("  windows volume: %.1f dB\n", device.hardware_gain_db);
    printf("  channels: %d", device.channels);
    if (device.kind == OutputKind::Asio) {
        printf("  buffer: %d..%d, preferred %d", device.buffer_min, device.buffer_max, device.buffer_preferred);
    }
    printf("\n  pcm:");
    for (const PcmMode& mode : device.pcm_modes) printf(" %u/%s", mode.rate, wire_name(mode.encoding));
    printf("\n");
    if (!device.dsd_rates.empty()) {
        printf("  native dsd:");
        for (uint32_t rate : device.dsd_rates) printf(" %u", rate);
        printf("\n");
    }
    if (!device.channel_names.empty()) {
        printf("  outputs:");
        for (const auto& name : device.channel_names) printf(" [%s]", name.c_str());
        printf("\n");
    }
    if (!device.input_channel_names.empty()) {
        printf("  inputs:");
        for (const auto& name : device.input_channel_names) printf(" [%s]", name.c_str());
        printf("\n");
    }
}

int list_devices() {
    for (const DeviceInfo& device : list_asio_devices()) print_device(device);
    for (const DeviceInfo& device : list_wasapi_devices(OutputKind::WasapiExclusive)) print_device(device);
    for (const DeviceInfo& device : list_wasapi_devices(OutputKind::WasapiShared)) print_device(device);
    return 0;
}

std::string lower_utf8(const std::wstring& text) {
    std::wstring copy = text;
    CharLowerBuffW(copy.data(), static_cast<DWORD>(copy.size()));
    const int size = WideCharToMultiByte(CP_UTF8, 0, copy.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, copy.c_str(), -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::string lower_name(const std::string& utf8) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring wide(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), size);
    return lower_utf8(wide);
}

std::optional<DeviceInfo> find_device(OutputKind kind, const std::wstring& needle) {
    const auto devices = kind == OutputKind::Asio ? list_asio_devices() : list_wasapi_devices(kind);
    const std::string wanted = lower_utf8(needle);
    for (const DeviceInfo& device : devices) {
        if (lower_name(device.name).find(wanted) != std::string::npos) return device;
    }
    return std::nullopt;
}

int usage() {
    fputs("usage: arpegio-cli devices\n"
          "       arpegio-cli play FILE... --output asio|exclusive|shared --device TEXT\n"
          "                   [--dsd auto|native|dop|pcm] [--dop-confirmed] [--gain-db DB]\n"
          "                   [--first-channel N] [--period-ms N] [--buffer N] [--wire s32-24|s24]\n"
          "                   [--volume-db DB] [--resampler r8brain|soxr]\n"
          "                   [--start SECONDS] [--last SECONDS] [--seconds SECONDS] [--track N] [--multichannel]\n"
          "       arpegio-cli play --db DB --album ID [--from N] [same options as above]\n"
          "       arpegio-cli decode FILE [OUT.raw] [--seconds S]\n"
          "       arpegio-cli loopback FILE --device TEXT --first-channel N --capture-channel N\n",
          stderr);
    return 2;
}

int play(int argc, wchar_t** argv) {
    std::vector<QueueItem> queue;
    OutputKind kind = OutputKind::WasapiExclusive;
    std::wstring device_text;
    PlaybackPolicy policy;
    OutputOptions options;
    double start = 0.0;
    double limit = 0.0;
    int sacd_track = 0;
    bool multichannel = false;
    std::wstring wire;
    std::filesystem::path database;
    int64_t album = 0;
    size_t first = 0;
    double last = 0.0;
    double volume_db = 0.0;

    for (int i = 2; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == L"--output" && has_value) {
            const std::wstring value = argv[++i];
            kind = value == L"asio" ? OutputKind::Asio
                   : value == L"shared" ? OutputKind::WasapiShared : OutputKind::WasapiExclusive;
        } else if (arg == L"--device" && has_value) {
            device_text = argv[++i];
        } else if (arg == L"--dsd" && has_value) {
            const std::wstring value = argv[++i];
            policy.dsd = value == L"native" ? DsdPolicy::Native
                         : value == L"dop" ? DsdPolicy::Dop
                         : value == L"pcm" ? DsdPolicy::Pcm : DsdPolicy::Auto;
        } else if (arg == L"--dop-confirmed") {
            policy.dop_confirmed = true;
        } else if (arg == L"--gain-db" && has_value) {
            policy.dsd_to_pcm_gain_db = wcstod(argv[++i], nullptr);
        } else if (arg == L"--volume-db" && has_value) {
            volume_db = wcstod(argv[++i], nullptr);
        } else if (arg == L"--resampler" && has_value) {
            policy.resampler = std::wstring(argv[++i]) == L"soxr" ? ResamplerKind::Soxr : ResamplerKind::R8brain;
        } else if (arg == L"--first-channel" && has_value) {
            options.first_channel = _wtoi(argv[++i]);
        } else if (arg == L"--period-ms" && has_value) {
            options.period_ms = wcstoul(argv[++i], nullptr, 10);
        } else if (arg == L"--buffer" && has_value) {
            options.buffer_frames = _wtoi(argv[++i]);
        } else if (arg == L"--start" && has_value) {
            start = wcstod(argv[++i], nullptr);
        } else if (arg == L"--seconds" && has_value) {
            limit = wcstod(argv[++i], nullptr);
        } else if (arg == L"--track" && has_value) {
            sacd_track = _wtoi(argv[++i]);
        } else if (arg == L"--multichannel") {
            multichannel = true;
        } else if (arg == L"--wire" && has_value) {
            wire = argv[++i];
        } else if (arg == L"--db" && has_value) {
            database = argv[++i];
        } else if (arg == L"--album" && has_value) {
            album = _wtoi64(argv[++i]);
        } else if (arg == L"--from" && has_value) {
            first = wcstoul(argv[++i], nullptr, 10);
        } else if (arg == L"--last" && has_value) {
            last = wcstod(argv[++i], nullptr);
        } else if (arg.rfind(L"--", 0) == 0) {
            return usage();
        } else if (is_sacd_image(arg)) {
            const SacdDisc disc = read_sacd_disc(arg);
            size_t area = 0;
            for (size_t a = 0; a < disc.areas.size(); ++a) {
                if (disc.areas[a].stereo != multichannel) area = a;
            }
            const size_t tracks = disc.areas.empty() ? 0 : disc.areas[area].tracks.size();
            for (size_t t = 0; t < tracks; ++t) {
                if (sacd_track > 0 && t + 1 != static_cast<size_t>(sacd_track)) continue;
                QueueItem item;
                item.path = arg;
                item.open = [path = std::filesystem::path(arg), area, t] { return open_sacd_track(path, area, t); };
                queue.push_back(std::move(item));
            }
        } else {
            QueueItem item;
            item.path = arg;
            queue.push_back(std::move(item));
        }
    }
    if (!database.empty()) {
        LibraryStore store(database);
        for (const Track& track : store.album_tracks(album)) queue.push_back(make_queue_item(track));
    }
    if (first >= queue.size() || device_text.empty()) return usage();

    // Goes straight to the join with the next track.
    if (last > 0.0) {
        const QueueItem& item = queue[first];
        const auto decoder = item.open ? item.open() : open_decoder(item.path);
        start = std::max(0.0, decoder->format().seconds() - last);
    }

    auto device = find_device(kind, device_text);
    if (!device) {
        fputs("device not found\n", stderr);
        return 1;
    }
    if (!device->error.empty()) {
        fprintf(stderr, "device unavailable: %s\n", device->error.c_str());
        return 1;
    }
    // Forces the WASAPI container, to find out what a device accepts or recognizes as DoP.
    if (wire == L"s32-24" || wire == L"s24") {
        for (PcmMode& mode : device->pcm_modes) {
            mode.encoding = wire == L"s24" ? WireEncoding::S24 : WireEncoding::S32;
            mode.valid_bits = 24;
        }
    }
    if (device->hardware_gain_db != 0.0) {
        fprintf(stderr,
                "warning: the Windows volume of this device is at %.1f dB. Set it to 100 %% and use the volume on the DAC:\n"
                "         otherwise the output is not bit-perfect and DoP is not recognized.\n",
                device->hardware_gain_db);
    }

    Player player;
    player.set_device(*device, options);
    player.set_policy(policy);
    player.set_volume(std::pow(10.0, volume_db / 20.0));
    player.set_queue(std::move(queue));
    player.play(first, start);

    std::string path_shown;
    const DWORD begin = GetTickCount();
    while (!player.wait_until_stopped(1000)) {
        const PlayerStatus status = player.status();
        if (status.signal_path != path_shown) {
            path_shown = status.signal_path;
            printf("%s\n", path_shown.c_str());
        }
        printf("track %zu  %7.1f / %.1f s  buffer %3.0f%%  underruns %llu\n", status.queue_index + 1, status.position_seconds,
               status.duration_seconds, status.ring_capacity ? 100.0 * status.buffered_bytes / status.ring_capacity : 0.0,
               static_cast<unsigned long long>(status.underruns));
        fflush(stdout);
        if (limit > 0.0 && (GetTickCount() - begin) / 1000.0 >= limit) player.stop();
    }

    const PlayerStatus status = player.status();
    printf("callbacks %llu, late %llu, max gap %.1f ms, max service %.2f ms, period %u frames\n",
           static_cast<unsigned long long>(status.output.callbacks),
           static_cast<unsigned long long>(status.output.late_callbacks), status.output.max_gap_us / 1000.0,
           status.output.max_service_us / 1000.0, status.output.period_frames);
    printf("underruns %llu, clipped samples %llu, reopened %llu\n", static_cast<unsigned long long>(status.underruns),
           static_cast<unsigned long long>(status.clipped_samples), static_cast<unsigned long long>(status.reopened));
    if (!status.error.empty()) {
        fprintf(stderr, "error: %s\n", status.error.c_str());
        return 1;
    }
    return 0;
}

// Decodes without an output device: reports the format and speed, and can dump the samples.
// PCM goes out as little-endian S32, DSD as planar bytes.
int decode(int argc, wchar_t** argv) {
    const wchar_t* in = nullptr;
    const wchar_t* out = nullptr;
    double seconds = 0.0;
    for (int i = 2; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--seconds" && i + 1 < argc) seconds = wcstod(argv[++i], nullptr);
        else if (!in) in = argv[i];
        else out = argv[i];
    }
    if (!in) return usage();

    try {
        auto decoder = open_decoder(in);
        const SourceFormat format = decoder->format();
        printf("%s %u Hz, %d ch, %d bits, %.2f s\n", format.dsd ? "DSD" : "PCM", format.rate, format.channels, format.bits,
               format.seconds());

        FILE* dump = out ? _wfopen(out, L"wb") : nullptr;
        const double limit = seconds > 0.0 ? seconds : format.seconds() + 1.0;
        const LARGE_INTEGER zero{};
        LARGE_INTEGER frequency = zero, begin = zero, end = zero;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&begin);
        double decoded = 0.0;
        double peak = 0.0;
        if (format.dsd) {
            constexpr size_t kChunk = 1 << 16;
            std::vector<std::vector<uint8_t>> planes(format.channels, std::vector<uint8_t>(kChunk));
            std::vector<uint8_t*> pointers;
            for (auto& plane : planes) pointers.push_back(plane.data());
            while (decoded < limit) {
                const size_t got = decoder->read_dsd(pointers.data(), kChunk);
                if (got == 0) break;
                decoded += got * 8.0 / format.rate;
                if (dump) {
                    for (const auto& plane : planes) fwrite(plane.data(), 1, got, dump);
                }
            }
        } else {
            constexpr size_t kFrames = 1 << 14;
            std::vector<double> samples(kFrames * format.channels);
            std::vector<int32_t> integers(samples.size());
            Quantizer quantizer(WireEncoding::S32, 0);
            while (decoded < limit) {
                const size_t got = decoder->read_pcm(samples.data(), kFrames);
                if (got == 0) break;
                decoded += static_cast<double>(got) / format.rate;
                for (size_t i = 0; i < got * format.channels; ++i) peak = std::max(peak, std::abs(samples[i]));
                if (dump) {
                    quantizer.convert(samples.data(), got * format.channels, reinterpret_cast<uint8_t*>(integers.data()));
                    fwrite(integers.data(), 4, got * format.channels, dump);
                }
            }
        }
        if (dump) fclose(dump);
        QueryPerformanceCounter(&end);
        const double elapsed = static_cast<double>(end.QuadPart - begin.QuadPart) / frequency.QuadPart;
        printf("decoded %.2f s in %.2f s (%.0fx), peak %.2f dBFS\n", decoded, elapsed, decoded / std::max(elapsed, 1e-6),
               peak > 0.0 ? 20.0 * std::log10(peak) : -999.0);
    } catch (const std::exception& error) {
        fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
    return 0;
}

// Plays a PCM file through an ASIO loopback and compares what came back, sample by sample.
int loopback(int argc, wchar_t** argv) {
    std::wstring device_text;
    std::wstring file;
    OutputOptions options;
    for (int i = 2; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == L"--device" && has_value) device_text = argv[++i];
        else if (arg == L"--first-channel" && has_value) options.first_channel = _wtoi(argv[++i]);
        else if (arg == L"--capture-channel" && has_value) options.capture_channel = _wtoi(argv[++i]);
        else if (arg == L"--buffer" && has_value) options.buffer_frames = _wtoi(argv[++i]);
        else file = arg;
    }
    if (file.empty() || device_text.empty() || options.capture_channel < 0) return usage();

    // Expected samples, through the same conversion the engine uses.
    auto decoder = open_decoder(file);
    const SourceFormat source = decoder->format();
    if (source.dsd || source.channels != 2) {
        fputs("loopback needs a stereo PCM file\n", stderr);
        return 1;
    }
    std::vector<double> decoded(static_cast<size_t>(source.length) * 2);
    const size_t frames = decoder->read_pcm(decoded.data(), static_cast<size_t>(source.length));
    std::vector<int32_t> expected(frames * 2);
    Quantizer quantizer(WireEncoding::S32, 0);
    quantizer.convert(decoded.data(), frames * 2, reinterpret_cast<uint8_t*>(expected.data()));
    decoded = {};

    const auto device = find_device(OutputKind::Asio, device_text);
    if (!device || !device->error.empty()) {
        fputs("device not available\n", stderr);
        return 1;
    }

    std::atomic<size_t> written{0};
    std::vector<int32_t> captured((frames + static_cast<size_t>(source.rate) * 3) * 2, 0);
    options.capture_buffer = captured.data();
    options.capture_frames = captured.size() / 2;
    options.capture_written = &written;

    Player player;
    player.set_device(*device, options);
    QueueItem item;
    item.path = file;
    player.set_queue({std::move(item)});
    player.play();
    while (!player.wait_until_stopped(500)) {
    }
    const PlayerStatus status = player.status();
    printf("%s\n", status.signal_path.c_str());
    printf("callbacks %llu, late %llu, max gap %.1f ms, underruns %llu\n",
           static_cast<unsigned long long>(status.output.callbacks),
           static_cast<unsigned long long>(status.output.late_callbacks), status.output.max_gap_us / 1000.0,
           static_cast<unsigned long long>(status.underruns));
    if (!status.error.empty()) {
        fprintf(stderr, "error: %s\n", status.error.c_str());
        return 1;
    }

    const size_t got = written.load();
    size_t start = 0;
    while (start < got && captured[start * 2] == 0 && captured[start * 2 + 1] == 0) ++start;
    printf("captured %zu frames, first audio at frame %zu\n", got, start);

    // The file may begin with zeros, so alignment starts at its first non-zero frame.
    size_t first_file = 0;
    while (first_file < frames && expected[first_file * 2] == 0 && expected[first_file * 2 + 1] == 0) ++first_file;

    size_t compared = 0;
    size_t differing = 0;
    size_t differing_24 = 0;
    int64_t worst = 0;
    for (size_t frame = first_file; frame < frames && start + frame - first_file < got; ++frame) {
        const size_t at = (start + frame - first_file) * 2;
        for (int ch = 0; ch < 2; ++ch) {
            const int64_t heard = captured[at + ch];
            const int64_t wanted = expected[frame * 2 + ch];
            if (heard != wanted) {
                ++differing;
                worst = std::max<int64_t>(worst, heard > wanted ? heard - wanted : wanted - heard);
            }
            if (((heard + 128) >> 8) != ((wanted + 128) >> 8)) ++differing_24;
        }
        compared += 2;
    }
    printf("compared %zu samples: %zu differ at 32 bits (worst %lld units), %zu differ at 24 bits\n", compared, differing,
           static_cast<long long>(worst), differing_24);
    const bool identical = compared > 0 && differing_24 == 0;
    printf(identical ? (differing == 0 ? "BIT-PERFECT at 32 bits\n" : "BIT-PERFECT at 24 bits\n") : "NOT bit-perfect\n");
    return identical ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int status = 2;
    const std::wstring command = argc > 1 ? argv[1] : L"";
    if (command == L"devices") {
        status = list_devices();
    } else if (command == L"play") {
        try {
            status = play(argc, argv);
        } catch (const std::exception& error) {
            fprintf(stderr, "error: %s\n", error.what());
            status = 1;
        }
    } else if (command == L"decode") {
        status = decode(argc, argv);
    } else if (command == L"loopback") {
        status = loopback(argc, argv);
    } else {
        status = usage();
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return status;
}
