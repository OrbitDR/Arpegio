#include <windows.h>

#include <objbase.h>

#include <FLAC/stream_encoder.h>
#include <fileref.h>
#include <tag.h>
#include <tpropertymap.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "decode/decoder.h"
#include "decode/range_decoder.h"
#include "library/artwork.h"
#include "library/cue_sheet.h"
#include "library/folder_watcher.h"
#include "library/library.h"
#include "library/library_store.h"
#include "library/metadata.h"
#include "library/mqa.h"
#include "library/paths.h"
#include "library/scanner.h"
#include "library/track_source.h"
#include "test_support.h"

using namespace arpegio;
namespace fs = std::filesystem;

namespace {

// The accented name makes every test also cover Unicode paths.
class TempFolder {
public:
    TempFolder() {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                (L"arpegio_música_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++counter));
        fs::create_directories(path_ / "music");
    }
    ~TempFolder() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    const fs::path& root() const { return path_; }
    fs::path music() const { return path_ / "music"; }
    fs::path database() const { return path_ / "library.db"; }
    fs::path cache() const { return path_ / "cache"; }

private:
    fs::path path_;
};

constexpr uint32_t kRate = 44100;
constexpr int kBytesPerFrame = 4;
constexpr uint64_t kMqaSync = 0xBE0498C88;
constexpr uint32_t kMqaSyncBits = 36;

void put_bytes(std::ofstream& out, const void* data, size_t size) {
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

void put_wav_header(std::ofstream& out, uint16_t bits, uint32_t frames) {
    const uint16_t block = bits / 8 * 2;
    const uint32_t data_size = frames * block;
    const uint32_t riff_size = 36 + data_size;
    const uint16_t pcm = 1;
    const uint16_t channels = 2;
    const uint32_t byte_rate = kRate * block;
    const uint32_t format_size = 16;

    put_bytes(out, "RIFF", 4);
    put_bytes(out, &riff_size, 4);
    put_bytes(out, "WAVEfmt ", 8);
    put_bytes(out, &format_size, 4);
    put_bytes(out, &pcm, 2);
    put_bytes(out, &channels, 2);
    put_bytes(out, &kRate, 4);
    put_bytes(out, &byte_rate, 4);
    put_bytes(out, &block, 2);
    put_bytes(out, &bits, 2);
    put_bytes(out, "data", 4);
    put_bytes(out, &data_size, 4);
}

// 16-bit stereo. The left channel counts up from `first`, so any lost or moved sample is visible.
void write_wav(const fs::path& path, uint32_t frames, int first = 0) {
    std::ofstream out(path, std::ios::binary);
    put_wav_header(out, 16, frames);
    for (uint32_t i = 0; i < frames; ++i) {
        const int16_t left = static_cast<int16_t>((first + static_cast<int>(i)) % 32000);
        const int16_t right = static_cast<int16_t>(-left);
        put_bytes(out, &left, 2);
        put_bytes(out, &right, 2);
    }
}

// One second of stereo whose channels differ by `word`, `repeats` times over. A `lift` of 0 puts it at
// the 16th bit from the top, where MQA starts.
void write_sync_wav(const fs::path& path, uint16_t bits, int repeats, uint64_t word = kMqaSync, int lift = 0) {
    constexpr uint32_t kSpacing = 5000;
    std::ofstream out(path, std::ios::binary);
    put_wav_header(out, bits, kRate);
    for (uint32_t i = 0; i < kRate; ++i) {
        const uint32_t step = i % kSpacing;
        const bool inside = static_cast<int>(i / kSpacing) < repeats && step < kMqaSyncBits;
        const uint32_t flip = inside ? static_cast<uint32_t>((word >> (kMqaSyncBits - 1 - step)) & 1) : 0;
        const uint32_t left = (i & 0x7F) << (bits - 8);
        const uint32_t right = left ^ (flip << (bits - 16 + lift));
        put_bytes(out, &left, bits / 8);
        put_bytes(out, &right, bits / 8);
    }
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

// A real FLAC stream with the same ramp as write_wav, in memory.
std::string encode_flac(uint32_t frames) {
    std::string bytes;
    FLAC__StreamEncoder* encoder = FLAC__stream_encoder_new();
    FLAC__stream_encoder_set_channels(encoder, 2);
    FLAC__stream_encoder_set_bits_per_sample(encoder, 16);
    FLAC__stream_encoder_set_sample_rate(encoder, kRate);
    FLAC__stream_encoder_set_total_samples_estimate(encoder, frames);
    const auto write = [](const FLAC__StreamEncoder*, const FLAC__byte data[], size_t size, uint32_t, uint32_t,
                          void* context) {
        static_cast<std::string*>(context)->append(reinterpret_cast<const char*>(data), size);
        return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    };
    FLAC__stream_encoder_init_stream(encoder, write, nullptr, nullptr, nullptr, &bytes);
    std::vector<FLAC__int32> samples(size_t{frames} * 2);
    for (uint32_t i = 0; i < frames; ++i) {
        samples[size_t{i} * 2] = static_cast<FLAC__int32>(i % 32000);
        samples[size_t{i} * 2 + 1] = -static_cast<FLAC__int32>(i % 32000);
    }
    FLAC__stream_encoder_process_interleaved(encoder, samples.data(), frames);
    FLAC__stream_encoder_finish(encoder);
    FLAC__stream_encoder_delete(encoder);
    return bytes;
}

// An ID3v2.3 header announcing `body` bytes of padding.
std::string id3_tag(uint32_t body) {
    std::string tag = "ID3";
    tag.push_back(3);
    tag.push_back(0);
    tag.push_back(0);
    for (const int shift : {21, 14, 7, 0}) tag.push_back(static_cast<char>((body >> shift) & 0x7F));
    tag.append(body, '\0');
    return tag;
}

void tag_file(const fs::path& path, const char* title, const char* artist, const char* album, int track, int year,
              const char* album_artist = nullptr) {
    TagLib::FileRef file(path.c_str());
    file.tag()->setTitle(TagLib::String(title, TagLib::String::UTF8));
    file.tag()->setArtist(TagLib::String(artist, TagLib::String::UTF8));
    file.tag()->setAlbum(TagLib::String(album, TagLib::String::UTF8));
    file.tag()->setTrack(track);
    file.tag()->setYear(year);
    file.save();
    if (album_artist) {
        TagLib::PropertyMap properties = file.properties();
        properties.replace("ALBUMARTIST", TagLib::StringList(TagLib::String(album_artist, TagLib::String::UTF8)));
        file.setProperties(properties);
        file.save();
    }
}

// Solid 24-bit image.
std::vector<uint8_t> make_bmp(int width, int height) {
    const int row = (width * 3 + 3) & ~3;
    const int data_size = row * height;
    std::vector<uint8_t> image(54 + data_size, 0x7F);
    const auto put32 = [&image](size_t offset, int value) { std::memcpy(image.data() + offset, &value, 4); };
    image[0] = 'B';
    image[1] = 'M';
    put32(2, static_cast<int>(image.size()));
    put32(6, 0);
    put32(10, 54);
    put32(14, 40);
    put32(18, width);
    put32(22, height);
    image[26] = 1;
    image[27] = 0;
    image[28] = 24;
    image[29] = 0;
    put32(30, 0);
    put32(34, data_size);
    put32(38, 2835);
    put32(42, 2835);
    put32(46, 0);
    put32(50, 0);
    return image;
}

void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<int> read_left_channel(Decoder& decoder, size_t frames) {
    std::vector<double> buffer(frames * 2);
    std::vector<int> out;
    while (out.size() < frames) {
        const size_t got = decoder.read_pcm(buffer.data(), (frames - out.size()));
        if (got == 0) break;
        for (size_t i = 0; i < got; ++i) out.push_back(static_cast<int>(std::lround(buffer[i * 2] * 32768.0)));
    }
    return out;
}

// A stale row would not show in results, since a search joins with the tracks, but it stays in the index.
int64_t search_rows(LibraryStore& store) {
    return store.database().prepare("SELECT COUNT(*) FROM tracks_fts").single();
}

const Track* find_title(const std::vector<Track>& tracks, const char* title) {
    const auto found =
        std::find_if(tracks.begin(), tracks.end(), [title](const Track& track) { return track.title == title; });
    return found == tracks.end() ? nullptr : &*found;
}

// Bytes in a repeating pattern, so a slice can be checked by position.
class PatternDsdDecoder final : public Decoder {
public:
    explicit PatternDsdDecoder(uint64_t bytes) { format_ = {true, 2822400, 2, 1, bytes}; }

    const SourceFormat& format() const override { return format_; }

    size_t read_dsd(uint8_t* const* planar, size_t bytes) override {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(bytes, format_.length - position_));
        for (size_t i = 0; i < count; ++i) {
            planar[0][i] = static_cast<uint8_t>(position_ + i);
            planar[1][i] = static_cast<uint8_t>(~(position_ + i));
        }
        position_ += count;
        return count;
    }

    void seek(uint64_t position) override { position_ = position; }

private:
    SourceFormat format_;
    uint64_t position_ = 0;
};

constexpr const char* kCueAlbum =
    "PERFORMER \"Cue Artist\"\n"
    "TITLE \"Cue Album\"\n"
    "REM GENRE \"Jazz\"\n"
    "REM DATE 1999\n"
    "FILE \"album.wav\" WAVE\n"
    "  TRACK 01 AUDIO\n"
    "    TITLE \"Cue One\"\n"
    "    INDEX 01 00:00:00\n"
    "  TRACK 02 AUDIO\n"
    "    TITLE \"Cue Two\"\n"
    "    PERFORMER \"Guest\"\n"
    "    FLAGS PRE\n"
    "    INDEX 00 00:00:04\n"
    "    INDEX 01 00:00:05\n";

constexpr uint32_t kCueAlbumFrames = 3000;
constexpr uint32_t kCueSplitFrame = 2940;  // 5 CD frames of 588 samples

}  // namespace

TEST(cue_parses_tracks_and_ranges) {
    const CueSheet sheet = parse_cue(
        "REM GENRE \"Rock\"\n"
        "TITLE \"Album\"\n"
        "PERFORMER \"Artist\"\n"
        "FILE \"album.flac\" WAVE\n"
        "  TRACK 01 AUDIO\n"
        "    TITLE \"Track 1\"\n"
        "    INDEX 00 00:00:00\n"
        "    INDEX 01 00:02:00\n"
        "  TRACK 02 AUDIO\n"
        "    TITLE \"Track 2\"\n"
        "    SONGWRITER \"Writer\"\n"
        "    FLAGS PRE DCP\n"
        "    PREGAP 00:01:00\n"
        "    INDEX 01 03:15:37\n");
    CHECK(sheet.title == "Album");
    CHECK(sheet.performer == "Artist");
    CHECK(sheet.rem.at("GENRE") == "Rock");
    CHECK(sheet.tracks.size() == 2);
    CHECK(sheet.tracks[0].start == 150);
    CHECK(sheet.tracks[0].end == 14662);
    CHECK(sheet.tracks[1].start == 14662);
    CHECK(sheet.tracks[1].end == 0);
    CHECK(sheet.tracks[1].pre_emphasis);
    CHECK(sheet.tracks[1].songwriter == "Writer");
}

TEST(cue_with_one_file_per_track) {
    const CueSheet sheet = parse_cue(
        "FILE \"one.wav\" WAVE\n"
        "  TRACK 01 AUDIO\n"
        "    INDEX 01 00:00:00\n"
        "FILE \"two.wav\" WAVE\n"
        "  TRACK 02 AUDIO\n"
        "    INDEX 01 00:00:10\n");
    CHECK(sheet.tracks.size() == 2);
    CHECK(sheet.tracks[0].file == "one.wav");
    CHECK(sheet.tracks[0].end == 0);
    CHECK(sheet.tracks[1].file == "two.wav");
    CHECK(sheet.tracks[1].start == 10);
}

// EAC's default layout. The gap stays at the end of the previous file, so a FILE line lands between
// INDEX 00 and INDEX 01. AuroraPlayer rejected these sheets.
TEST(cue_with_gaps_appended_to_the_previous_file) {
    const CueSheet sheet = parse_cue(
        "FILE \"01.wav\" WAVE\n"
        "  TRACK 01 AUDIO\n"
        "    INDEX 01 00:00:00\n"
        "  TRACK 02 AUDIO\n"
        "    INDEX 00 04:00:00\n"
        "FILE \"02.wav\" WAVE\n"
        "    INDEX 01 00:00:00\n");
    CHECK(sheet.tracks.size() == 2);
    CHECK(sheet.tracks[0].end == 0);
    CHECK(sheet.tracks[1].file == "02.wav");
    CHECK(sheet.tracks[1].start == 0);
}

TEST(cue_leaves_data_tracks_out) {
    const CueSheet sheet = parse_cue(
        "FILE \"disc.wav\" WAVE\n"
        "  TRACK 01 AUDIO\n"
        "    INDEX 01 00:00:00\n"
        "  TRACK 02 MODE1/2352\n"
        "    INDEX 01 10:00:00\n");
    CHECK(sheet.tracks.size() == 1);
    CHECK(sheet.tracks[0].end == 45000);
}

TEST(cue_rejects_broken_sheets) {
    const auto rejects = [](const char* text) {
        try {
            parse_cue(text);
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    CHECK(rejects(""));
    CHECK(rejects("TRACK 01 AUDIO\n"));
    CHECK(rejects("FILE \"a.wav\" WAVE\n  TRACK 01 AUDIO\n"));
    CHECK(rejects("FILE \"a.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:61:00\n"));
    CHECK(rejects("FILE \"a.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:10:100\n"));
    CHECK(
        rejects("FILE \"a.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 01:00:00\n  TRACK 02 AUDIO\n    INDEX 01 00:30:00\n"));
}

// Two sheets in the real collection have times like this. Refusing them left an album as one track.
TEST(cue_accepts_frame_counts_past_the_limit) {
    const CueSheet sheet = parse_cue("FILE \"a.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 03:22:90\n");
    CHECK(sheet.tracks[0].start == (3 * 60 + 22) * 75 + 90);
}

TEST(cue_files_decode_their_text_encoding) {
    TempFolder folder;
    const fs::path path = folder.root() / "encoded.cue";
    const std::wstring wide = L"TITLE \"Canción\"\r\nFILE \"a.wav\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\n";

    std::string utf16 = "\xFF\xFE";
    utf16.append(reinterpret_cast<const char*>(wide.data()), wide.size() * sizeof(wchar_t));
    write_text(path, utf16);
    CHECK(parse_cue_file(path).title == "Canción");

    write_text(path, "\xEF\xBB\xBFTITLE \"Canción\"\nFILE \"a.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");
    CHECK(parse_cue_file(path).title == "Canción");

    // Windows-1252, as older rippers wrote it. The test assumes that code page on this machine.
    if (GetACP() == 1252) {
        write_text(path, "TITLE \"Canci\xF3n\"\nFILE \"a.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");
        CHECK(parse_cue_file(path).title == "Canción");
    }
}

TEST(range_decoder_plays_exactly_its_stretch) {
    TempFolder folder;
    const fs::path path = folder.root() / "range.wav";
    write_wav(path, 500);

    auto decoder = make_range_decoder(open_decoder(path), 100, 300);
    CHECK(decoder->format().length == 200);
    const std::vector<int> head = read_left_channel(*decoder, 1000);
    CHECK(head.size() == 200);
    CHECK(head.front() == 100);
    CHECK(head.back() == 299);

    decoder->seek(50);
    const std::vector<int> tail = read_left_channel(*decoder, 1000);
    CHECK(tail.size() == 150);
    CHECK(tail.front() == 150);

    decoder->seek(100000);
    CHECK(read_left_channel(*decoder, 10).empty());
}

TEST(adjacent_ranges_rebuild_the_file_without_a_gap) {
    TempFolder folder;
    const fs::path path = folder.root() / "whole.wav";
    write_wav(path, 3000);

    std::vector<int> joined;
    for (const auto& [start, end] : {std::pair<uint64_t, uint64_t>{0, 1177}, {1177, 1178}, {1178, 3000}}) {
        auto decoder = make_range_decoder(open_decoder(path), start, end);
        const std::vector<int> part = read_left_channel(*decoder, 3000);
        joined.insert(joined.end(), part.begin(), part.end());
    }
    CHECK(joined.size() == 3000);
    for (size_t i = 0; i < joined.size(); ++i) CHECK(joined[i] == static_cast<int>(i));
}

TEST(range_decoder_counts_dsd_in_bytes_per_channel) {
    auto decoder = make_range_decoder(std::make_unique<PatternDsdDecoder>(10000), 4704, 9408);
    CHECK(decoder->format().dsd);
    CHECK(decoder->format().length == 4704);

    std::vector<uint8_t> left(8192);
    std::vector<uint8_t> right(8192);
    uint8_t* planes[] = {left.data(), right.data()};
    CHECK(decoder->read_dsd(planes, 8192) == 4704);
    CHECK(left[0] == static_cast<uint8_t>(4704));
    CHECK(right[0] == static_cast<uint8_t>(~4704));
    CHECK(left[4703] == static_cast<uint8_t>(9407));
    CHECK(decoder->read_dsd(planes, 8192) == 0);

    decoder->seek(10);
    CHECK(decoder->read_dsd(planes, 1) == 1);
    CHECK(left[0] == static_cast<uint8_t>(4714));
}

TEST(range_decoder_refuses_a_stretch_outside_the_source) {
    bool threw = false;
    try {
        make_range_decoder(std::make_unique<PatternDsdDecoder>(100), 50, 101);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

// Found in the real collection. It went to the MP3 decoder and divided by zero channels.
TEST(flac_behind_an_id3_tag_opens_as_flac) {
    TempFolder folder;
    const fs::path path = folder.root() / "tagged.flac";
    write_text(path, id3_tag(2000) + encode_flac(5000));

    auto decoder = open_decoder(path);
    CHECK(decoder->format().rate == kRate);
    CHECK(decoder->format().length == 5000);
    const std::vector<int> samples = read_left_channel(*decoder, 5000);
    CHECK(samples.size() == 5000);
    CHECK(samples[1234] == 1234);
}

// Also from the collection: a valid FLAC whose header does not say how long it is.
TEST(flac_without_a_total_in_its_header_gets_measured) {
    TempFolder folder;
    const fs::path path = folder.root() / "piped.flac";
    constexpr size_t kTotalSamplesOffset = 21;  // low nibble, then four bytes
    std::string stream = encode_flac(50000);
    stream[kTotalSamplesOffset] = static_cast<char>(stream[kTotalSamplesOffset] & 0xF0);
    std::fill_n(stream.begin() + kTotalSamplesOffset + 1, 4, '\0');
    write_text(path, stream);

    auto decoder = open_decoder(path);
    CHECK(decoder->format().length == 50000);
    CHECK(read_left_channel(*decoder, 4)[3] == 3);
    decoder->seek(40000);
    CHECK(read_left_channel(*decoder, 1)[0] == 8000);
}

TEST(id3_tag_with_nothing_playable_behind_it_is_an_error) {
    TempFolder folder;
    const fs::path path = folder.root() / "empty.mp3";
    write_text(path, id3_tag(100) + std::string(5000, 'x'));

    bool threw = false;
    try {
        open_decoder(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(metadata_reads_tags_without_touching_the_file) {
    TempFolder folder;
    const fs::path path = folder.root() / "tagged.wav";
    write_wav(path, 1000);
    tag_file(path, "Título", "Björk", "Álbum", 7, 2024, "Sello");

    const auto modified = fs::last_write_time(path);
    const Metadata metadata = read_metadata(path);
    CHECK(fs::last_write_time(path) == modified);
    CHECK(metadata.title == "Título");
    CHECK(metadata.artist == "Björk");
    CHECK(metadata.album == "Álbum");
    CHECK(metadata.album_artist == "Sello");
    CHECK(metadata.track_number == 7);
    CHECK(metadata.year == 2024);
    CHECK(metadata.codec == "WAV");
}

TEST(metadata_of_an_unreadable_file_is_empty) {
    TempFolder folder;
    const fs::path path = folder.root() / "broken.flac";
    write_text(path, "not audio");
    const Metadata metadata = read_metadata(path);
    CHECK(metadata.title.empty());
    CHECK(metadata.codec == "FLAC");
}

TEST(metadata_trims_stray_spaces) {
    TempFolder folder;
    const fs::path path = folder.root() / "spaces.wav";
    write_wav(path, 1000);
    tag_file(path, " Title ", "José José ", "Album  ", 1, 2000);
    const Metadata metadata = read_metadata(path);
    CHECK(metadata.title == "Title");
    CHECK(metadata.artist == "José José");
    CHECK(metadata.album == "Album");
}

TEST(folder_artwork_prefers_usual_names) {
    TempFolder folder;
    CHECK(read_folder_artwork(folder.root()).empty());
    const std::vector<uint8_t> front = make_bmp(2, 2);
    write_bytes(folder.root() / "FOLDER.BMP", front);
    write_bytes(folder.root() / "back.bmp", make_bmp(3, 3));
    write_bytes(folder.root() / "disc.bmp", make_bmp(4, 4));
    CHECK(read_folder_artwork(folder.root()) == front);
}

TEST(folder_artwork_takes_a_lone_image_of_any_name) {
    TempFolder folder;
    const std::vector<uint8_t> image = make_bmp(2, 2);
    write_bytes(folder.root() / "1c2332e0-9d5b-4aac.bmp", image);
    CHECK(read_folder_artwork(folder.root()) == image);
}

TEST(folder_artwork_does_not_guess_between_several_images) {
    TempFolder folder;
    write_bytes(folder.root() / "one.bmp", make_bmp(2, 2));
    write_bytes(folder.root() / "two.bmp", make_bmp(3, 3));
    CHECK(read_folder_artwork(folder.root()).empty());
}

TEST(folder_artwork_looks_in_scan_folders) {
    TempFolder folder;
    fs::create_directories(folder.root() / "Covers");
    const std::vector<uint8_t> front = make_bmp(2, 2);
    write_bytes(folder.root() / "Covers" / "Front.bmp", front);
    write_bytes(folder.root() / "Covers" / "Back.bmp", make_bmp(3, 3));
    CHECK(read_folder_artwork(folder.root()) == front);
}

TEST(artwork_cache_stores_original_and_thumbnail) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    TempFolder folder;
    ArtworkCache cache(folder.cache());

    const std::vector<uint8_t> image = make_bmp(1024, 256);
    const std::optional<ArtworkFile> stored = cache.store(image);
    CHECK(stored.has_value());
    if (!stored) return;
    CHECK(stored->width == 1024);
    CHECK(stored->height == 256);
    CHECK(stored->mime == "image/bmp");
    CHECK(fs::exists(cache.original_path(*stored)));
    CHECK(fs::file_size(cache.original_path(*stored)) == image.size());

    const fs::path thumbnail = cache.thumbnail_path(*stored);
    CHECK(thumbnail.extension() == ".jpg");
    std::ifstream input(thumbnail, std::ios::binary);
    char magic[3] = {};
    input.read(magic, 3);
    CHECK(std::memcmp(magic, "\xFF\xD8\xFF", 3) == 0);

    const std::optional<ArtworkFile> again = cache.store(image);
    CHECK(again.has_value() && again->hash == stored->hash);
}

TEST(artwork_cache_rejects_what_is_not_an_image) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    TempFolder folder;
    ArtworkCache cache(folder.cache());
    CHECK(!cache.store({}).has_value());
    CHECK(!cache.store({1, 2, 3, 4}).has_value());
    CHECK(!cache.store({0xFF, 0xD8, 0xFF, 0x00}).has_value());  // says JPEG, is not
}

TEST(mqa_is_found_at_the_bits_it_uses) {
    TempFolder folder;
    const fs::path path = folder.music() / "sync.wav";
    for (const uint16_t bits : {uint16_t{16}, uint16_t{24}}) {
        for (const int lift : {0, 1, 2}) {
            write_sync_wav(path, bits, 3, kMqaSync, lift);
            CHECK(has_mqa_sync(*open_decoder(path)));
        }
    }
}

TEST(mqa_takes_more_than_a_lucky_match) {
    TempFolder folder;
    const fs::path path = folder.music() / "sync.wav";
    write_sync_wav(path, 16, 1);
    CHECK(!has_mqa_sync(*open_decoder(path)));
    write_sync_wav(path, 24, 3, kMqaSync ^ 1);
    CHECK(!has_mqa_sync(*open_decoder(path)));
    write_sync_wav(path, 16, 3, kMqaSync, 3);
    CHECK(!has_mqa_sync(*open_decoder(path)));
    write_wav(path, kRate);
    CHECK(!has_mqa_sync(*open_decoder(path)));
}

TEST(scan_marks_mqa_files_and_their_cue_tracks) {
    TempFolder folder;
    const fs::path music = folder.music();
    write_sync_wav(music / "sync.wav", 16, 3);
    write_sync_wav(music / "whole.wav", 24, 3);
    write_wav(music / "plain.wav", kRate);
    write_text(music / "sync.cue",
               "FILE \"sync.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n"
               "  TRACK 02 AUDIO\n    INDEX 01 00:00:30\n");

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    scan_folder(store, artwork, music);
    const std::vector<Track> tracks = store.tracks();
    CHECK(tracks.size() == 4);
    for (const Track& track : tracks) CHECK(track.mqa == (track.relative_path != "plain.wav"));
}

TEST(older_catalogues_read_their_lossless_files_again) {
    TempFolder folder;
    const fs::path music = folder.music();
    write_sync_wav(music / "sync.wav", 16, 3);

    ArtworkCache artwork(folder.cache());
    TrackId id = 0;
    FolderId root = 0;
    {
        LibraryStore store(folder.database());
        scan_folder(store, artwork, music);
        id = store.tracks().at(0).id;
        root = store.folders().at(0).id;

        Track mp3;
        mp3.relative_path = "lossy.mp3";
        mp3.codec = "MP3";
        mp3.stamp = {5, 7};
        mp3.format = {false, kRate, 2, 16, kRate};
        store.upsert_track(root, mp3, 0, nullptr);

        // Version 1 had no such column, and nothing in it says which files are MQA.
        store.database().exec("ALTER TABLE tracks DROP COLUMN is_mqa");
        store.database().set_user_version(1);
    }

    LibraryStore store(folder.database());
    CHECK(store.schema_version() == 2);
    CHECK(!store.track(id)->mqa);
    const std::map<std::string, KnownFile> known = store.known_files(root);
    CHECK(known.at("sync.wav").stamp.modified == 0);
    CHECK(known.at("lossy.mp3").stamp.modified == 7);

    const ScanResult result = scan_folder(store, artwork, music);
    CHECK(result.unchanged == 0);
    CHECK(result.tracks_written == 1);
    CHECK(store.track(id)->mqa);
    CHECK(scan_folder(store, artwork, music).unchanged == 1);
}

TEST(albums_take_the_section_of_most_of_their_tracks) {
    TempFolder folder;
    LibraryStore store(folder.database());
    const FolderId root = store.add_folder(folder.music());

    int next = 0;
    const auto add = [&](const char* album, const char* codec, uint32_t rate, int bits, bool mqa = false, bool dsd = false) {
        Track track;
        track.relative_path = std::to_string(++next);
        track.album = album;
        track.artist = "Artist";
        track.codec = codec;
        track.format = {dsd, rate, 2, bits, rate};
        track.mqa = mqa;
        store.upsert_track(root, track, 0, nullptr);
    };

    add("cd", "FLAC", 44100, 16);
    add("16 at 48", "WAV", 48000, 16);
    add("24 at 44", "FLAC", 44100, 24);
    add("16 at 96", "FLAC", 96000, 16);
    add("24 at 96", "WavPack", 96000, 24);
    add("mqa cd", "FLAC", 44100, 16, true);
    add("mqa 24", "FLAC", 48000, 24, true);
    add("dsd", "DSF", 5644800, 1, false, true);
    add("mp3", "MP3", 44100, 16);
    add("big aac", "AAC", 96000, 24);

    add("mostly mqa", "FLAC", 44100, 16, true);
    add("mostly mqa", "FLAC", 44100, 16, true);
    add("mostly mqa", "FLAC", 44100, 16);
    add("mostly cd", "FLAC", 44100, 16, true);
    add("mostly cd", "FLAC", 44100, 16);
    add("mostly cd", "FLAC", 44100, 16);
    add("tied", "MP3", 44100, 16);
    add("tied", "FLAC", 96000, 24);

    std::map<std::string, AlbumSection> sections;
    for (const Album& album : store.albums()) sections[album.title] = album.section;
    CHECK(sections.size() == 13);
    CHECK(sections.at("cd") == AlbumSection::Cd);
    CHECK(sections.at("16 at 48") == AlbumSection::HighRes);
    CHECK(sections.at("24 at 44") == AlbumSection::HighRes);
    CHECK(sections.at("16 at 96") == AlbumSection::HighRes);
    CHECK(sections.at("24 at 96") == AlbumSection::HighRes);
    CHECK(sections.at("mqa cd") == AlbumSection::Mqa);
    CHECK(sections.at("mqa 24") == AlbumSection::Mqa);
    CHECK(sections.at("dsd") == AlbumSection::Dsd);
    CHECK(sections.at("mp3") == AlbumSection::Lossy);
    CHECK(sections.at("big aac") == AlbumSection::Lossy);
    CHECK(sections.at("mostly mqa") == AlbumSection::Mqa);
    CHECK(sections.at("mostly cd") == AlbumSection::Cd);
    CHECK(sections.at("tied") == AlbumSection::HighRes);
}

TEST(store_keeps_its_schema_across_opens) {
    TempFolder folder;
    {
        LibraryStore store(folder.database());
        CHECK(store.schema_version() == 2);
        store.add_folder(folder.music());
    }
    {
        LibraryStore store(folder.database());
        CHECK(store.schema_version() == 2);
        CHECK(store.folders().size() == 1);
        CHECK(store.add_folder(folder.music()) == store.folders()[0].id);
        store.database().set_user_version(99);
    }
    bool threw = false;
    try {
        LibraryStore store(folder.database());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(scan_catalogues_files_and_cue_tracks) {
    TempFolder folder;
    const fs::path music = folder.music();
    write_wav(music / "one.wav", 1000);
    tag_file(music / "one.wav", "One", "Artist", "Album", 1, 2024);
    write_wav(music / "album.wav", kCueAlbumFrames);
    write_text(music / "album.cue", kCueAlbum);
    write_bytes(music / "cover.bmp", make_bmp(8, 8));

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    uint64_t last_done = 0;
    uint64_t last_total = 0;
    const ScanResult result = scan_folder(store, artwork, music, {}, [&](uint64_t done, uint64_t total, const std::string&) {
        last_done = done;
        last_total = total;
    });
    CHECK(result.warnings.empty());
    CHECK(result.files == 3);
    CHECK(result.tracks_written == 3);
    CHECK(result.cue_sheets == 1);
    CHECK(last_done == 3);
    CHECK(last_total == 3);

    const std::vector<Track> tracks = store.tracks();
    CHECK(tracks.size() == 3);
    const Track* one = find_title(tracks, "One");
    const Track* cue_one = find_title(tracks, "Cue One");
    const Track* cue_two = find_title(tracks, "Cue Two");
    CHECK(one && cue_one && cue_two);
    if (!one || !cue_one || !cue_two) return;

    CHECK(one->artist == "Artist");
    CHECK(one->album == "Album");
    CHECK(one->album_artist == "Artist");
    CHECK(one->part == 0);
    CHECK(one->range_end == 0);
    CHECK(one->format.length == 1000);
    CHECK(one->format.rate == kRate);
    CHECK(one->format.bits == 16);

    CHECK(cue_one->part == 1);
    CHECK(cue_one->range_start == 0);
    CHECK(cue_one->range_end == kCueSplitFrame);
    CHECK(cue_one->format.length == kCueSplitFrame);
    CHECK(cue_one->album == "Cue Album");
    CHECK(cue_one->artist == "Cue Artist");
    CHECK(cue_one->genre == "Jazz");
    CHECK(cue_one->year == 1999);

    CHECK(cue_two->part == 2);
    CHECK(cue_two->range_start == kCueSplitFrame);
    CHECK(cue_two->format.length == kCueAlbumFrames - kCueSplitFrame);
    CHECK(cue_two->artist == "Guest");
    CHECK(cue_two->album_artist == "Cue Artist");
    CHECK(cue_two->pre_emphasis);
    CHECK(cue_two->artwork_id != 0);
    CHECK(cue_two->album_artwork_id != 0);

    const std::optional<ArtworkFile> cover = store.artwork(cue_two->artwork_id);
    CHECK(cover.has_value() && cover->width == 8);
    if (cover) CHECK(fs::exists(artwork.thumbnail_path(*cover)));

    auto decoder = make_queue_item(*cue_two).open();
    CHECK(decoder->format().length == kCueAlbumFrames - kCueSplitFrame);
    const std::vector<int> samples = read_left_channel(*decoder, 1000);
    CHECK(samples.size() == 60);
    CHECK(samples.front() == static_cast<int>(kCueSplitFrame));
}

TEST(scan_skips_what_did_not_change) {
    TempFolder folder;
    const fs::path music = folder.music();
    write_wav(music / "a.wav", 1000);
    write_wav(music / "b.wav", 1000);
    write_wav(music / "album.wav", kCueAlbumFrames);
    write_text(music / "album.cue", kCueAlbum);

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    scan_folder(store, artwork, music);
    const TrackId id = find_title(store.tracks(), "a") ? find_title(store.tracks(), "a")->id : 0;
    CHECK(id != 0);
    store.set_favorite(id, true);

    ScanResult result = scan_folder(store, artwork, music);
    CHECK(result.tracks_written == 0);
    CHECK(result.tracks_removed == 0);
    CHECK(result.unchanged == 3);
    CHECK(store.tracks().size() == 4);

    // A retagged file is read again, keeps its id and leaves no trace of its old album.
    tag_file(music / "a.wav", "Retagged", "Someone", "New Album", 1, 2001);
    result = scan_folder(store, artwork, music);
    CHECK(result.tracks_written == 1);
    CHECK(result.unchanged == 2);
    const std::vector<Track> tracks = store.tracks();
    const Track* retagged = find_title(tracks, "Retagged");
    CHECK(retagged && retagged->id == id && retagged->favorite);
    for (const Album& album : store.albums()) CHECK(album.track_count > 0);

    result = scan_folder(store, artwork, music, ScanOptions{.force = true});
    CHECK(result.unchanged == 0);
    CHECK(result.tracks_written == 4);
    CHECK(store.tracks().size() == 4);
}

TEST(scan_follows_files_coming_and_going) {
    TempFolder folder;
    const fs::path music = folder.music();
    write_wav(music / "stays.wav", 1000);
    write_wav(music / "goes.wav", 1000);
    tag_file(music / "goes.wav", "Goes", "Leaving", "Gone Album", 1, 2000);

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    scan_folder(store, artwork, music);
    CHECK(!store.search("Goes").empty());

    fs::remove(music / "goes.wav");
    write_wav(music / "new.wav", 1000);
    const ScanResult result = scan_folder(store, artwork, music);
    CHECK(result.tracks_written == 1);
    CHECK(result.tracks_removed == 1);
    CHECK(store.tracks().size() == 2);
    CHECK(store.search("Goes").empty());
    CHECK(store.search("Leaving").empty());
    CHECK(search_rows(store) == 2);
    for (const Artist& artist : store.artists()) CHECK(artist.name != "Leaving");
}

TEST(scan_switches_between_a_file_and_its_cue_sheet) {
    TempFolder folder;
    const fs::path music = folder.music();
    write_wav(music / "album.wav", kCueAlbumFrames);

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    scan_folder(store, artwork, music);
    CHECK(store.tracks().size() == 1);

    write_text(music / "album.cue", kCueAlbum);
    scan_folder(store, artwork, music);
    CHECK(store.tracks().size() == 2);
    CHECK(store.tracks()[0].part == 1);
    CHECK(scan_folder(store, artwork, music).unchanged == 1);

    fs::remove(music / "album.cue");
    const ScanResult result = scan_folder(store, artwork, music);
    CHECK(result.tracks_written == 1);
    CHECK(result.tracks_removed == 2);
    const std::vector<Track> tracks = store.tracks();
    CHECK(tracks.size() == 1);
    CHECK(tracks[0].part == 0);
    CHECK(tracks[0].range_end == 0);
    CHECK(tracks[0].format.length == kCueAlbumFrames);
}

TEST(scan_reads_a_cue_sheet_again_when_it_changes) {
    TempFolder folder;
    const fs::path music = folder.music();
    write_wav(music / "album.wav", kCueAlbumFrames);
    write_text(music / "album.cue", kCueAlbum);

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    scan_folder(store, artwork, music);

    // Same size on purpose, so only the date tells. Windows stamps files with a clock of about 15 ms,
    // which a test outruns, so the date is moved by hand.
    std::string edited = kCueAlbum;
    edited.replace(edited.find("Cue Two"), 7, "Renamed");
    const auto modified = fs::last_write_time(music / "album.cue");
    write_text(music / "album.cue", edited);
    fs::last_write_time(music / "album.cue", modified + std::chrono::hours(1));
    const ScanResult result = scan_folder(store, artwork, music);
    CHECK(result.tracks_written == 2);
    CHECK(find_title(store.tracks(), "Renamed") != nullptr);
    CHECK(store.tracks().size() == 2);
}

TEST(scan_finds_cue_audio_with_another_name) {
    TempFolder folder;
    const fs::path music = folder.music();

    // The sheet names the .flac it was ripped to; the file on disk has another extension and case.
    write_wav(music / "Disc.wav", kCueAlbumFrames);
    std::string renamed = kCueAlbum;
    renamed.replace(renamed.find("album.wav"), 9, "disc.flac");
    write_text(music / "disc.cue", renamed);

    // The ripper's temporary name. Only the sheet's own name connects it to the audio.
    fs::create_directories(music / "other");
    write_wav(music / "other" / "rip.wav", kCueAlbumFrames);
    std::string temporary = kCueAlbum;
    temporary.replace(temporary.find("album.wav"), 9, "CDImage.wav");
    write_text(music / "other" / "rip.cue", temporary);

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    const ScanResult result = scan_folder(store, artwork, music);
    CHECK(result.warnings.empty());
    CHECK(result.cue_sheets == 2);
    CHECK(store.tracks().size() == 4);
    for (const Track& track : store.tracks()) CHECK(track.part != 0);
}

TEST(scan_lets_only_one_sheet_claim_a_file) {
    TempFolder folder;
    const fs::path music = folder.music();
    write_wav(music / "album.wav", kCueAlbumFrames);
    write_text(music / "album.cue", kCueAlbum);
    write_text(music / "album (eac).cue", kCueAlbum);

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    const ScanResult result = scan_folder(store, artwork, music);
    CHECK(result.cue_sheets == 1);
    CHECK(result.duplicate_sheets == 1);
    CHECK(result.warnings.empty());
    CHECK(store.tracks().size() == 2);
}

TEST(scan_splits_a_file_with_an_embedded_sheet) {
    TempFolder folder;
    const fs::path music = folder.music();
    write_wav(music / "album.wav", kCueAlbumFrames);
    {
        TagLib::FileRef file((music / "album.wav").c_str());
        TagLib::PropertyMap properties = file.properties();
        properties.replace("CUESHEET", TagLib::StringList(TagLib::String(kCueAlbum, TagLib::String::UTF8)));
        file.setProperties(properties);
        file.save();
    }
    CHECK(!read_metadata(music / "album.wav").cue_sheet.empty());

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    scan_folder(store, artwork, music);

    const std::vector<Track> tracks = store.tracks();
    CHECK(tracks.size() == 2);
    CHECK(find_title(tracks, "Cue Two") != nullptr);
    const ScanResult again = scan_folder(store, artwork, music);
    CHECK(again.unchanged == 1);
    CHECK(again.tracks_written == 0);
    CHECK(store.tracks().size() == 2);
}

TEST(scan_warns_and_carries_on_past_bad_files) {
    TempFolder folder;
    const fs::path music = folder.music();
    write_wav(music / "good.wav", 1000);
    write_text(music / "bad.flac", "not audio");
    write_text(music / "bad.cue", "this is not a sheet");
    write_text(music / "missing.cue", "FILE \"gone.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    const ScanResult result = scan_folder(store, artwork, music);
    CHECK(result.warnings.size() == 3);
    CHECK(store.tracks().size() == 1);
}

TEST(scan_ignores_iso_files_that_are_not_sacd) {
    TempFolder folder;
    const fs::path music = folder.music();
    write_text(music / "windows.iso", std::string(4 * 1024 * 1024, 'x'));

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    const ScanResult result = scan_folder(store, artwork, music);
    CHECK(result.ignored_images == 1);
    CHECK(result.warnings.empty());
    CHECK(store.tracks().empty());
}

TEST(scan_of_a_missing_folder_leaves_the_catalogue_alone) {
    TempFolder folder;
    const fs::path music = folder.music();
    write_wav(music / "a.wav", 1000);

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    scan_folder(store, artwork, music);
    fs::rename(music, folder.root() / "unplugged");

    bool threw = false;
    try {
        scan_folder(store, artwork, music);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(store.tracks().size() == 1);
}

TEST(cancelled_scan_removes_nothing) {
    TempFolder folder;
    const fs::path music = folder.music();
    write_wav(music / "a.wav", 1000);
    write_wav(music / "b.wav", 1000);

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    scan_folder(store, artwork, music);

    const std::atomic<bool> cancel{true};
    const ScanResult result = scan_folder(store, artwork, music, {}, {}, &cancel);
    CHECK(result.cancelled);
    CHECK(store.tracks().size() == 2);
}

TEST(search_ignores_accents_and_finds_prefixes) {
    TempFolder folder;
    const fs::path music = folder.music();
    write_wav(music / "a.wav", 1000);
    tag_file(music / "a.wav", "Canción del Mar", "Björk", "Homogénic", 1, 1997);
    write_wav(music / "b.wav", 1000);
    tag_file(music / "b.wav", "Other", "AC/DC", "Back in Black", 1, 1980);
    write_wav(music / "c.wav", 1000);
    tag_file(music / "c.wav", "÷", "Someone", "Divide", 1, 2017);

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    scan_folder(store, artwork, music);

    CHECK(store.search("bjork").size() == 1);
    CHECK(store.search("BJÖRK").size() == 1);
    CHECK(store.search("cancion mar").size() == 1);
    CHECK(store.search("homog").size() == 1);
    CHECK(store.search("back black").size() == 1);
    CHECK(store.search("ac/dc").size() == 1);
    CHECK(store.search("÷").size() == 1);
    CHECK(store.search("\"quoted").empty());
    CHECK(store.search("nothing like this").empty());
    CHECK(store.search("").empty());
    CHECK(store.search("   ").empty());
}

TEST(albums_and_artists_group_the_tracks) {
    TempFolder folder;
    const fs::path music = folder.music();
    write_wav(music / "2.wav", 1000);
    tag_file(music / "2.wav", "Second", "Band", "Record", 2, 2000);
    write_wav(music / "1.wav", 1000);
    tag_file(music / "1.wav", "First", "BAND", "record", 1, 2000);
    write_wav(music / "guest.wav", 1000);
    tag_file(music / "guest.wav", "Guest Spot", "Band", "Compilation", 1, 2005, "Various");

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    scan_folder(store, artwork, music);

    const std::vector<Album> albums = store.albums();
    CHECK(albums.size() == 2);
    const auto record = std::find_if(albums.begin(), albums.end(), [](const Album& album) { return album.year == 2000; });
    CHECK(record != albums.end());
    if (record == albums.end()) return;
    CHECK(record->track_count == 2);
    const std::vector<Track> tracks = store.album_tracks(record->id);
    CHECK(tracks.size() == 2);
    CHECK(tracks[0].title == "First");
    CHECK(tracks[1].title == "Second");

    const std::vector<Artist> artists = store.artists();
    CHECK(artists.size() == 2);
    const auto band =
        std::find_if(artists.begin(), artists.end(), [](const Artist& artist) { return artist.track_count == 3; });
    CHECK(band != artists.end());
    if (band == artists.end()) return;
    CHECK(band->album_count == 1);
    CHECK(store.artist_albums(band->id).size() == 2);
    CHECK(store.artist_tracks(band->id).size() == 3);
}

// Real cases: a playlist-like album with each track dated by its original release, and an artist
// with several self-titled albums.
TEST(albums_are_a_title_in_a_folder) {
    TempFolder folder;
    const fs::path music = folder.music();
    fs::create_directories(music / "1984");
    fs::create_directories(music / "1988");
    fs::create_directories(music / "mix");
    write_wav(music / "1984" / "a.wav", 1000);
    tag_file(music / "1984" / "a.wav", "A", "Chayanne", "Chayanne", 1, 1984);
    write_wav(music / "1988" / "b.wav", 1000);
    tag_file(music / "1988" / "b.wav", "B", "Chayanne", "Chayanne", 1, 1988);
    write_wav(music / "mix" / "c.wav", 1000);
    tag_file(music / "mix" / "c.wav", "C", "One", "Sessions", 1, 1971, "Curator");
    write_wav(music / "mix" / "d.wav", 1000);
    tag_file(music / "mix" / "d.wav", "D", "Two", "Sessions", 2, 1983, "Curator");

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    scan_folder(store, artwork, music);

    const std::vector<Album> albums = store.albums();
    CHECK(albums.size() == 3);
    const auto sessions =
        std::find_if(albums.begin(), albums.end(), [](const Album& album) { return album.title == "Sessions"; });
    CHECK(sessions != albums.end());
    if (sessions == albums.end()) return;
    CHECK(sessions->track_count == 2);
    CHECK(sessions->album_artist == "Curator");
    CHECK(sessions->year == 1983);
}

TEST(album_credit_follows_the_majority) {
    TempFolder folder;
    const fs::path music = folder.music();
    fs::create_directories(music / "solo");
    fs::create_directories(music / "various");
    int number = 0;
    for (const char* artist : {"Main", "Main", "Main", "Guest"}) {
        const std::string name = std::to_string(++number) + ".wav";
        write_wav(music / "solo" / name, 1000);
        tag_file(music / "solo" / name, name.c_str(), artist, "Solo", number, 2000);
    }
    for (const char* artist : {"A", "B", "C"}) {
        const std::string name = std::to_string(++number) + ".wav";
        write_wav(music / "various" / name, 1000);
        tag_file(music / "various" / name, name.c_str(), artist, "Mixed", number, 2000);
    }

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    scan_folder(store, artwork, music);

    const std::vector<Album> albums = store.albums();
    CHECK(albums.size() == 2);
    for (const Album& album : albums) CHECK(album.album_artist == (album.title == "Solo" ? "Main" : kVariousArtists));
    for (const Track& track : store.album_tracks(albums[0].id)) CHECK(track.album_artist == albums[0].album_artist);
}

TEST(disc_folders_join_their_album) {
    TempFolder folder;
    const fs::path music = folder.music();
    fs::create_directories(music / "Box Set" / "CD1");
    fs::create_directories(music / "Box Set" / "Disc 2");
    fs::create_directories(music / "Disc 07 - Soundtrack");
    write_wav(music / "Box Set" / "CD1" / "a.wav", 1000);
    write_wav(music / "Box Set" / "Disc 2" / "a.wav", 1000);
    write_wav(music / "Disc 07 - Soundtrack" / "a.wav", 1000);
    write_bytes(music / "Box Set" / "cover.bmp", make_bmp(4, 4));

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    scan_folder(store, artwork, music);

    const std::vector<Album> albums = store.albums();
    CHECK(albums.size() == 2);
    const auto box = std::find_if(albums.begin(), albums.end(), [](const Album& album) { return album.title == "Box Set"; });
    CHECK(box != albums.end());
    if (box == albums.end()) return;
    CHECK(box->track_count == 2);
    CHECK(box->artwork_id != 0);
    const std::vector<Track> tracks = store.album_tracks(box->id);
    CHECK(tracks.size() == 2);
    CHECK(tracks[0].disc_number == 1);
    CHECK(tracks[1].disc_number == 2);
}

TEST(favorites_playlists_and_queue) {
    TempFolder folder;
    const fs::path music = folder.music();
    for (const char* name : {"a.wav", "b.wav", "c.wav"}) write_wav(music / name, 1000);

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    scan_folder(store, artwork, music);
    const std::vector<Track> tracks = store.tracks();
    CHECK(tracks.size() == 3);
    if (tracks.size() != 3) return;

    store.set_favorite(tracks[1].id, true);
    store.set_favorite(tracks[1].id, true);
    CHECK(store.favorites().size() == 1);
    CHECK(store.track(tracks[1].id)->favorite);
    store.set_favorite(tracks[1].id, false);
    CHECK(store.favorites().empty());

    const PlaylistId playlist = store.create_playlist("Mezcla");
    for (const Track& track : tracks) store.add_to_playlist(playlist, track.id);
    store.add_to_playlist(playlist, tracks[0].id);  // a playlist may repeat a track
    CHECK(store.playlists().size() == 1);
    CHECK(store.playlists()[0].track_count == 4);

    // Indices follow what the caller sees, also after earlier removals left gaps.
    store.remove_from_playlist(playlist, 1);
    store.remove_from_playlist(playlist, 1);
    const std::vector<Track> left = store.playlist_tracks(playlist);
    CHECK(left.size() == 2);
    CHECK(left[0].id == tracks[0].id);
    CHECK(left[1].id == tracks[0].id);

    store.set_queue({tracks[2].id, tracks[0].id});
    CHECK(store.queue().size() == 2);
    CHECK(store.queue()[0].id == tracks[2].id);
    store.set_queue({tracks[1].id});
    CHECK(store.queue().size() == 1);

    // Deleting a track from disk takes it out of everything that referred to it.
    store.set_favorite(tracks[0].id, true);
    fs::remove(tracks[0].path);
    scan_folder(store, artwork, music);
    CHECK(store.playlist_tracks(playlist).empty());
    CHECK(store.favorites().empty());

    store.delete_playlist(playlist);
    CHECK(store.playlists().empty());
}

TEST(removing_a_folder_clears_everything_under_it) {
    TempFolder folder;
    const fs::path music = folder.music();
    write_wav(music / "album.wav", kCueAlbumFrames);
    write_text(music / "album.cue", kCueAlbum);
    write_bytes(music / "cover.bmp", make_bmp(4, 4));

    LibraryStore store(folder.database());
    ArtworkCache artwork(folder.cache());
    scan_folder(store, artwork, music);
    CHECK(store.tracks().size() == 2);

    store.remove_folder(fs::weakly_canonical(music));
    CHECK(store.folders().empty());
    CHECK(store.tracks().empty());
    CHECK(store.albums().empty());
    CHECK(store.artists().empty());
    CHECK(store.search("Cue").empty());
    CHECK(search_rows(store) == 0);  // these rows go through cascades, not through remove_unseen
}

TEST(watcher_reports_a_burst_once) {
    TempFolder folder;
    std::atomic<int> calls{0};
    {
        FolderWatcher watcher(folder.music(), [&calls] { ++calls; }, 150);
        Sleep(50);
        for (int i = 0; i < 5; ++i) {
            write_text(folder.music() / ("file" + std::to_string(i) + ".txt"), "x");
            Sleep(20);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (calls == 0 && std::chrono::steady_clock::now() < deadline) Sleep(10);
        Sleep(300);
    }
    CHECK(calls == 1);
}

TEST(library_follows_a_watched_folder) {
    TempFolder folder;
    const fs::path music = folder.music();
    write_wav(music / "first.wav", 1000);

    const auto wait_for_tracks = [](Library& library, size_t count) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (library.store().tracks().size() == count && library.wait_until_idle(100)) return true;
            Sleep(20);
        }
        return false;
    };

    {
        Library library(folder.database(), folder.cache(), {}, 100);
        library.add_folder(music);
        CHECK(wait_for_tracks(library, 1));
        CHECK(library.status().revision > 0);
        CHECK(library.status().error.empty());

        bool threw = false;
        try {
            library.add_folder(music / "..\\music");
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);

        fs::create_directories(music / "inner");
        threw = false;
        try {
            library.add_folder(music / "inner");
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);

        write_wav(music / "second.wav", 1000);
        CHECK(wait_for_tracks(library, 2));
    }

    // Changes made while closed are picked up at the next start.
    write_wav(music / "third.wav", 1000);
    {
        Library library(folder.database(), folder.cache(), {}, 100);
        CHECK(wait_for_tracks(library, 3));
        library.remove_folder(music);
        CHECK(library.wait_until_idle(5000));
        CHECK(library.store().folders().empty());
        CHECK(library.store().tracks().empty());
    }
}
