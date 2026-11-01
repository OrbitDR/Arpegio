#include "library/cue_sheet.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace arpegio {

namespace {

constexpr int kFramesPerSecond = 75;
constexpr int kMaxTrackNumber = 99;
constexpr unsigned kMaxFrames = 99;  // above 74 is out of spec, but real sheets have it; see parse_time

std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

std::vector<std::string> tokenize(std::string_view line) {
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false;
    bool started = false;
    for (const char c : line) {
        if (c == '"') {
            quoted = !quoted;
            started = true;
        } else if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
            if (started) tokens.push_back(std::move(current));
            current.clear();
            started = false;
        } else {
            current.push_back(c);
            started = true;
        }
    }
    if (started) tokens.push_back(std::move(current));
    return tokens;
}

std::string join(const std::vector<std::string>& tokens, size_t first) {
    std::string out;
    for (size_t i = first; i < tokens.size(); ++i) {
        if (i > first) out.push_back(' ');
        out += tokens[i];
    }
    return out;
}

std::optional<uint64_t> parse_time(const std::string& text) {
    unsigned minutes = 0;
    unsigned seconds = 0;
    unsigned frames = 0;
    if (std::sscanf(text.c_str(), "%u:%u:%u", &minutes, &seconds, &frames) != 3) return std::nullopt;
    // Some tools write 03:22:90. The boundary moves a fraction of a second, while refusing the sheet
    // would leave the whole album as one track.
    if (seconds >= 60 || frames > kMaxFrames) return std::nullopt;
    return (uint64_t{minutes} * 60 + seconds) * kFramesPerSecond + frames;
}

std::string wide_to_utf8(const std::wstring& wide) {
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string decode_text(const std::string& bytes) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const int size = static_cast<int>(bytes.size());
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) return bytes.substr(3);
    if (size >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF))) {
        std::wstring wide((size - 2) / 2, L'\0');
        std::memcpy(wide.data(), data + 2, wide.size() * sizeof(wchar_t));
        if (data[0] == 0xFE) {
            for (wchar_t& c : wide) c = static_cast<wchar_t>(c << 8 | c >> 8);
        }
        return wide_to_utf8(wide);
    }

    // Without a mark, valid UTF-8 stays as it is. Older rippers wrote the Windows code page.
    if (bytes.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), size, nullptr, 0) > 0) {
        return bytes;
    }
    std::wstring wide(MultiByteToWideChar(CP_ACP, 0, bytes.data(), size, nullptr, 0), L'\0');
    MultiByteToWideChar(CP_ACP, 0, bytes.data(), size, wide.data(), static_cast<int>(wide.size()));
    return wide_to_utf8(wide);
}

struct ParsedTrack {
    CueTrack track;
    bool audio = true;
    bool has_start = false;
};

}  // namespace

CueSheet parse_cue(std::string_view text) {
    CueSheet sheet;
    std::vector<ParsedTrack> parsed;
    std::string file;
    size_t file_index = 0;
    bool have_file = false;
    int line_number = 0;

    const auto fail = [&line_number](const std::string& message) {
        return std::runtime_error("line " + std::to_string(line_number) + ": " + message);
    };

    std::istringstream input{std::string(text)};
    std::string line;
    while (std::getline(input, line)) {
        ++line_number;
        const std::vector<std::string> tokens = tokenize(line);
        if (tokens.empty()) continue;
        const std::string command = upper(tokens[0]);
        ParsedTrack* current = parsed.empty() ? nullptr : &parsed.back();

        if (command == "REM") {
            if (tokens.size() >= 2) {
                sheet.rem[upper(tokens[1])] = join(tokens, 2);
                if (current) current->track.rem[upper(tokens[1])] = join(tokens, 2);
            }
        } else if (command == "FILE") {
            if (tokens.size() < 2) throw fail("FILE without a name");
            if (have_file) ++file_index;
            have_file = true;
            file = tokens[1];
        } else if (command == "TRACK") {
            if (tokens.size() < 2) throw fail("TRACK without a number");
            if (!have_file) throw fail("TRACK before FILE");
            ParsedTrack track;
            track.track.number = std::atoi(tokens[1].c_str());
            if (track.track.number <= 0 || track.track.number > kMaxTrackNumber) throw fail("TRACK number out of range");
            track.track.file = file;
            track.track.file_index = file_index;
            track.audio = tokens.size() < 3 || upper(tokens[2]) == "AUDIO";
            parsed.push_back(std::move(track));
        } else if (command == "TITLE" || command == "PERFORMER" || command == "SONGWRITER") {
            const std::string value = join(tokens, 1);
            if (current) {
                if (command == "TITLE") current->track.title = value;
                else if (command == "PERFORMER") current->track.performer = value;
                else current->track.songwriter = value;
            } else if (command == "TITLE") {
                sheet.title = value;
            } else if (command == "PERFORMER") {
                sheet.performer = value;
            }
        } else if (command == "INDEX") {
            if (!current) throw fail("INDEX before TRACK");
            if (tokens.size() < 3) throw fail("INDEX without a time");
            const std::optional<uint64_t> time = parse_time(tokens[2]);
            if (!time) throw fail("invalid INDEX time");
            if (std::atoi(tokens[1].c_str()) == 1) {
                // Rippers that keep gaps in the previous file put a FILE line between INDEX 00 and 01.
                current->track.file = file;
                current->track.file_index = file_index;
                current->track.start = *time;
                current->has_start = true;
            }
        } else if (command == "FLAGS") {
            if (current) {
                for (size_t i = 1; i < tokens.size(); ++i) {
                    if (upper(tokens[i]) == "PRE") current->track.pre_emphasis = true;
                }
            }
        }
        // PREGAP and POSTGAP are silence that is not in the file, and the rest does not affect playback.
    }

    if (parsed.empty()) throw std::runtime_error("the sheet has no tracks");
    for (const ParsedTrack& track : parsed) {
        if (!track.has_start) throw std::runtime_error("track " + std::to_string(track.track.number) + " has no INDEX 01");
    }
    for (size_t i = 0; i + 1 < parsed.size(); ++i) {
        CueTrack& track = parsed[i].track;
        const CueTrack& next = parsed[i + 1].track;
        if (next.file_index != track.file_index) continue;
        if (next.start < track.start) throw std::runtime_error("tracks are not in ascending order");
        track.end = next.start;
    }
    for (ParsedTrack& track : parsed) {
        if (track.audio) sheet.tracks.push_back(std::move(track.track));
    }
    if (sheet.tracks.empty()) throw std::runtime_error("the sheet has no audio tracks");
    return sheet;
}

CueSheet parse_cue_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open the file");
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return parse_cue(decode_text(bytes));
}

}  // namespace arpegio
