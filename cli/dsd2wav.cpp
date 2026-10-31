// Converts a DSF file to a 64-bit float WAV with the engine's DSD to PCM path.
// Used to measure filters: --taps replaces the designed FIR with coefficients from a text file.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "dsd/dsd_fir.h"
#include "dsd/dsd_pcm_plan.h"
#include "formats/dsf_reader.h"
#include "formats/wav_writer.h"

using namespace arpegio;

namespace {

void usage() {
    fputs("usage: arpegio-dsd2wav IN.dsf OUT.wav [--rate HZ] [--gain-db DB] [--seconds S]\n"
          "                       [--skip S] [--taps FILE --divider N] [--print-filter]\n",
          stderr);
}

std::vector<double> read_taps(const wchar_t* path) {
    std::ifstream in(path);
    std::vector<double> taps;
    double value;
    while (in >> value) taps.push_back(value);
    return taps;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        usage();
        return 2;
    }
    uint32_t rate = 0;
    uint32_t divider = 0;
    double gain_db = 0.0;
    double seconds = 0.0;
    double skip = 0.0;
    const wchar_t* taps_path = nullptr;
    bool print_filter = false;
    for (int i = 3; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--rate" && i + 1 < argc) rate = wcstoul(argv[++i], nullptr, 10);
        else if (arg == L"--gain-db" && i + 1 < argc) gain_db = wcstod(argv[++i], nullptr);
        else if (arg == L"--seconds" && i + 1 < argc) seconds = wcstod(argv[++i], nullptr);
        else if (arg == L"--skip" && i + 1 < argc) skip = wcstod(argv[++i], nullptr);
        else if (arg == L"--taps" && i + 1 < argc) taps_path = argv[++i];
        else if (arg == L"--divider" && i + 1 < argc) divider = wcstoul(argv[++i], nullptr, 10);
        else if (arg == L"--print-filter") print_filter = true;
        else {
            usage();
            return 2;
        }
    }

    try {
        DsfReader reader(argv[1]);
        const uint32_t dsd_rate = reader.sample_rate();
        const int channels = reader.channels();

        std::vector<double> taps;
        if (taps_path) {
            taps = read_taps(taps_path);
            if (taps.empty() || divider == 0) throw std::runtime_error("--taps needs --divider");
            rate = dsd_rate / divider;
        } else {
            const auto source = classify_dsd_rate(dsd_rate);
            if (!source) throw std::runtime_error("not a DSD rate");
            const std::vector<uint32_t> rates = rate ? std::vector<uint32_t>{rate} : std::vector<uint32_t>{};
            const auto plan = plan_dsd_to_pcm(dsd_rate, rates);
            if (!plan) throw std::runtime_error("no plan for this rate");
            if (plan->needs_resampler) throw std::runtime_error("this rate needs the resampler");
            rate = plan->fir_rate;
            divider = plan->divider;
            taps = design_dsd_fir(dsd_rate, plan->filter);
            if (print_filter) {
                printf("dsd=%u pcm=%u divider=%u taps=%zu f3=%.0f pass=%.0f stop=%.0f atten=%.0f\n", dsd_rate, rate,
                       divider, taps.size(), plan->filter.f3_hz, plan->filter.pass_hz, plan->filter.stop_hz,
                       plan->filter.atten_db);
            }
        }

        DsdToPcm converter(std::move(taps), divider, channels);
        WavWriter writer(argv[2], rate, channels);

        if (skip > 0.0) reader.seek_bytes(static_cast<uint64_t>(skip * dsd_rate / 8.0));
        uint64_t remaining = seconds > 0.0 ? static_cast<uint64_t>(seconds * dsd_rate / 8.0)
                                           : reader.total_bytes_per_channel();

        constexpr size_t kChunk = 1 << 16;
        std::vector<std::vector<uint8_t>> buffers(channels, std::vector<uint8_t>(kChunk));
        std::vector<uint8_t*> planes(channels);
        for (int ch = 0; ch < channels; ++ch) planes[ch] = buffers[ch].data();
        std::vector<double> out(converter.frames_for(kChunk) * channels + channels * 8);

        const double gain = std::pow(10.0, gain_db / 20.0);
        double peak = 0.0;
        const auto start = std::chrono::steady_clock::now();
        uint64_t total_bytes = 0;
        while (remaining > 0) {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(kChunk, remaining));
            const size_t got = reader.read(planes.data(), want);
            if (got == 0) break;
            total_bytes += got;
            remaining -= got;
            const size_t frames = converter.process(planes.data(), got, out.data());
            for (size_t i = 0; i < frames * channels; ++i) {
                out[i] *= gain;
                peak = std::max(peak, std::abs(out[i]));
            }
            writer.write(out.data(), frames);
        }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const double audio_seconds = static_cast<double>(total_bytes) * 8.0 / dsd_rate;
        printf("%.1f s of audio in %.2f s (%.0fx real time), peak %.2f dBFS\n", audio_seconds, elapsed,
               audio_seconds / elapsed, 20.0 * std::log10(std::max(peak, 1e-12)));
    } catch (const std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
