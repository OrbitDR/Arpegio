// DSDIFF: plain DSD in interleaved bytes, or DST frames.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "decode/decoder.h"
#include "decode/framed_dsd.h"

namespace arpegio {

namespace {

constexpr size_t kChunkHeader = 12;

uint64_t be64(const uint8_t* p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | p[i];
    return value;
}

bool is(const uint8_t* id, const char* text) {
    return std::memcmp(id, text, 4) == 0;
}

struct FileCloser {
    void operator()(std::FILE* f) const { std::fclose(f); }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

struct DffLayout {
    uint32_t rate = 0;
    int channels = 0;
    bool compressed = false;
    uint64_t data_offset = 0;
    uint64_t data_size = 0;
    uint64_t frames = 0;  // DST only
};

uint32_t read_be32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

DffLayout parse(std::FILE* file) {
    uint8_t head[16];
    if (std::fread(head, 1, 16, file) != 16 || !is(head, "FRM8") || !is(head + 12, "DSD ")) {
        throw std::runtime_error("DFF: not a DSDIFF file");
    }
    const uint64_t end = 12 + be64(head + 4);

    DffLayout layout;
    while (static_cast<uint64_t>(_ftelli64(file)) + kChunkHeader <= end) {
        uint8_t chunk[kChunkHeader];
        if (std::fread(chunk, 1, kChunkHeader, file) != kChunkHeader) break;
        const uint64_t size = be64(chunk + 4);
        const uint64_t body = _ftelli64(file);
        const uint64_t next = body + size + (size & 1);

        if (is(chunk, "PROP")) {
            uint8_t type[4];
            if (std::fread(type, 1, 4, file) != 4 || !is(type, "SND ")) throw std::runtime_error("DFF: bad PROP chunk");
            while (static_cast<uint64_t>(_ftelli64(file)) + kChunkHeader <= body + size) {
                uint8_t sub[kChunkHeader];
                if (std::fread(sub, 1, kChunkHeader, file) != kChunkHeader) break;
                const uint64_t sub_size = be64(sub + 4);
                const uint64_t sub_next = _ftelli64(file) + sub_size + (sub_size & 1);
                uint8_t value[6] = {};
                std::fread(value, 1, std::min<uint64_t>(sub_size, sizeof(value)), file);
                if (is(sub, "FS  ")) layout.rate = read_be32(value);
                else if (is(sub, "CHNL")) layout.channels = value[0] << 8 | value[1];
                else if (is(sub, "CMPR")) layout.compressed = is(value, "DST ");
                _fseeki64(file, static_cast<int64_t>(sub_next), SEEK_SET);
            }
        } else if (is(chunk, "DSD ")) {
            layout.data_offset = body;
            layout.data_size = size;
        } else if (is(chunk, "DST ")) {
            layout.data_offset = body;
            layout.data_size = size;
            uint8_t info[kChunkHeader + 6];
            if (std::fread(info, 1, sizeof(info), file) == sizeof(info) && is(info, "FRTE")) {
                layout.frames = read_be32(info + kChunkHeader);
            }
        }
        _fseeki64(file, static_cast<int64_t>(next), SEEK_SET);
    }
    if (layout.rate == 0 || layout.channels < 1 || layout.channels > 6 || layout.data_offset == 0) {
        throw std::runtime_error("DFF: missing stream information");
    }
    return layout;
}

class DffPlainDecoder final : public Decoder {
public:
    DffPlainDecoder(File file, const DffLayout& layout) : file_(std::move(file)), layout_(layout) {
        format_ = {true, layout.rate, layout.channels, 1, layout.data_size / layout.channels};
        _fseeki64(file_.get(), static_cast<int64_t>(layout.data_offset), SEEK_SET);
    }

    const SourceFormat& format() const override { return format_; }

    size_t read_dsd(uint8_t* const* planar, size_t bytes) override {
        const int channels = format_.channels;
        bytes = static_cast<size_t>(std::min<uint64_t>(bytes, format_.length - position_));
        raw_.resize(bytes * channels);
        const size_t got = std::fread(raw_.data(), channels, bytes, file_.get());
        for (int ch = 0; ch < channels; ++ch) {
            const uint8_t* in = raw_.data() + ch;
            for (size_t i = 0; i < got; ++i, in += channels) planar[ch][i] = *in;
        }
        position_ += got;
        return got;
    }

    void seek(uint64_t position) override {
        position_ = std::min(position, format_.length);
        _fseeki64(file_.get(), static_cast<int64_t>(layout_.data_offset + position_ * format_.channels), SEEK_SET);
    }

private:
    File file_;
    DffLayout layout_;
    SourceFormat format_;
    uint64_t position_ = 0;
    std::vector<uint8_t> raw_;
};

class DffDstDecoder final : public FramedDsdDecoder {
public:
    DffDstDecoder(File file, const DffLayout& layout) : file_(std::move(file)), layout_(layout) {
        set_stream(layout.rate, layout.channels, layout.frames);
        _fseeki64(file_.get(), static_cast<int64_t>(layout.data_offset), SEEK_SET);
    }

protected:
    bool next_frame(std::vector<uint8_t>& data, bool& compressed) override {
        const uint64_t end = layout_.data_offset + layout_.data_size;
        for (;;) {
            uint8_t chunk[kChunkHeader];
            if (static_cast<uint64_t>(_ftelli64(file_.get())) + kChunkHeader > end) return false;
            if (std::fread(chunk, 1, kChunkHeader, file_.get()) != kChunkHeader) return false;
            const uint64_t size = be64(chunk + 4);
            if (is(chunk, "DSTF")) {
                data.resize(static_cast<size_t>(size));
                if (std::fread(data.data(), 1, data.size(), file_.get()) != data.size()) return false;
                if (size & 1) _fseeki64(file_.get(), 1, SEEK_CUR);
                compressed = true;
                return true;
            }
            _fseeki64(file_.get(), static_cast<int64_t>(size + (size & 1)), SEEK_CUR);
        }
    }

    void seek_frame(uint64_t frame) override {
        // Frames have no index we can rely on, so walk the chunk headers once.
        if (offsets_.empty()) index_frames();
        if (offsets_.empty()) return;
        const size_t index = static_cast<size_t>(std::min<uint64_t>(frame, offsets_.size() - 1));
        _fseeki64(file_.get(), static_cast<int64_t>(offsets_[index]), SEEK_SET);
    }

private:
    void index_frames() {
        const uint64_t end = layout_.data_offset + layout_.data_size;
        uint64_t position = layout_.data_offset;
        while (position + kChunkHeader <= end) {
            _fseeki64(file_.get(), static_cast<int64_t>(position), SEEK_SET);
            uint8_t chunk[kChunkHeader];
            if (std::fread(chunk, 1, kChunkHeader, file_.get()) != kChunkHeader) break;
            const uint64_t size = be64(chunk + 4);
            if (is(chunk, "DSTF")) offsets_.push_back(position);
            position += kChunkHeader + size + (size & 1);
        }
    }

    File file_;
    DffLayout layout_;
    std::vector<uint64_t> offsets_;
};

}  // namespace

std::unique_ptr<Decoder> open_dff_decoder(const std::filesystem::path& path) {
    File file(_wfopen(path.c_str(), L"rb"));
    if (!file) throw std::runtime_error("DFF: cannot open file");
    const DffLayout layout = parse(file.get());
    if (layout.compressed) return std::make_unique<DffDstDecoder>(std::move(file), layout);
    return std::make_unique<DffPlainDecoder>(std::move(file), layout);
}

}  // namespace arpegio
