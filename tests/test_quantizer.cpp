#include <cmath>
#include <cstring>
#include <random>
#include <vector>

#include "audio/quantizer.h"
#include "dsd/dop.h"
#include "test_support.h"

using namespace arpegio;

// The whole bit-perfect claim rests on these: integer to double to integer must be the identity.
TEST(every_24_bit_value_survives) {
    Quantizer quantizer(WireEncoding::S24, 0);
    constexpr int kStep = 1 << 24;
    std::vector<double> in(1 << 16);
    std::vector<uint8_t> out(in.size() * 3);
    for (int base = -(kStep / 2); base < kStep / 2; base += static_cast<int>(in.size())) {
        for (size_t i = 0; i < in.size(); ++i) in[i] = (base + static_cast<int>(i)) * (1 << 8) / 2147483648.0;
        quantizer.convert(in.data(), in.size(), out.data());
        for (size_t i = 0; i < in.size(); ++i) {
            const int32_t value = static_cast<int32_t>(static_cast<uint32_t>(out[i * 3]) << 8 |
                                                       static_cast<uint32_t>(out[i * 3 + 1]) << 16 |
                                                       static_cast<uint32_t>(out[i * 3 + 2]) << 24) >> 8;
            if (value != base + static_cast<int>(i)) {
                CHECK(false);
                return;
            }
        }
    }
    CHECK(quantizer.clipped() == 0);
}

TEST(random_32_bit_values_survive) {
    Quantizer quantizer(WireEncoding::S32, 0);
    std::mt19937 rng(7);
    std::vector<int32_t> values(1 << 20);
    std::vector<double> in(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<int32_t>(rng());
        in[i] = values[i] / 2147483648.0;
    }
    std::vector<int32_t> out(values.size());
    quantizer.convert(in.data(), in.size(), reinterpret_cast<uint8_t*>(out.data()));
    CHECK(std::memcmp(out.data(), values.data(), values.size() * 4) == 0);
}

TEST(sixteen_bit_lands_in_the_top_of_32) {
    Quantizer quantizer(WireEncoding::S32, 0);
    const double in = -12345 / 32768.0;
    int32_t out = 0;
    quantizer.convert(&in, 1, reinterpret_cast<uint8_t*>(&out));
    CHECK(out == -12345 * 65536);
}

TEST(right_aligned_keeps_sign) {
    Quantizer quantizer(WireEncoding::S32Right24, 0);
    const double in = -1.0 / 8388608.0;
    int32_t out = 0;
    quantizer.convert(&in, 1, reinterpret_cast<uint8_t*>(&out));
    CHECK(out == -1);
}

TEST(clipping_is_counted) {
    Quantizer quantizer(WireEncoding::S16, 0);
    const double in[2] = {1.5, -1.5};
    int16_t out[2] = {};
    quantizer.convert(in, 2, reinterpret_cast<uint8_t*>(out));
    CHECK(out[0] == 32767);
    CHECK(out[1] == -32768);
    CHECK(quantizer.clipped() == 2);
}

TEST(tpdf_dither_stays_within_two_lsb) {
    Quantizer quantizer(WireEncoding::S32, 24);
    std::vector<double> in(100000, 0.0);
    std::vector<int32_t> out(in.size());
    quantizer.convert(in.data(), in.size(), reinterpret_cast<uint8_t*>(out.data()));
    double sum = 0.0;
    double peak = 0.0;
    for (int32_t value : out) {
        const double lsb = value / 256.0;
        sum += lsb;
        peak = std::max(peak, std::abs(lsb));
    }
    CHECK(peak <= 2.0);
    CHECK(std::abs(sum / out.size()) < 0.02);
}

TEST(dop_frames_and_markers) {
    DopPacker packer(WireEncoding::S32, 2);
    const uint8_t left[] = {0x11, 0x22, 0x33, 0x44};
    const uint8_t right[] = {0xAA, 0xBB, 0xCC, 0xDD};
    const uint8_t* planes[] = {left, right};
    uint8_t out[4 * 2 * 2];
    CHECK(packer.pack(planes, 4, out) == 2);
    uint32_t phase = 0;
    dop_stamp_markers(out, 2, 2, 4, phase);
    const uint8_t expected[] = {0x00, 0x22, 0x11, 0x05, 0x00, 0xBB, 0xAA, 0x05,
                                0x00, 0x44, 0x33, 0xFA, 0x00, 0xDD, 0xCC, 0xFA};
    CHECK(std::memcmp(out, expected, sizeof(out)) == 0);
    CHECK(phase == 2);
}

TEST(dop_carries_an_odd_byte) {
    DopPacker packer(WireEncoding::S24, 1);
    const uint8_t first[] = {0x11, 0x22, 0x33};
    const uint8_t second[] = {0x44, 0x55};
    const uint8_t* plane = first;
    uint8_t out[3 * 2];
    CHECK(packer.pack(&plane, 3, out) == 1);
    CHECK(out[0] == 0x22 && out[1] == 0x11);
    plane = second;
    CHECK(packer.pack(&plane, 2, out) == 1);
    CHECK(out[0] == 0x44 && out[1] == 0x33);
}
