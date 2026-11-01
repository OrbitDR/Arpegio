// Tags and embedded pictures through TagLib. Sample formats come from the decoders, not from here.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arpegio {

// What the ReplayGain tags say, empty where they say nothing. Peaks are linear, with 1 at full scale.
struct ReplayGainTags {
    std::optional<double> track_gain_db;
    std::optional<double> track_peak;
    std::optional<double> album_gain_db;
    std::optional<double> album_peak;

    bool operator==(const ReplayGainTags&) const = default;
};

// Tag values such as "-6.54 dB" and "0.988", in the same form in files and in CUE sheets. Empty when the
// value makes no sense, because a wrong gain is worse than none.
std::optional<double> parse_replay_gain(std::string_view text);
std::optional<double> parse_replay_peak(std::string_view text);

struct Metadata {
    std::string title;
    std::string artist;
    std::string album;
    std::string album_artist;
    std::string composer;
    std::string genre;
    std::string comment;
    std::string codec;
    std::string cue_sheet;  // embedded in the tags, empty when there is none
    int track_number = 0;
    int disc_number = 0;
    int year = 0;
    int bitrate = 0;  // kbit/s
    bool compilation = false;
    ReplayGainTags replay_gain;
    std::vector<uint8_t> artwork;
};

// Opens the file read-only. A file TagLib cannot parse returns empty tags: being unreadable by TagLib
// says nothing about whether the audio plays.
Metadata read_metadata(const std::filesystem::path& path);

// Cover for the audio in a folder: an image with a usual name, else one that is alone there, in the
// folder itself or in a subfolder for scans. Empty when there is none.
std::vector<uint8_t> read_folder_artwork(const std::filesystem::path& folder);

}  // namespace arpegio
