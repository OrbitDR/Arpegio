// Turns a catalogued track into something the engine can play.
#pragma once

#include <memory>

#include "decode/decoder.h"
#include "engine/player.h"
#include "library/library_store.h"

namespace arpegio {

// Whole files, CUE tracks and SACD tracks all come out as one Decoder. Throws std::runtime_error.
std::unique_ptr<Decoder> open_track(const Track& track);

QueueItem make_queue_item(const Track& track);

}  // namespace arpegio
