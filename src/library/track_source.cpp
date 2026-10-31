#include "library/track_source.h"

#include "decode/range_decoder.h"
#include "decode/sacd_iso.h"

namespace arpegio {

std::unique_ptr<Decoder> open_track(const Track& track) {
    if (track.sacd_area >= 0) return open_sacd_track(track.path, track.sacd_area, track.sacd_track);
    std::unique_ptr<Decoder> decoder = open_decoder(track.path);
    if (track.range_end == 0) return decoder;
    return make_range_decoder(std::move(decoder), track.range_start, track.range_end);
}

QueueItem make_queue_item(const Track& track) {
    return {track.path, [track] { return open_track(track); }};
}

}  // namespace arpegio
