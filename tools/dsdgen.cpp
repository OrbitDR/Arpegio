// Writes a stereo DSF test file: sine tones through a 7th-order 1-bit delta-sigma modulator.
// The noise transfer function comes from a coefficient file made by tools/measure/design_ntf.py.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

namespace {

constexpr int kOrder = 7;

struct Tone {
    double hz;
    double amplitude;
};

struct Modulator {
    std::array<double, kOrder + 1> a{};  // NTF denominator, a[0] == 1
    std::array<double, kOrder + 1> b{};  // NTF numerator, b[0] == 1
    std::array<double, kOrder + 1> e{};  // past quantizer errors
    std::array<double, kOrder + 1> w{};  // past loop filter outputs
    double peak = 0.0;

    int step(double input) {
        double loop = 0.0;
        for (int k = 1; k <= kOrder; ++k) loop += (b[k] - a[k]) * e[k] - a[k] * w[k];
        const double v = input + loop;
        const int bit = v >= 0.0 ? 1 : 0;
        const double y = bit ? 1.0 : -1.0;
        for (int k = kOrder; k > 1; --k) {
            e[k] = e[k - 1];
            w[k] = w[k - 1];
        }
        e[1] = y - v;
        w[1] = loop;
        peak = std::max(peak, std::abs(v));
        return bit;
    }
};

void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void put64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void put_id(std::vector<uint8_t>& out, const char* id) {
    out.insert(out.end(), id, id + 4);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 5) {
        fputs("usage: arpegio-dsdgen OUT.dsf NTF.txt RATE SECONDS [HZ:DBFS ...]\n", stderr);
        return 2;
    }
    const uint32_t rate = wcstoul(argv[3], nullptr, 10);
    const double seconds = wcstod(argv[4], nullptr);
    std::vector<Tone> tones;
    for (int i = 5; i < argc; ++i) {
        wchar_t* end = nullptr;
        const double hz = wcstod(argv[i], &end);
        const double db = wcstod(end + 1, nullptr);
        tones.push_back({hz, std::pow(10.0, db / 20.0)});
    }

    Modulator left;
    {
        std::ifstream in(argv[2]);
        for (auto& c : left.b) in >> c;
        for (auto& c : left.a) in >> c;
        if (!in) {
            fputs("cannot read NTF coefficients\n", stderr);
            return 1;
        }
    }
    Modulator right = left;

    constexpr uint32_t kBlock = 4096;
    const uint64_t samples = static_cast<uint64_t>(seconds * rate);
    const uint64_t blocks = (samples / 8 + kBlock - 1) / kBlock;
    const uint64_t data_size = blocks * kBlock * 2;

    std::vector<uint8_t> head;
    put_id(head, "DSD ");
    put64(head, 28);
    put64(head, 28 + 52 + 12 + data_size);
    put64(head, 0);
    put_id(head, "fmt ");
    put64(head, 52);
    put32(head, 1);
    put32(head, 0);
    put32(head, 2);  // stereo
    put32(head, 2);
    put32(head, rate);
    put32(head, 1);  // LSB first
    put64(head, samples);
    put32(head, kBlock);
    put32(head, 0);
    put_id(head, "data");
    put64(head, 12 + data_size);

    FILE* f = _wfopen(argv[1], L"wb");
    if (!f) {
        fputs("cannot create output\n", stderr);
        return 1;
    }
    fwrite(head.data(), 1, head.size(), f);

    const double dt = 1.0 / rate;
    std::vector<double> phase_step;
    for (const Tone& t : tones) phase_step.push_back(2.0 * std::numbers::pi * t.hz * dt);

    std::vector<uint8_t> l(kBlock), r(kBlock);
    uint64_t n = 0;
    for (uint64_t block = 0; block < blocks; ++block) {
        std::fill(l.begin(), l.end(), uint8_t{0});
        std::fill(r.begin(), r.end(), uint8_t{0});
        for (uint32_t byte = 0; byte < kBlock; ++byte) {
            for (int bit = 0; bit < 8; ++bit, ++n) {
                double input = 0.0;
                for (size_t t = 0; t < tones.size(); ++t) {
                    input += tones[t].amplitude * std::sin(std::fmod(phase_step[t] * static_cast<double>(n),
                                                                     2.0 * std::numbers::pi));
                }
                l[byte] |= static_cast<uint8_t>(left.step(input) << bit);
                r[byte] |= static_cast<uint8_t>(right.step(input) << bit);
            }
        }
        fwrite(l.data(), 1, kBlock, f);
        fwrite(r.data(), 1, kBlock, f);
    }
    fclose(f);
    printf("%llu samples, peak quantizer input %.2f\n", static_cast<unsigned long long>(samples), left.peak);
    return 0;
}
