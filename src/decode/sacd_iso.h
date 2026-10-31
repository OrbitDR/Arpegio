// SACD disc images: table of contents, text and track decoding.
// The layout follows the structures in SACD Ripper, GPL 2 or later, and foo_input_sacd.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "decode/decoder.h"

namespace arpegio {

struct SacdTrack {
    std::string title;
    std::string performer;
    std::string composer;
    uint32_t first_lsn = 0;
    uint32_t length_lsn = 0;
    uint32_t start_frame = 0;      // 75 per second, from the start of the area
    uint32_t duration_frames = 0;
};

struct SacdArea {
    bool stereo = false;
    bool dst = false;
    int channels = 0;
    uint32_t rate = 0;
    uint32_t first_lsn = 0;
    uint32_t last_lsn = 0;
    std::vector<SacdTrack> tracks;
};

struct SacdDisc {
    std::string album_title;
    std::string album_artist;
    std::string disc_title;
    std::string disc_artist;
    int year = 0;
    int disc_number = 0;
    int disc_total = 0;
    std::vector<SacdArea> areas;
};

// Many .iso files are not SACD images, so this checks for the disc signature.
bool is_sacd_image(const std::filesystem::path& path);

// Throws std::runtime_error when the table of contents cannot be read.
SacdDisc read_sacd_disc(const std::filesystem::path& path);

std::unique_ptr<Decoder> open_sacd_track(const std::filesystem::path& path, size_t area, size_t track);

}  // namespace arpegio
