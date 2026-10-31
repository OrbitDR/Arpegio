#include <memory>

#include "decode/decoder.h"
#include "formats/dsf_reader.h"

namespace arpegio {

namespace {

class DsfDecoder final : public Decoder {
public:
    explicit DsfDecoder(const std::filesystem::path& path) : reader_(path) {
        format_.dsd = true;
        format_.rate = reader_.sample_rate();
        format_.channels = reader_.channels();
        format_.bits = 1;
        format_.length = reader_.total_bytes_per_channel();
    }

    const SourceFormat& format() const override { return format_; }

    size_t read_dsd(uint8_t* const* planar, size_t bytes) override { return reader_.read(planar, bytes); }

    void seek(uint64_t position) override { reader_.seek_bytes(position); }

private:
    DsfReader reader_;
    SourceFormat format_;
};

}  // namespace

std::unique_ptr<Decoder> open_dsf_decoder(const std::filesystem::path& path) {
    return std::make_unique<DsfDecoder>(path);
}

}  // namespace arpegio
