#include "library/metadata.h"

#include <windows.h>

#include <fileref.h>
#include <mp4properties.h>
#include <tag.h>
#include <tfilestream.h>
#include <tpropertymap.h>
#include <tvariant.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

#include "library/paths.h"

namespace arpegio {

namespace {

constexpr const wchar_t* kCoverNames[] = {L"cover", L"folder",   L"front",   L"frontal",
                                          L"album", L"albumart", L"artwork", L"portada"};
constexpr const wchar_t* kCoverExtensions[] = {L".jpg", L".jpeg", L".png", L".bmp", L".webp"};
constexpr const wchar_t* kScanFolders[] = {L"covers", L"cover", L"artwork", L"art", L"scans", L"scan"};

constexpr double kMaxReplayGainDb = 51.0;  // what the format can store
constexpr double kMaxReplayPeak = 16.0;    // inter-sample and encoder overshoots stay well below this

// Decimal commas come from taggers running under a Spanish or German locale.
std::optional<double> leading_number(std::string_view text) {
    std::string copy(text);
    std::replace(copy.begin(), copy.end(), ',', '.');
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str() || !std::isfinite(value)) return std::nullopt;
    return value;
}

// Tags carry stray spaces often enough to split artists and albums that are really the same.
std::string trimmed(std::string text) {
    constexpr const char* kSpace = " \t\r\n";
    const size_t first = text.find_first_not_of(kSpace);
    if (first == std::string::npos) return {};
    text.erase(text.find_last_not_of(kSpace) + 1);
    text.erase(0, first);
    return text;
}

std::string utf8(const TagLib::String& text) {
    return trimmed(text.to8Bit(true));
}

std::wstring lowered(std::wstring text) {
    CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<std::filesystem::path> list_images(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> images;
    std::error_code code;
    for (std::filesystem::directory_iterator it(folder, code), end; !code && it != end; it.increment(code)) {
        std::error_code entry_code;
        if (!it->is_regular_file(entry_code)) continue;
        const std::wstring extension = lowered(it->path().extension().wstring());
        const bool image = std::any_of(std::begin(kCoverExtensions), std::end(kCoverExtensions),
                                       [&extension](const wchar_t* known) { return extension == known; });
        if (image) images.push_back(it->path());
    }
    std::sort(images.begin(), images.end());
    return images;
}

std::vector<uint8_t> read_cover_in(const std::filesystem::path& folder) {
    const std::vector<std::filesystem::path> images = list_images(folder);
    for (const wchar_t* name : kCoverNames) {
        for (const std::filesystem::path& image : images) {
            if (lowered(image.stem().wstring()) != name) continue;
            std::vector<uint8_t> bytes = read_file(image);
            if (!bytes.empty()) return bytes;
        }
    }
    return images.size() == 1 ? read_file(images.front()) : std::vector<uint8_t>{};
}

std::string first_value(const TagLib::PropertyMap& properties, const char* key) {
    const auto found = properties.find(key);
    return found == properties.end() || found->second.isEmpty() ? std::string() : utf8(found->second.front());
}

// Also takes the leading number of "2012-03-01" and "1/2".
int first_number(const TagLib::PropertyMap& properties, const char* key) {
    return std::atoi(first_value(properties, key).c_str());
}

std::string codec_name(const std::filesystem::path& path, const TagLib::AudioProperties* audio) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    if (const auto* mp4 = dynamic_cast<const TagLib::MP4::Properties*>(audio)) {
        return mp4->codec() == TagLib::MP4::Properties::ALAC ? "ALAC" : "AAC";
    }
    if (extension == L".flac") return "FLAC";
    if (extension == L".wav" || extension == L".wave" || extension == L".rf64") return "WAV";
    if (extension == L".aif" || extension == L".aiff" || extension == L".aifc") return "AIFF";
    if (extension == L".ape") return "APE";
    if (extension == L".wv") return "WavPack";
    if (extension == L".mp3") return "MP3";
    if (extension == L".ogg" || extension == L".oga") return "Vorbis";
    if (extension == L".wma") return "WMA";
    if (extension == L".dsf") return "DSF";
    if (extension == L".dff") return "DFF";
    if (extension == L".m4a" || extension == L".mp4" || extension == L".aac") return "AAC";
    return {};
}

void read_picture(TagLib::FileRef& file, Metadata& metadata) {
    const auto pictures = file.complexProperties("PICTURE");
    if (pictures.isEmpty()) return;
    auto chosen = pictures.begin();
    for (auto picture = pictures.begin(); picture != pictures.end(); ++picture) {
        if (picture->value("pictureType").toString() == "Front Cover") {
            chosen = picture;
            break;
        }
    }
    const TagLib::ByteVector data = chosen->value("data").toByteVector();
    metadata.artwork.assign(data.begin(), data.end());
}

ReplayGainTags read_replay_gain(const TagLib::PropertyMap& properties) {
    return {parse_replay_gain(first_value(properties, "REPLAYGAIN_TRACK_GAIN")),
            parse_replay_peak(first_value(properties, "REPLAYGAIN_TRACK_PEAK")),
            parse_replay_gain(first_value(properties, "REPLAYGAIN_ALBUM_GAIN")),
            parse_replay_peak(first_value(properties, "REPLAYGAIN_ALBUM_PEAK"))};
}

}  // namespace

std::optional<double> parse_replay_gain(std::string_view text) {
    const std::optional<double> gain = leading_number(text);
    return gain && std::abs(*gain) <= kMaxReplayGainDb ? gain : std::nullopt;
}

std::optional<double> parse_replay_peak(std::string_view text) {
    // Some taggers write 0 for a peak they never measured.
    const std::optional<double> peak = leading_number(text);
    return peak && *peak > 0.0 && *peak <= kMaxReplayPeak ? peak : std::nullopt;
}

Metadata read_metadata(const std::filesystem::path& path) {
    Metadata metadata;
    metadata.codec = codec_name(path, nullptr);

    TagLib::FileStream stream(path.c_str(), true);
    if (!stream.isOpen()) return metadata;
    TagLib::FileRef file(&stream, true, TagLib::AudioProperties::Average);
    if (file.isNull()) return metadata;

    if (const TagLib::Tag* tag = file.tag()) {
        metadata.title = utf8(tag->title());
        metadata.artist = utf8(tag->artist());
        metadata.album = utf8(tag->album());
        metadata.genre = utf8(tag->genre());
        metadata.comment = utf8(tag->comment());
        metadata.track_number = static_cast<int>(tag->track());
        metadata.year = static_cast<int>(tag->year());
    }

    const TagLib::PropertyMap properties = file.file()->properties();
    metadata.album_artist = first_value(properties, "ALBUMARTIST");
    metadata.composer = first_value(properties, "COMPOSER");
    metadata.cue_sheet = first_value(properties, "CUESHEET");
    metadata.disc_number = first_number(properties, "DISCNUMBER");
    metadata.compilation = first_number(properties, "COMPILATION") != 0;
    metadata.replay_gain = read_replay_gain(properties);
    if (metadata.track_number == 0) metadata.track_number = first_number(properties, "TRACKNUMBER");
    if (metadata.year == 0) metadata.year = first_number(properties, "DATE");

    if (const TagLib::AudioProperties* audio = file.audioProperties()) {
        metadata.bitrate = std::max(0, audio->bitrate());
        metadata.codec = codec_name(path, audio);
    }
    read_picture(file, metadata);
    return metadata;
}

std::vector<uint8_t> read_folder_artwork(const std::filesystem::path& folder) {
    std::vector<uint8_t> cover = read_cover_in(folder);
    for (const wchar_t* name : kScanFolders) {
        if (!cover.empty()) break;
        cover = read_cover_in(folder / name);
    }
    return cover;
}

}  // namespace arpegio
