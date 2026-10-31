// Content-addressed covers on disk: the original bytes plus a 512 px JPEG thumbnail. Images go through
// Windows Imaging Component, so the calling thread must have COM initialized.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace arpegio {

struct ArtworkFile {
    std::string hash;
    std::string file;  // name of the original under <root>/artwork
    std::string mime;
    int width = 0;
    int height = 0;
};

class ArtworkCache {
public:
    // Creates the folders. Throws std::filesystem::filesystem_error when it cannot.
    explicit ArtworkCache(const std::filesystem::path& root);

    // Empty for pictures that do not decode: a bad cover must not reject its track.
    std::optional<ArtworkFile> store(const std::vector<uint8_t>& image);

    std::filesystem::path original_path(const ArtworkFile& artwork) const;
    // The original when no thumbnail could be written.
    std::filesystem::path thumbnail_path(const ArtworkFile& artwork) const;

private:
    std::filesystem::path root_;
    std::unordered_map<std::string, std::optional<ArtworkFile>> seen_;
};

}  // namespace arpegio
