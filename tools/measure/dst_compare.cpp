// Compares Arpegio's DST decoder with libdstdec from foo_input_sacd, frame by frame, on a real disc.
// Not part of the normal build: libdstdec is only a reference here. Build with dst_compare.bat.
// Usage: dst_compare IMAGE.iso AREA TRACK     (AREA and TRACK count from 0)
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "decoder/decoder.h"
#include "decode/framed_dsd.h"
#include "decode/sacd_iso.h"
#include "dsd/dst_decoder.h"

using namespace arpegio;

// libdstdec expects its host to provide this.
void log_printf(const char*, ...) {}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 4) {
        fputs("usage: dst_compare IMAGE.iso AREA TRACK\n", stderr);
        return 2;
    }
    constexpr uint64_t kMaxFrames = 6000;  // 80 seconds

    const size_t area_index = _wtoi(argv[2]);
    const size_t track_index = _wtoi(argv[3]);
    const SacdDisc disc = read_sacd_disc(argv[1]);
    const SacdArea& area = disc.areas[area_index];

    auto decoder = open_sacd_track(argv[1], area_index, track_index);
    auto* framed = dynamic_cast<FramedDsdDecoder*>(decoder.get());

    DstDecoder ours(area.channels, area.rate);
    dst::decoder_t reference;
    const unsigned frame_bytes = static_cast<unsigned>(ours.frame_bytes());
    if (reference.init(area.channels, frame_bytes) != 0) {
        fputs("reference init failed\n", stderr);
        return 1;
    }

    std::vector<uint8_t> raw;
    std::vector<uint8_t> a(frame_bytes * area.channels);
    std::vector<uint8_t> b(frame_bytes * area.channels);
    uint64_t frames = 0;
    uint64_t different = 0;
    uint64_t failed = 0;
    uint64_t plain = 0;
    double ours_time = 0.0;
    double reference_time = 0.0;
    bool compressed = false;
    while (frames < kMaxFrames && framed->read_raw_frame(raw, compressed)) {
        ++frames;
        if (!compressed) {
            ++plain;
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = ours.decode(raw.data(), raw.size(), a.data());
        const auto t1 = std::chrono::steady_clock::now();
        reference.run(raw.data(), static_cast<unsigned>(raw.size() * 8), b.data());
        const auto t2 = std::chrono::steady_clock::now();
        ours_time += std::chrono::duration<double>(t1 - t0).count();
        reference_time += std::chrono::duration<double>(t2 - t1).count();
        if (!ok) {
            ++failed;
        } else if (std::memcmp(a.data(), b.data(), a.size()) != 0) {
            ++different;
        }
    }
    const double seconds = frames / 75.0;
    printf("area %zu track %zu '%s', %d ch: %llu frames, %llu plain, %llu different, %llu failed | ours %.1fx, reference %.1fx\n",
           area_index, track_index, area.tracks[track_index].title.c_str(), area.channels,
           static_cast<unsigned long long>(frames), static_cast<unsigned long long>(plain),
           static_cast<unsigned long long>(different), static_cast<unsigned long long>(failed), seconds / ours_time,
           seconds / reference_time);
    return different == 0 && failed == 0 ? 0 : 1;
}
