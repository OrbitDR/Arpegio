// Brings the catalogue up to date with one folder. Unchanged files are not read again. Files are read
// outside any transaction and written in short batches, so browsing stays responsive during a scan.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "library/artwork.h"
#include "library/library_store.h"

namespace arpegio {

struct ScanOptions {
    bool force = false;  // read every file, changed or not
    bool extract_artwork = true;
    bool sacd_multichannel = false;  // catalogue the multichannel area of discs that also have stereo
};

struct ScanResult {
    uint64_t files = 0;      // audio files, CUE sheets and disc images found
    uint64_t unchanged = 0;  // of those, skipped because nothing changed
    uint64_t tracks_written = 0;
    uint64_t tracks_removed = 0;
    uint64_t cue_sheets = 0;        // sheets that were read and used
    uint64_t duplicate_sheets = 0;  // second sheets for audio another one already covers
    uint64_t ignored_images = 0;    // .iso files that are not SACD
    bool cancelled = false;         // cancelled scans keep what they wrote and remove nothing
    std::vector<std::string> warnings;
};

using ScanProgress = std::function<void(uint64_t done, uint64_t total, const std::string& path)>;

// Throws std::runtime_error when the folder cannot be listed, leaving the catalogue as it was, so a
// disconnected drive never looks like a library that was deleted.
ScanResult scan_folder(LibraryStore& store, ArtworkCache& artwork, const std::filesystem::path& folder,
                       const ScanOptions& options = {}, const ScanProgress& progress = {},
                       const std::atomic<bool>* cancel = nullptr);

}  // namespace arpegio
