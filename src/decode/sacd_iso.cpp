#include "decode/sacd_iso.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "decode/framed_dsd.h"

namespace arpegio {

namespace {

constexpr uint32_t kLsnSize = 2048;
constexpr uint32_t kRawSectorSize = 2064;  // raw images carry a 12 byte sector header
constexpr uint32_t kRawHeader = 12;
constexpr uint32_t kMasterTocLsn = 510;
constexpr uint32_t kMasterTocSectors = 10;
constexpr int kFramesPerSecond = 75;
constexpr int kDataTypeAudio = 2;
constexpr int kMaxPackets = 7;

constexpr int kTextTitle = 1;
constexpr int kTextPerformer = 2;
constexpr int kTextComposer = 4;

// Master and area tables of contents store big-endian integers at these offsets.
constexpr size_t kMasterAlbumSetSize = 16;
constexpr size_t kMasterAlbumSequence = 18;
constexpr size_t kMasterArea1Start = 64;
constexpr size_t kMasterArea2Start = 72;
constexpr size_t kMasterArea1Size = 84;
constexpr size_t kMasterArea2Size = 86;
constexpr size_t kMasterYear = 120;
constexpr size_t kMasterLocales = 136;

constexpr size_t kAreaTocSize = 10;
constexpr size_t kAreaSampleRate = 20;
constexpr size_t kAreaFrameFormat = 21;
constexpr size_t kAreaChannels = 32;
constexpr size_t kAreaSpeakers = 33;
constexpr size_t kAreaTrackCount = 69;
constexpr size_t kAreaFirstLsn = 72;
constexpr size_t kAreaLastLsn = 76;
constexpr size_t kAreaLanguages = 88;

constexpr int kFrameFormatDst = 0;

struct FileCloser {
    void operator()(std::FILE* f) const { std::fclose(f); }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

uint32_t be16(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 8 | p[1];
}

uint32_t be32(const uint8_t* p) {
    return be16(p) << 16 | be16(p + 2);
}

uint32_t code_page(uint8_t character_set) {
    switch (character_set & 7) {
    case 2:
    case 7: return 28591;  // ISO 8859-1
    case 3: return 932;    // Music Shift-JIS
    case 4: return 949;
    case 5: return 936;
    case 6: return 950;
    default: return 20127;  // ISO 646
    }
}

std::string to_utf8(const char* text, size_t limit, uint32_t page) {
    const size_t length = strnlen(text, limit);
    if (length == 0) return {};
    const int wide_size = MultiByteToWideChar(page, 0, text, static_cast<int>(length), nullptr, 0);
    if (wide_size <= 0) return std::string(text, length);
    std::wstring wide(wide_size, L'\0');
    MultiByteToWideChar(page, 0, text, static_cast<int>(length), wide.data(), wide_size);
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size, nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size, out.data(), size, nullptr, nullptr);
    return out;
}

// Finds the master table of contents, which fixes the sector size.
uint32_t detect_sector_size(std::FILE* file) {
    char magic[8];
    _fseeki64(file, static_cast<int64_t>(kMasterTocLsn) * kLsnSize, SEEK_SET);
    if (std::fread(magic, 1, 8, file) == 8 && std::memcmp(magic, "SACDMTOC", 8) == 0) return kLsnSize;
    _fseeki64(file, static_cast<int64_t>(kMasterTocLsn) * kRawSectorSize + kRawHeader, SEEK_SET);
    if (std::fread(magic, 1, 8, file) == 8 && std::memcmp(magic, "SACDMTOC", 8) == 0) return kRawSectorSize;
    return 0;
}

bool read_sectors(std::FILE* file, uint32_t sector_size, uint32_t lsn, uint32_t count, uint8_t* out) {
    const uint32_t header = sector_size == kRawSectorSize ? kRawHeader : 0;
    for (uint32_t i = 0; i < count; ++i) {
        _fseeki64(file, static_cast<int64_t>(lsn + i) * sector_size + header, SEEK_SET);
        if (std::fread(out + i * kLsnSize, 1, kLsnSize, file) != kLsnSize) return false;
    }
    return true;
}

uint32_t rate_from_code(uint8_t code) {
    switch (code) {
    case 5: return 128 * 44100;
    case 6: return 256 * 44100;
    case 14: return 64 * 48000;
    case 15: return 128 * 48000;
    case 16: return 256 * 48000;
    default: return 64 * 44100;
    }
}

void read_track_text(const uint8_t* table, const uint8_t* end, SacdArea& area, uint32_t page) {
    for (size_t i = 0; i < area.tracks.size(); ++i) {
        const uint32_t position = be16(table + 8 + i * 2);
        if (position == 0) continue;
        const char* cursor = reinterpret_cast<const char*>(table + position);
        const char* limit = reinterpret_cast<const char*>(end);
        if (cursor >= limit) continue;
        const int entries = static_cast<uint8_t>(*cursor);
        cursor += 4;
        for (int entry = 0; entry < entries && cursor + 2 < limit; ++entry) {
            const int type = static_cast<uint8_t>(*cursor);
            cursor += 2;  // type byte and a fixed 0x20
            const size_t available = static_cast<size_t>(limit - cursor);
            std::string text = to_utf8(cursor, available, page);
            if (type == kTextTitle) area.tracks[i].title = text;
            else if (type == kTextPerformer) area.tracks[i].performer = text;
            else if (type == kTextComposer) area.tracks[i].composer = text;
            while (cursor < limit && *cursor != 0) ++cursor;
            while (cursor < limit && *cursor == 0) ++cursor;
        }
    }
}

SacdArea read_area(std::FILE* file, uint32_t sector_size, uint32_t lsn, uint32_t size_sectors) {
    std::vector<uint8_t> data(static_cast<size_t>(size_sectors) * kLsnSize);
    if (data.empty() || !read_sectors(file, sector_size, lsn, size_sectors, data.data())) {
        throw std::runtime_error("SACD: cannot read area table");
    }
    if (std::memcmp(data.data(), "TWOCHTOC", 8) != 0 && std::memcmp(data.data(), "MULCHTOC", 8) != 0) {
        throw std::runtime_error("SACD: bad area table");
    }

    SacdArea area;
    area.channels = data[kAreaChannels];
    area.stereo = area.channels == 2 && (data[kAreaSpeakers] & 0x1F) == 0;
    area.rate = rate_from_code(data[kAreaSampleRate]);
    area.dst = (data[kAreaFrameFormat] & 0x0F) == kFrameFormatDst;
    area.first_lsn = be32(data.data() + kAreaFirstLsn);
    area.last_lsn = be32(data.data() + kAreaLastLsn);
    const int track_count = data[kAreaTrackCount];
    const uint32_t page = code_page(data[kAreaLanguages + 2]);
    area.tracks.resize(track_count);

    const uint8_t* cursor = data.data() + kLsnSize;
    const uint8_t* end = data.data() + std::min<size_t>(data.size(), be16(data.data() + kAreaTocSize) * size_t{kLsnSize});
    bool text_seen = false;
    while (cursor + 8 < end) {
        if (std::memcmp(cursor, "SACDTTxt", 8) == 0) {
            if (!text_seen) read_track_text(cursor, end, area, page);
            text_seen = true;
            cursor += kLsnSize;
        } else if (std::memcmp(cursor, "SACD_IGL", 8) == 0) {
            cursor += 2 * size_t{kLsnSize};
        } else if (std::memcmp(cursor, "SACD_ACC", 8) == 0) {
            cursor += 32 * size_t{kLsnSize};
        } else if (std::memcmp(cursor, "SACDTRL1", 8) == 0) {
            for (int i = 0; i < track_count; ++i) {
                area.tracks[i].first_lsn = be32(cursor + 8 + i * 4);
                area.tracks[i].length_lsn = be32(cursor + 8 + 255 * 4 + i * 4);
            }
            cursor += kLsnSize;
        } else if (std::memcmp(cursor, "SACDTRL2", 8) == 0) {
            for (int i = 0; i < track_count; ++i) {
                const uint8_t* start = cursor + 8 + i * 4;
                const uint8_t* duration = cursor + 8 + 255 * 4 + i * 4;
                area.tracks[i].start_frame = (start[0] * 60u + start[1]) * kFramesPerSecond + start[2];
                area.tracks[i].duration_frames = (duration[0] * 60u + duration[1]) * kFramesPerSecond + duration[2];
            }
            cursor += kLsnSize;
        } else {
            break;
        }
    }
    return area;
}

class SacdTrackDecoder final : public FramedDsdDecoder {
public:
    SacdTrackDecoder(File file, uint32_t sector_size, const SacdArea& area, const SacdTrack& track)
        : file_(std::move(file)),
          sector_size_(sector_size),
          dst_(area.dst),
          area_first_(area.first_lsn),
          area_last_(area.last_lsn),
          first_lsn_(track.first_lsn),
          start_time_(track.start_frame),
          end_time_(track.start_frame + track.duration_frames) {
        set_stream(area.rate, area.channels, track.duration_frames);
        position_at(first_lsn_);
        wanted_time_ = start_time_;
    }

protected:
    bool next_frame(std::vector<uint8_t>& data, bool& compressed) override {
        for (;;) {
            if (!collect_frame(assembled_, frame_time_)) return false;
            if (frame_time_ >= end_time_) return false;
            if (frame_time_ < wanted_time_) continue;
            data.swap(assembled_);
            compressed = dst_;
            return true;
        }
    }

    void seek_frame(uint64_t frame) override {
        const uint32_t target = start_time_ + static_cast<uint32_t>(frame);

        // Each sector names the frames that begin in it, so the time can be bisected.
        uint32_t low = area_first_;
        uint32_t high = area_last_;
        while (low + 1 < high) {
            const uint32_t middle = low + (high - low) / 2;
            int64_t time = -1;
            uint32_t probe = middle;
            while (probe < high && (time = first_time_in_sector(probe)) < 0) ++probe;
            if (time < 0 || time > target) high = middle;
            else low = probe;
        }
        position_at(low);
        wanted_time_ = target;
    }

private:
    struct Packet {
        bool frame_start;
        int type;
        uint32_t length;
    };

    void position_at(uint32_t lsn) {
        next_lsn_ = lsn;
        packet_index_ = 0;
        packet_count_ = 0;
        assembling_ = false;
        stream_ended_ = false;
        const uint32_t header = sector_size_ == kRawSectorSize ? kRawHeader : 0;
        _fseeki64(file_.get(), static_cast<int64_t>(lsn) * sector_size_ + header, SEEK_SET);
    }

    bool parse_sector(const uint8_t* sector, Packet* packets, int& packet_count, uint32_t* times, int& time_count,
                      size_t& offset) const {
        const uint8_t header = sector[0];
        const bool dst = header & 1;
        time_count = (header >> 2) & 7;
        packet_count = (header >> 5) & 7;
        offset = 1;
        for (int i = 0; i < packet_count; ++i) {
            const uint8_t first = sector[offset];
            packets[i].frame_start = (first >> 7) & 1;
            packets[i].type = (first >> 3) & 7;
            packets[i].length = static_cast<uint32_t>(first & 7) << 8 | sector[offset + 1];
            offset += 2;
        }
        // Plain DSD sectors drop the fourth byte of each frame entry.
        const size_t entry = dst ? 4 : 3;
        for (int i = 0; i < time_count; ++i) {
            const uint8_t* time = sector + offset;
            times[i] = (time[0] * 60u + time[1]) * kFramesPerSecond + time[2];
            offset += entry;
        }
        return true;
    }

    int64_t first_time_in_sector(uint32_t lsn) {
        uint8_t sector[kLsnSize];
        if (!read_sectors(file_.get(), sector_size_, lsn, 1, sector)) return -1;
        Packet packets[kMaxPackets + 1];
        uint32_t times[kMaxPackets + 1];
        int packet_count = 0;
        int time_count = 0;
        size_t offset = 0;
        parse_sector(sector, packets, packet_count, times, time_count, offset);
        return time_count > 0 ? static_cast<int64_t>(times[0]) : -1;
    }

    bool read_next_sector() {
        if (next_lsn_ > area_last_) return false;
        if (std::fread(sector_, 1, kLsnSize, file_.get()) != kLsnSize) return false;
        if (sector_size_ == kRawSectorSize) _fseeki64(file_.get(), kRawSectorSize - kLsnSize, SEEK_CUR);
        ++next_lsn_;
        parse_sector(sector_, packets_, packet_count_, times_, time_count_, offset_);
        packet_index_ = 0;
        time_index_ = 0;
        return true;
    }

    // Gathers one whole frame from the packets of consecutive sectors.
    bool collect_frame(std::vector<uint8_t>& out, uint32_t& time) {
        for (;;) {
            if (packet_index_ >= packet_count_) {
                if (!stream_ended_ && !read_next_sector()) stream_ended_ = true;
                if (stream_ended_) {
                    if (!assembling_) return false;
                    assembling_ = false;
                    out.swap(partial_);
                    time = partial_time_;
                    return true;
                }
                continue;
            }

            const Packet& packet = packets_[packet_index_];
            if (packet.type == kDataTypeAudio) {
                if (packet.frame_start) {
                    if (assembling_) {
                        // The previous frame ends here. This packet is handled on the next call.
                        assembling_ = false;
                        out.swap(partial_);
                        time = partial_time_;
                        return true;
                    }
                    assembling_ = true;
                    partial_.clear();
                    partial_time_ = time_index_ < time_count_ ? times_[time_index_] : partial_time_ + 1;
                    ++time_index_;
                }
                if (assembling_ && offset_ + packet.length <= kLsnSize) {
                    partial_.insert(partial_.end(), sector_ + offset_, sector_ + offset_ + packet.length);
                }
            }
            offset_ += packet.length;
            ++packet_index_;
        }
    }

    File file_;
    uint32_t sector_size_;
    bool dst_;
    uint32_t area_first_;
    uint32_t area_last_;
    uint32_t first_lsn_;
    uint32_t start_time_;
    uint32_t end_time_;
    uint32_t wanted_time_ = 0;

    uint32_t next_lsn_ = 0;
    uint8_t sector_[kLsnSize] = {};
    Packet packets_[kMaxPackets + 1] = {};
    uint32_t times_[kMaxPackets + 1] = {};
    int packet_count_ = 0;
    int packet_index_ = 0;
    int time_count_ = 0;
    int time_index_ = 0;
    size_t offset_ = 0;
    bool assembling_ = false;
    bool stream_ended_ = false;
    std::vector<uint8_t> partial_;
    std::vector<uint8_t> assembled_;
    uint32_t partial_time_ = 0;
    uint32_t frame_time_ = 0;
};

}  // namespace

bool is_sacd_image(const std::filesystem::path& path) {
    File file(_wfopen(path.c_str(), L"rb"));
    return file && detect_sector_size(file.get()) != 0;
}

SacdDisc read_sacd_disc(const std::filesystem::path& path) {
    File file(_wfopen(path.c_str(), L"rb"));
    if (!file) throw std::runtime_error("SACD: cannot open file");
    const uint32_t sector_size = detect_sector_size(file.get());
    if (sector_size == 0) throw std::runtime_error("SACD: not a disc image");

    std::vector<uint8_t> master(kMasterTocSectors * kLsnSize);
    if (!read_sectors(file.get(), sector_size, kMasterTocLsn, kMasterTocSectors, master.data())) {
        throw std::runtime_error("SACD: cannot read the master table");
    }

    SacdDisc disc;
    disc.disc_total = static_cast<int>(be16(master.data() + kMasterAlbumSetSize));
    disc.disc_number = static_cast<int>(be16(master.data() + kMasterAlbumSequence));
    disc.year = static_cast<int>(be16(master.data() + kMasterYear));

    const uint8_t* text = master.data() + kLsnSize;
    if (std::memcmp(text, "SACDText", 8) == 0) {
        const uint32_t page = code_page(master[kMasterLocales + 2]);
        const auto field = [&](size_t index) {
            const uint32_t position = be16(text + 16 + index * 2);
            if (position == 0 || position >= kLsnSize) return std::string{};
            return to_utf8(reinterpret_cast<const char*>(text + position), kLsnSize - position, page);
        };
        disc.album_title = field(0);
        disc.album_artist = field(1);
        disc.disc_title = field(8);
        disc.disc_artist = field(9);
    }

    for (const auto [start_offset, size_offset] : {std::pair{kMasterArea1Start, kMasterArea1Size},
                                                   std::pair{kMasterArea2Start, kMasterArea2Size}}) {
        const uint32_t start = be32(master.data() + start_offset);
        const uint32_t size = be16(master.data() + size_offset);
        if (start != 0 && size != 0) disc.areas.push_back(read_area(file.get(), sector_size, start, size));
    }
    return disc;
}

std::unique_ptr<Decoder> open_sacd_track(const std::filesystem::path& path, size_t area_index, size_t track_index) {
    const SacdDisc disc = read_sacd_disc(path);
    if (area_index >= disc.areas.size() || track_index >= disc.areas[area_index].tracks.size()) {
        throw std::runtime_error("SACD: no such track");
    }
    File file(_wfopen(path.c_str(), L"rb"));
    if (!file) throw std::runtime_error("SACD: cannot open file");
    const uint32_t sector_size = detect_sector_size(file.get());
    const SacdArea& area = disc.areas[area_index];
    return std::make_unique<SacdTrackDecoder>(std::move(file), sector_size, area, area.tracks[track_index]);
}

}  // namespace arpegio
