// Turns a catalogued track into something the engine can play.
#pragma once

#include <memory>

#include "decode/decoder.h"
#include "engine/player.h"
#include "library/library_store.h"

namespace arpegio {

enum class ReplayGainMode { Off, Track, Album };

// Whole files, CUE tracks and SACD tracks all come out as one Decoder. Throws std::runtime_error.
std::unique_ptr<Decoder> open_track(const Track& track);

// How much to scale a track. 1 when the tags say nothing, so an untagged track stays untouched. Album mode
// falls back on the track's own tags. A gain never pushes the track's peak past full scale, and with no
// peak to go by it only lowers the level.
double replay_gain_factor(const ReplayGainTags& tags, ReplayGainMode mode);

QueueItem make_queue_item(const Track& track, ReplayGainMode mode = ReplayGainMode::Off);

}  // namespace arpegio
