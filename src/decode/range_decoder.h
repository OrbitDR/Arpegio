// A stretch of another decoder, presented as a source of its own. A CUE track is one of these.
#pragma once

#include <cstdint>
#include <memory>

#include "decode/decoder.h"

namespace arpegio {

// Plays [start, end) of `inner`, in frames or in bytes per channel for DSD. Throws std::runtime_error
// when the stretch does not fit in the source.
std::unique_ptr<Decoder> make_range_decoder(std::unique_ptr<Decoder> inner, uint64_t start, uint64_t end);

}  // namespace arpegio
