// Prints a SACD image's table of contents, and with --verify decodes whole tracks and hashes the
// DSD bytes, so the decoder can be compared against another implementation.
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "decode/framed_dsd.h"
#include "decode/sacd_iso.h"

using namespace arpegio;

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        fputs("usage: arpegio-sacd IMAGE.iso [--verify AREA TRACK [--dump FILE]]\n", stderr);
        return 2;
    }
    try {
        const SacdDisc disc = read_sacd_disc(argv[1]);
        printf("album: %s | %s | %d | disc %d/%d\n", disc.album_title.c_str(), disc.album_artist.c_str(), disc.year,
               disc.disc_number, disc.disc_total);
        for (size_t a = 0; a < disc.areas.size(); ++a) {
            const SacdArea& area = disc.areas[a];
            printf("area %zu: %s %d ch, %u Hz, %s, lsn %u..%u, %zu tracks\n", a, area.stereo ? "stereo" : "multichannel",
                   area.channels, area.rate, area.dst ? "DST" : "plain DSD", area.first_lsn, area.last_lsn, area.tracks.size());
            for (size_t t = 0; t < area.tracks.size() && t < 40; ++t) {
                const SacdTrack& track = area.tracks[t];
                printf("  %2zu  %5.1f s  start %6u  lsn %8u+%u  %s | %s\n", t + 1, track.duration_frames / 75.0,
                       track.start_frame, track.first_lsn, track.length_lsn, track.title.c_str(), track.performer.c_str());
            }
        }

        if (argc >= 5 && std::wstring(argv[2]) == L"--verify") {
            const size_t area = _wtoi(argv[3]);
            const size_t track = _wtoi(argv[4]);
            FILE* dump = nullptr;
            if (argc >= 7 && std::wstring(argv[5]) == L"--dump") dump = _wfopen(argv[6], L"wb");

            auto decoder = open_sacd_track(argv[1], area, track);
            const SourceFormat format = decoder->format();
            constexpr size_t kChunk = 1 << 16;
            std::vector<std::vector<uint8_t>> planes(format.channels, std::vector<uint8_t>(kChunk));
            std::vector<uint8_t*> pointers;
            for (auto& plane : planes) pointers.push_back(plane.data());

            uint64_t hash = 14695981039346656037ull;
            uint64_t total = 0;
            const auto begin = std::chrono::steady_clock::now();
            for (;;) {
                const size_t got = decoder->read_dsd(pointers.data(), kChunk);
                if (got == 0) break;
                total += got;
                for (int ch = 0; ch < format.channels; ++ch) {
                    if (dump) fwrite(planes[ch].data(), 1, got, dump);
                    for (size_t i = 0; i < got; ++i) hash = (hash ^ planes[ch][i]) * 1099511628211ull;
                }
            }
            if (dump) fclose(dump);
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
            const double seconds = total * 8.0 / format.rate;
            const auto* framed = dynamic_cast<const FramedDsdDecoder*>(decoder.get());
            printf("decoded %.2f s of %.2f s in %.2f s (%.1fx real time), damaged frames %llu, hash %016llx\n", seconds,
                   format.seconds(), elapsed, seconds / elapsed,
                   static_cast<unsigned long long>(framed ? framed->damaged_frames() : 0),
                   static_cast<unsigned long long>(hash));
        }
    } catch (const std::exception& error) {
        fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
    return 0;
}
