// CUE sheets. Times stay in CD frames, 75 per second, so each caller converts them to its own units.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace arpegio {

struct CueTrack {
    int number = 0;
    std::string file;
    size_t file_index = 0;
    std::string title;
    std::string performer;
    std::string songwriter;
    uint64_t start = 0;  // INDEX 01, within `file`
    uint64_t end = 0;    // INDEX 01 of the next track in the same file, 0 for the end of the file
    bool pre_emphasis = false;
    std::map<std::string, std::string> rem;  // the REM lines of this track, keys in upper case
};

struct CueSheet {
    std::string title;
    std::string performer;
    std::map<std::string, std::string> rem;  // all the REM lines, keys in upper case
    std::vector<CueTrack> tracks;            // audio tracks only
};

// Both throw std::runtime_error when the sheet cannot be used.
CueSheet parse_cue(std::string_view text);
CueSheet parse_cue_file(const std::filesystem::path& path);

}  // namespace arpegio
