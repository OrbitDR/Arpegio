// Tags and embedded pictures through TagLib. Sample formats come from the decoders, not from here.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace arpegio {

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
    std::vector<uint8_t> artwork;
};

// Opens the file read-only. A file TagLib cannot parse returns empty tags: being unreadable by TagLib
// says nothing about whether the audio plays.
Metadata read_metadata(const std::filesystem::path& path);

// Cover for the audio in a folder: an image with a usual name, else one that is alone there, in the
// folder itself or in a subfolder for scans. Empty when there is none.
std::vector<uint8_t> read_folder_artwork(const std::filesystem::path& folder);

}  // namespace arpegio
