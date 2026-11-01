// Recognises MQA in the audio itself, because tags miss many of these files. Arpegio does not decode MQA:
// the public sync word is all it looks for.
#pragma once

namespace arpegio {

class Decoder;

// Reads the first seconds from wherever the decoder is. Only 16 and 24-bit stereo PCM can carry it, so
// anything else returns false without reading.
bool has_mqa_sync(Decoder& decoder);

}  // namespace arpegio
