#include "library/mqa.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "decode/decoder.h"

namespace arpegio {

namespace {

// The stream runs through the XOR of both channels, at the 16th bit from the top or one of the two above.
constexpr uint64_t kSyncWord = 0xBE0498C88;
constexpr uint64_t kSyncMask = 0xFFFFFFFFF;  // 36 bits
constexpr int kBitsWatched = 3;

// Real files show their first sync within half a second and then more than one per second. One match
// can happen by chance in ordinary audio, two cannot.
constexpr double kSecondsRead = 4.0;
constexpr int kSyncsRequired = 2;
constexpr size_t kBlockFrames = 4096;

}  // namespace

bool has_mqa_sync(Decoder& decoder) {
    const SourceFormat& format = decoder.format();
    if (format.dsd || format.channels != 2 || (format.bits != 16 && format.bits != 24)) return false;

    const double scale = std::ldexp(1.0, format.bits - 1);
    const int shift = format.bits - 16;
    std::array<uint64_t, kBitsWatched> windows{};
    std::array<int, kBitsWatched> syncs{};
    std::vector<double> block(kBlockFrames * 2);

    uint64_t remaining = static_cast<uint64_t>(kSecondsRead * format.rate);
    while (remaining > 0) {
        const size_t wanted = static_cast<size_t>(std::min<uint64_t>(remaining, kBlockFrames));
        const size_t got = decoder.read_pcm(block.data(), wanted);
        if (got == 0) break;
        remaining -= got;

        for (size_t frame = 0; frame < got; ++frame) {
            const long left = std::lround(block[frame * 2] * scale);
            const long right = std::lround(block[frame * 2 + 1] * scale);
            const uint32_t difference = static_cast<uint32_t>(left ^ right) >> shift;
            for (int bit = 0; bit < kBitsWatched; ++bit) {
                windows[bit] = ((windows[bit] << 1) | ((difference >> bit) & 1u)) & kSyncMask;
                if (windows[bit] == kSyncWord && ++syncs[bit] == kSyncsRequired) return true;
            }
        }
    }
    return false;
}

}  // namespace arpegio
