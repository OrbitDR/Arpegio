// Works on a library database from the command line, to check the catalogue against a real collection.
#include <windows.h>

#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "decode/decoder.h"
#include "library/artwork.h"
#include "library/library_store.h"
#include "library/paths.h"
#include "library/scanner.h"
#include "library/track_source.h"

using namespace arpegio;

namespace {

constexpr size_t kProbeFrames = 4096;

std::string utf8_argument(const wchar_t* text) {
    return path_to_utf8(std::filesystem::path(text));
}

void print_track(const Track& track) {
    printf("%6lld  %02d.%02d  %7.1f s  %-8s %7u Hz %2d ch  %s - %s [%s]\n", static_cast<long long>(track.id),
           track.disc_number, track.track_number, track.format.seconds(), track.codec.c_str(), track.format.rate,
           track.format.channels, track.artist.c_str(), track.title.c_str(), track.album.c_str());
}

int scan(LibraryStore& store, const std::filesystem::path& cache, const std::wstring& folder, int argc, wchar_t** argv) {
    ScanOptions options;
    bool verbose = false;
    for (int i = 0; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--force") options.force = true;
        else if (arg == L"--no-artwork") options.extract_artwork = false;
        else if (arg == L"--multichannel") options.sacd_multichannel = true;
        else if (arg == L"--verbose") verbose = true;
    }

    ArtworkCache artwork(cache);
    const auto begin = std::chrono::steady_clock::now();
    const ScanResult result =
        scan_folder(store, artwork, folder, options, [verbose](uint64_t done, uint64_t total, const std::string& path) {
            if (verbose) {
                fprintf(stderr, "%s\n", path.c_str());
                fflush(stderr);
            } else if (done % 250 == 0 || done == total) {
                fprintf(stderr, "\r%llu / %llu", static_cast<unsigned long long>(done),
                        static_cast<unsigned long long>(total));
            }
        });
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();

    fprintf(stderr, "\n");
    for (const std::string& warning : result.warnings) printf("warning: %s\n", warning.c_str());
    printf(
        "%.2f s: %llu files, %llu unchanged, %llu tracks written, %llu removed, %llu CUE sheets, %llu duplicate sheets, "
        "%llu images ignored, %zu warnings\n",
        seconds, static_cast<unsigned long long>(result.files), static_cast<unsigned long long>(result.unchanged),
        static_cast<unsigned long long>(result.tracks_written), static_cast<unsigned long long>(result.tracks_removed),
        static_cast<unsigned long long>(result.cue_sheets), static_cast<unsigned long long>(result.duplicate_sheets),
        static_cast<unsigned long long>(result.ignored_images), result.warnings.size());
    return 0;
}

int stats(LibraryStore& store) {
    const std::vector<Track> tracks = store.tracks();
    std::map<std::string, int> codecs;
    double seconds = 0.0;
    int with_artwork = 0;
    int cue = 0;
    int sacd = 0;
    int dsd = 0;
    int mqa = 0;
    int no_album_artwork = 0;
    for (const Track& track : tracks) {
        ++codecs[track.codec];
        seconds += track.format.seconds();
        with_artwork += track.artwork_id != 0;
        cue += track.range_end != 0;
        sacd += track.sacd_area >= 0;
        dsd += track.format.dsd;
        mqa += track.mqa;
    }
    const std::vector<Album> albums = store.albums();
    int single_track_albums = 0;
    std::map<std::string, int> sections;
    for (const Album& album : albums) {
        no_album_artwork += album.artwork_id == 0;
        single_track_albums += album.track_count == 1;
        ++sections[section_name(album.section)];
    }
    printf("schema %d\n%zu tracks, %zu albums, %zu artists, %.1f hours\n", store.schema_version(), tracks.size(),
           albums.size(), store.artists().size(), seconds / 3600.0);
    printf("%d DSD, %d MQA, %d from CUE sheets, %d from SACD images, %d with artwork\n", dsd, mqa, cue, sacd, with_artwork);
    printf("%d albums without artwork, %d albums with a single track\n", no_album_artwork, single_track_albums);
    for (const auto& [codec, count] : codecs) printf("%6d  %s\n", count, codec.c_str());
    for (const auto& [section, count] : sections) printf("%6d  albums in %s\n", count, section.c_str());
    return 0;
}

// Reads exactly `count` positions, as doubles or as DSD bytes in one flat list.
std::vector<double> read_block(Decoder& decoder, size_t count) {
    const SourceFormat format = decoder.format();
    const size_t channels = format.channels;
    std::vector<double> out;
    std::vector<double> pcm(count * channels);
    std::vector<std::vector<uint8_t>> planes(channels, std::vector<uint8_t>(count));
    std::vector<uint8_t*> pointers;
    for (auto& plane : planes) pointers.push_back(plane.data());

    while (out.size() < count * channels) {
        const size_t wanted = count - out.size() / channels;
        const size_t got = format.dsd ? decoder.read_dsd(pointers.data(), wanted) : decoder.read_pcm(pcm.data(), wanted);
        if (got == 0) break;
        for (size_t i = 0; i < got; ++i) {
            for (size_t ch = 0; ch < channels; ++ch) out.push_back(format.dsd ? planes[ch][i] : pcm[i * channels + ch]);
        }
    }
    return out;
}

// A CUE track only joins the previous one without a gap if seeking lands on the exact sample. Reading
// straight through the boundary from well before it must give what the track gives after its seek.
bool range_start_is_exact(const Track& track) {
    constexpr uint64_t kLead = 200000;
    const std::unique_ptr<Decoder> whole = open_decoder(track.path);
    const uint64_t lead = std::min(track.range_start, kLead);
    whole->seek(track.range_start - lead);
    if (lead) read_block(*whole, static_cast<size_t>(lead));
    const std::vector<double> straight = read_block(*whole, kProbeFrames);

    const std::unique_ptr<Decoder> range = open_track(track);
    return !straight.empty() && read_block(*range, kProbeFrames) == straight;
}

// Opens every track and reads at the start, the middle and just before the end, which also exercises
// seeking inside CUE ranges and SACD images.
int verify(LibraryStore& store, bool verbose) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::vector<Track> tracks = store.tracks();
    const auto begin = std::chrono::steady_clock::now();
    int failures = 0;
    std::map<std::string, int> exact;
    std::map<std::string, int> inexact;

    for (size_t i = 0; i < tracks.size(); ++i) {
        const Track& track = tracks[i];
        const auto report = [&](const std::string& text) {
            printf("FAILED %s #%d: %s\n", track.relative_path.c_str(), track.part, text.c_str());
            ++failures;
        };
        if (verbose) {
            fprintf(stderr, "%s #%d\n", track.relative_path.c_str(), track.part);
            fflush(stderr);
        } else if (i % 250 == 0) {
            fprintf(stderr, "\r%zu / %zu", i, tracks.size());
        }

        try {
            const std::unique_ptr<Decoder> decoder = open_track(track);
            const SourceFormat format = decoder->format();
            if (format.length != track.format.length) {
                report("catalogued length " + std::to_string(track.format.length) + ", decoder says " +
                       std::to_string(format.length));
            }

            std::vector<double> pcm(kProbeFrames * format.channels);
            std::vector<std::vector<uint8_t>> planes(format.channels, std::vector<uint8_t>(kProbeFrames));
            std::vector<uint8_t*> pointers;
            for (auto& plane : planes) pointers.push_back(plane.data());

            const uint64_t tail = format.length > kProbeFrames ? format.length - kProbeFrames : 0;
            for (const uint64_t position : {uint64_t{0}, format.length / 2, tail}) {
                decoder->seek(position);
                const size_t got = format.dsd ? decoder->read_dsd(pointers.data(), kProbeFrames)
                                              : decoder->read_pcm(pcm.data(), kProbeFrames);
                if (got == 0) report("nothing to read at " + std::to_string(position));
            }

            if (track.range_start != 0) {
                if (range_start_is_exact(track)) {
                    ++exact[track.codec];
                } else {
                    ++inexact[track.codec];
                    report("the track does not start on its exact sample");
                }
            }
        } catch (const std::exception& error) {
            report(error.what());
        }
    }

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    printf("\n%zu tracks opened in %.1f s, %d failures\n", tracks.size(), seconds, failures);
    for (const auto& [codec, count] : exact) printf("%6d  CUE starts exact in %s\n", count, codec.c_str());
    for (const auto& [codec, count] : inexact) printf("%6d  CUE starts NOT exact in %s\n", count, codec.c_str());
    return failures == 0 ? 0 : 1;
}

int usage() {
    fputs(
        "usage: arpegio-library DB scan FOLDER [--force] [--no-artwork] [--multichannel] [--verbose]\n"
        "       arpegio-library DB stats\n"
        "       arpegio-library DB albums\n"
        "       arpegio-library DB artists\n"
        "       arpegio-library DB album ID\n"
        "       arpegio-library DB search TEXT\n"
        "       arpegio-library DB verify [--verbose]\n",
        stderr);
    return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3) return usage();
    try {
        const std::filesystem::path database = argv[1];
        const std::filesystem::path folder =
            database.has_parent_path() ? database.parent_path() : std::filesystem::path(".");
        LibraryStore store(database);
        const std::wstring command = argv[2];

        if (command == L"scan" && argc >= 4) return scan(store, folder / "cache", argv[3], argc - 4, argv + 4);
        if (command == L"stats") return stats(store);
        if (command == L"verify") return verify(store, argc >= 4 && std::wstring(argv[3]) == L"--verbose");
        if (command == L"albums") {
            for (const Album& album : store.albums()) {
                printf("%5lld  %4d  %3d tracks  %-5s  %s - %s\n", static_cast<long long>(album.id), album.year,
                       album.track_count, section_name(album.section), album.album_artist.c_str(), album.title.c_str());
            }
            return 0;
        }
        if (command == L"artists") {
            for (const Artist& artist : store.artists()) {
                printf("%5lld  %3d albums  %4d tracks  %s\n", static_cast<long long>(artist.id), artist.album_count,
                       artist.track_count, artist.name.c_str());
            }
            return 0;
        }
        if (command == L"album" && argc >= 4) {
            for (const Track& track : store.album_tracks(_wtoi64(argv[3]))) print_track(track);
            return 0;
        }
        if (command == L"search" && argc >= 4) {
            for (const Track& track : store.search(utf8_argument(argv[3]))) print_track(track);
            return 0;
        }
    } catch (const std::exception& error) {
        fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
    return usage();
}
