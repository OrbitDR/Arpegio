#include "audio/quantizer.h"

#include <cmath>
#include <cstring>

namespace arpegio {

Quantizer::Quantizer(WireEncoding encoding, int dither_bits) : encoding_(encoding) {
    set_dither_bits(dither_bits);
}

void Quantizer::set_dither_bits(int dither_bits) {
    dither_scale_ = dither_bits > 0 ? std::ldexp(1.0, 1 - dither_bits) : 0.0;
}

double Quantizer::next_dither() {
    // Two uniform values give a triangular density of +/- 1 LSB.
    const auto uniform = [this] {
        rng_ ^= rng_ >> 12;
        rng_ ^= rng_ << 25;
        rng_ ^= rng_ >> 27;
        return static_cast<double>((rng_ * 0x2545F4914F6CDD1Dull) >> 11) * (1.0 / 9007199254740992.0);
    };
    return (uniform() - uniform()) * dither_scale_;
}

void Quantizer::convert(const double* samples, size_t count, uint8_t* out) {
    const bool dither = dither_scale_ != 0.0;
    const auto to_int = [&](double value, double scale, int64_t low, int64_t high) {
        if (dither) value += next_dither();
        const int64_t rounded = std::llrint(value * scale);
        if (rounded > high) {
            ++clipped_;
            return high;
        }
        if (rounded < low) {
            ++clipped_;
            return low;
        }
        return rounded;
    };

    switch (encoding_) {
    case WireEncoding::S16:
        for (size_t i = 0; i < count; ++i, out += 2) {
            const auto v = static_cast<int16_t>(to_int(samples[i], 32768.0, -32768, 32767));
            std::memcpy(out, &v, 2);
        }
        break;
    case WireEncoding::S24:
        for (size_t i = 0; i < count; ++i, out += 3) {
            const auto v = static_cast<int32_t>(to_int(samples[i], 8388608.0, -8388608, 8388607));
            out[0] = static_cast<uint8_t>(v);
            out[1] = static_cast<uint8_t>(v >> 8);
            out[2] = static_cast<uint8_t>(v >> 16);
        }
        break;
    case WireEncoding::S32:
        for (size_t i = 0; i < count; ++i, out += 4) {
            const auto v = static_cast<int32_t>(to_int(samples[i], 2147483648.0, -2147483648LL, 2147483647LL));
            std::memcpy(out, &v, 4);
        }
        break;
    case WireEncoding::S32Right24:
    case WireEncoding::S32Right20:
    case WireEncoding::S32Right18:
    case WireEncoding::S32Right16: {
        const int bits = wire_resolution(encoding_);
        const int64_t high = (1LL << (bits - 1)) - 1;
        const double scale = std::ldexp(1.0, bits - 1);
        for (size_t i = 0; i < count; ++i, out += 4) {
            const auto v = static_cast<int32_t>(to_int(samples[i], scale, -high - 1, high));
            std::memcpy(out, &v, 4);
        }
        break;
    }
    case WireEncoding::F32:
        for (size_t i = 0; i < count; ++i, out += 4) {
            const float v = static_cast<float>(dither ? samples[i] + next_dither() : samples[i]);
            std::memcpy(out, &v, 4);
        }
        break;
    case WireEncoding::F64:
        std::memcpy(out, samples, count * 8);
        break;
    default:
        break;
    }
}

void read_wire_samples(const uint8_t* bytes, WireEncoding encoding, double* samples, size_t count) {
    switch (encoding) {
    case WireEncoding::S16:
        for (size_t i = 0; i < count; ++i, bytes += 2) {
            int16_t value = 0;
            std::memcpy(&value, bytes, 2);
            samples[i] = value / 32768.0;
        }
        break;
    case WireEncoding::S24:
        for (size_t i = 0; i < count; ++i, bytes += 3) {
            const uint32_t raw = static_cast<uint32_t>(bytes[0]) << 8 | static_cast<uint32_t>(bytes[1]) << 16 |
                                 static_cast<uint32_t>(bytes[2]) << 24;
            samples[i] = (static_cast<int32_t>(raw) >> 8) / 8388608.0;
        }
        break;
    case WireEncoding::S32:
        for (size_t i = 0; i < count; ++i, bytes += 4) {
            int32_t value = 0;
            std::memcpy(&value, bytes, 4);
            samples[i] = value / 2147483648.0;
        }
        break;
    case WireEncoding::S32Right24:
    case WireEncoding::S32Right20:
    case WireEncoding::S32Right18:
    case WireEncoding::S32Right16: {
        const double scale = std::ldexp(1.0, wire_resolution(encoding) - 1);
        for (size_t i = 0; i < count; ++i, bytes += 4) {
            int32_t value = 0;
            std::memcpy(&value, bytes, 4);
            samples[i] = value / scale;
        }
        break;
    }
    case WireEncoding::F32:
        for (size_t i = 0; i < count; ++i, bytes += 4) {
            float value = 0.0f;
            std::memcpy(&value, bytes, 4);
            samples[i] = value;
        }
        break;
    case WireEncoding::F64:
        std::memcpy(samples, bytes, count * 8);
        break;
    default:
        break;
    }
}

}  // namespace arpegio
