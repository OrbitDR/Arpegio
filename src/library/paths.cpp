#include "library/paths.h"

#include <windows.h>

namespace arpegio {

std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path path_from_utf8(std::string_view text) {
    return std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size());
}

std::wstring path_key(const std::filesystem::path& path) {
    std::wstring key = path.generic_wstring();
    CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
    return key;
}

}  // namespace arpegio
