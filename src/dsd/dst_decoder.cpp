// Port of FFmpeg's libavcodec/dstdec.c, Copyright (c) 2014 Peter Ross, LGPL 2.1 or later.
// It keeps the arithmetic of that decoder and drops the FFmpeg plumbing, so raw DSD comes out with
// no dependency on a particular FFmpeg release.
#include "dsd/dst_decoder.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace arpegio {

namespace {

constexpr int kMaxChannels = 6;
constexpr int kMaxElements = 2 * kMaxChannels;

constexpr int8_t kFilterCodePrediction[3][3] = {{-8}, {-16, 8}, {-9, -5, 6}};
constexpr int8_t kProbabilityCodePrediction[3][3] = {{-8}, {-16, 8}, {-24, 24, -8}};

struct Table {
    unsigned elements = 0;
    unsigned length[kMaxElements] = {};
    int coefficients[kMaxElements][128] = {};
};

// Reads MSB first. Past the end it returns zeros, and truncated frames fail their checks.
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), bits_(size * 8) {}

    size_t left() const { return bits_ > position_ ? bits_ - position_ : 0; }

    unsigned read1() {
        if (position_ >= bits_) {
            ++position_;
            return 0;
        }
        const unsigned bit = (data_[position_ >> 3] >> (7 - (position_ & 7))) & 1;
        ++position_;
        return bit;
    }

    unsigned read(unsigned count) {
        unsigned value = 0;
        while (count > 0) {
            const unsigned offset = static_cast<unsigned>(position_ & 7);
            const unsigned take = std::min(count, 8 - offset);
            const unsigned byte = position_ < bits_ ? data_[position_ >> 3] : 0;
            value = (value << take) | ((byte >> (8 - offset - take)) & ((1u << take) - 1));
            position_ += take;
            count -= take;
        }
        return value;
    }

    int read_signed(unsigned count) {
        const unsigned value = read(count);
        return static_cast<int>(value << (32 - count)) >> (32 - count);
    }

    // Rice code with a separate sign bit.
    int read_golomb(unsigned k) {
        unsigned zeros = 0;
        while (left() > 0 && read1() == 0) ++zeros;
        const int magnitude = static_cast<int>((zeros << k) | read(k));
        return (magnitude != 0 && read1()) ? -magnitude : magnitude;
    }

private:
    const uint8_t* data_;
    size_t bits_;
    size_t position_ = 0;
};

struct ArithmeticDecoder {
    unsigned a = 0;
    unsigned c = 0;

    void init(BitReader& bits) {
        a = 4095;
        c = bits.read(12);
    }

    unsigned decode(BitReader& bits, unsigned probability) {
        const unsigned k = (a >> 8) | ((a >> 7) & 1);
        const unsigned q = k * probability;
        const unsigned a_q = a - q;
        const unsigned symbol = c < a_q;
        if (symbol) {
            a = a_q;
        } else {
            a = q;
            c -= a_q;
        }
        if (a < 2048) {
            const int n = 11 - (std::bit_width(a) - 1);
            a <<= n;
            c = (c << n) | bits.read(n);
        }
        return symbol;
    }
};

bool read_map(BitReader& bits, Table& table, unsigned map[kMaxChannels], int channels) {
    table.elements = 1;
    map[0] = 0;
    if (bits.read1()) {
        std::fill_n(map, kMaxChannels, 0u);
        return true;
    }
    for (int ch = 1; ch < channels; ++ch) {
        const unsigned width = std::bit_width(table.elements);
        map[ch] = bits.read(width);
        if (map[ch] == table.elements) {
            if (++table.elements >= kMaxElements) return false;
        } else if (map[ch] > table.elements) {
            return false;
        }
    }
    return true;
}

void read_uncoded(BitReader& bits, int* out, unsigned count, unsigned width, bool is_signed, int offset) {
    for (unsigned i = 0; i < count; ++i) {
        out[i] = (is_signed ? bits.read_signed(width) : static_cast<int>(bits.read(width))) + offset;
    }
}

bool read_table(BitReader& bits, Table& table, const int8_t prediction[3][3], unsigned length_bits, unsigned width,
                bool is_signed, int offset) {
    for (unsigned i = 0; i < table.elements; ++i) {
        table.length[i] = bits.read(length_bits) + 1;
        if (!bits.read1()) {
            read_uncoded(bits, table.coefficients[i], table.length[i], width, is_signed, offset);
            continue;
        }
        const unsigned method = bits.read(2);
        if (method == 3) return false;
        read_uncoded(bits, table.coefficients[i], method + 1, width, is_signed, offset);
        const unsigned lsb_size = bits.read(3);
        for (unsigned j = method + 1; j < table.length[i]; ++j) {
            int predicted = 0;
            for (unsigned k = 0; k < method + 1; ++k) {
                predicted += prediction[method][k] * table.coefficients[i][j - k - 1];
            }
            int value = bits.read_golomb(lsb_size);
            value += predicted >= 0 ? -((predicted + 4) / 8) : (-predicted + 3) / 8;
            if (!is_signed && (value < offset || value >= offset + (1 << width))) return false;
            table.coefficients[i][j] = value;
        }
    }
    return true;
}

unsigned reverse_7_bits(unsigned value) {
    unsigned reversed = 0;
    for (int bit = 0; bit < 7; ++bit) reversed |= ((value >> bit) & 1) << (6 - bit);
    return reversed;
}

}  // namespace

struct DstDecoder::State {
    Table filters;
    Table probabilities;
    ArithmeticDecoder arithmetic;
    int16_t filter[kMaxElements][16][256];
    uint8_t status[kMaxChannels][16];
    uint32_t samples_per_frame = 0;

    bool build_filter() {
        for (unsigned element = 0; element < filters.elements; ++element) {
            const int length = static_cast<int>(filters.length[element]);
            for (int group = 0; group < 16; ++group) {
                const int total = std::clamp(length - group * 8, 0, 8);
                for (int byte = 0; byte < 256; ++byte) {
                    int64_t value = 0;
                    for (int bit = 0; bit < total; ++bit) {
                        value += (((byte >> bit) & 1) * 2 - 1) * filters.coefficients[element][group * 8 + bit];
                    }
                    if (static_cast<int16_t>(value) != value) return false;
                    filter[element][group][byte] = static_cast<int16_t>(value);
                }
            }
        }
        return true;
    }
};

DstDecoder::DstDecoder(int channels, uint32_t dsd_rate)
    : state_(std::make_unique<State>()), channels_(channels), frame_bytes_(588ull * (dsd_rate / 44100) / 8) {
    if (channels < 1 || channels > kMaxChannels || dsd_rate % 44100 != 0) {
        throw std::runtime_error("DST: unsupported stream layout");
    }
    state_->samples_per_frame = static_cast<uint32_t>(frame_bytes_ * 8);
}

DstDecoder::~DstDecoder() = default;

bool DstDecoder::decode(const uint8_t* data, size_t size, uint8_t* out) {
    if (size <= 1) return false;
    State& s = *state_;
    const size_t total = frame_bytes_ * channels_;

    BitReader bits(data, size);
    if (!bits.read1()) {
        // Frame stored without compression. Short frames are padded with DSD silence.
        bits.read1();
        if (bits.read(6) != 0) return false;
        const size_t count = std::min(size - 1, total);
        std::memcpy(out, data + 1, count);
        std::memset(out + count, 0x69, total - count);
        return true;
    }

    // Only the segmentation layouts real discs use: same for all channels, ending at the frame.
    if (!bits.read1() || !bits.read1() || !bits.read1()) return false;

    const unsigned same_map = bits.read1();
    unsigned filter_map[kMaxChannels];
    unsigned probability_map[kMaxChannels];
    if (!read_map(bits, s.filters, filter_map, channels_)) return false;
    if (same_map) {
        s.probabilities.elements = s.filters.elements;
        std::memcpy(probability_map, filter_map, sizeof(filter_map));
    } else if (!read_map(bits, s.probabilities, probability_map, channels_)) {
        return false;
    }

    unsigned half_probability[kMaxChannels];
    for (int ch = 0; ch < channels_; ++ch) half_probability[ch] = bits.read1();

    if (!read_table(bits, s.filters, kFilterCodePrediction, 7, 9, true, 0)) return false;
    if (!read_table(bits, s.probabilities, kProbabilityCodePrediction, 6, 7, false, 1)) return false;

    if (bits.read1()) return false;
    s.arithmetic.init(bits);
    if (!s.build_filter()) return false;

    std::memset(s.status, 0xAA, sizeof(s.status));
    std::memset(out, 0, total);

    // The first arithmetic symbol is the DST "x" bit, decoded and discarded.
    s.arithmetic.decode(bits, reverse_7_bits(s.filters.coefficients[0][0] & 127) + 1);

    for (uint32_t i = 0; i < s.samples_per_frame; ++i) {
        for (int ch = 0; ch < channels_; ++ch) {
            const unsigned element = filter_map[ch];
            int16_t (*filter)[256] = s.filter[element];
            uint8_t* status = s.status[ch];

            int16_t predict = 0;
            for (int group = 0; group < 16; ++group) predict = static_cast<int16_t>(predict + filter[group][status[group]]);

            unsigned probability = 128;
            if (!half_probability[ch] || i >= s.filters.length[element]) {
                const unsigned table = probability_map[ch];
                const unsigned index = static_cast<unsigned>(std::abs(static_cast<int>(predict))) >> 3;
                probability = static_cast<unsigned>(
                    s.probabilities.coefficients[table][std::min(index, s.probabilities.length[table] - 1)]);
            }

            const unsigned residual = s.arithmetic.decode(bits, probability);
            const unsigned value = ((static_cast<unsigned>(predict) >> 15) ^ residual) & 1;
            out[(i >> 3) * channels_ + ch] |= static_cast<uint8_t>(value << (7 - (i & 7)));

            uint64_t low;
            uint64_t high;
            std::memcpy(&low, status, 8);
            std::memcpy(&high, status + 8, 8);
            high = (high << 1) | (low >> 63);
            low = (low << 1) | value;
            std::memcpy(status, &low, 8);
            std::memcpy(status + 8, &high, 8);
        }
    }
    return true;
}

}  // namespace arpegio
