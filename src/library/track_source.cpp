#include "library/track_source.h"

#include <algorithm>
#include <cmath>

#include "decode/range_decoder.h"
#include "decode/sacd_iso.h"

namespace arpegio {

std::unique_ptr<Decoder> open_track(const Track& track) {
    if (track.sacd_area >= 0) return open_sacd_track(track.path, track.sacd_area, track.sacd_track);
    std::unique_ptr<Decoder> decoder = open_decoder(track.path);
    if (track.range_end == 0) return decoder;
    return make_range_decoder(std::move(decoder), track.range_start, track.range_end);
}

double replay_gain_factor(const ReplayGainTags& tags, ReplayGainMode mode) {
    if (mode == ReplayGainMode::Off) return 1.0;
    const bool album = mode == ReplayGainMode::Album && tags.album_gain_db.has_value();
    const std::optional<double> gain_db = album ? tags.album_gain_db : tags.track_gain_db;
    const std::optional<double> peak = album ? tags.album_peak : tags.track_peak;
    if (!gain_db) return 1.0;
    return std::min(std::pow(10.0, *gain_db / 20.0), peak ? 1.0 / *peak : 1.0);
}

QueueItem make_queue_item(const Track& track, ReplayGainMode mode) {
    return {track.path, [track] { return open_track(track); }, track.id, replay_gain_factor(track.replay_gain, mode)};
}

}  // namespace arpegio
