// DSD silence with no file behind it, for checks against a real DAC.
#pragma once

#include <cstdint>
#include <memory>

#include "decode/decoder.h"

namespace arpegio {

std::unique_ptr<Decoder> make_dsd_silence(uint32_t rate, int channels, double seconds);

}  // namespace arpegio
