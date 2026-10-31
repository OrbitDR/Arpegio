// Paths as UTF-8 text: the database and the tags are UTF-8, Windows paths are UTF-16.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace arpegio {

std::string path_to_utf8(const std::filesystem::path& path);
std::filesystem::path path_from_utf8(std::string_view text);

// Windows compares names without case, so anything that matches paths needs a folded key.
std::wstring path_key(const std::filesystem::path& path);

}  // namespace arpegio
