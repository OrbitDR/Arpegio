#include "library/scanner.h"

#include <windows.h>

#include <objbase.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "decode/decoder.h"
#include "decode/sacd_iso.h"
#include "library/cue_sheet.h"
#include "library/metadata.h"
#include "library/mqa.h"
#include "library/paths.h"

namespace arpegio {

namespace {

constexpr size_t kBatchTracks = 128;
constexpr int kCueFramesPerSecond = 75;
constexpr const wchar_t* kDiscPrefixes[] = {L"disco", L"disc", L"disk", L"cd"};

// Formats the decoders handle. SACD images and CUE sheets are told apart separately.
constexpr const wchar_t* kAudioExtensions[] = {L".flac", L".wav", L".wave", L".rf64", L".aif", L".aiff",
                                               L".aifc", L".ape", L".wv",   L".mp3",  L".ogg", L".oga",
                                               L".m4a",  L".mp4", L".aac",  L".wma",  L".dsf", L".dff"};

struct ComScope {
    ComScope() : result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(result)) CoUninitialize();
    }
    HRESULT result;
};

struct FoundFile {
    std::filesystem::path path;
    FileStamp stamp;
};

struct FileRead {
    SourceFormat format;
    bool mqa = false;
    Metadata metadata;
    std::optional<ArtworkFile> artwork;
};

struct PendingTrack {
    Track track;
    std::string cue_path;
    FileStamp cue_stamp;
    std::optional<ArtworkFile> artwork;
};

std::wstring lower_extension(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    CharLowerBuffW(extension.data(), static_cast<DWORD>(extension.size()));
    return extension;
}

bool is_audio_extension(const std::wstring& extension) {
    return std::any_of(std::begin(kAudioExtensions), std::end(kAudioExtensions),
                       [&extension](const wchar_t* known) { return extension == known; });
}

bool is_hidden_directory(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
}

// The number in a folder called like "CD1" or "Disc 2", 0 for any other name.
int disc_folder_number(const std::filesystem::path& folder) {
    std::wstring name = folder.filename().wstring();
    CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
    for (const wchar_t* prefix : kDiscPrefixes) {
        if (!name.starts_with(prefix)) continue;
        size_t i = std::wcslen(prefix);
        while (i < name.size() && (name[i] == L' ' || name[i] == L'-' || name[i] == L'_' || name[i] == L'.')) ++i;
        const bool digits =
            i < name.size() && std::all_of(name.begin() + i, name.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
        return digits ? _wtoi(name.c_str() + i) : 0;
    }
    return 0;
}

std::string first_nonempty(std::initializer_list<const std::string*> values) {
    for (const std::string* value : values) {
        if (!value->empty()) return *value;
    }
    return {};
}

std::string rem_value(const CueSheet& sheet, const char* key) {
    const auto found = sheet.rem.find(key);
    return found == sheet.rem.end() ? std::string() : found->second;
}

class FolderScan {
public:
    FolderScan(LibraryStore& store, ArtworkCache& artwork, const std::filesystem::path& root, const ScanOptions& options,
               const ScanProgress& progress, const std::atomic<bool>* cancel)
        : store_(store), artwork_(artwork), root_(root), options_(options), progress_(progress), cancel_(cancel) {}

    ScanResult run() {
        list_files();
        folder_ = store_.add_folder(root_);
        known_files_ = store_.known_files(folder_);
        known_cues_ = store_.known_cues(folder_);
        total_ = cues_.size() + audio_.size() + images_.size();
        result_.files = total_;

        // Sheets go first because they decide which audio files are not tracks of their own.
        for (const FoundFile& cue : cues_) {
            if (next(cue.path)) scan_cue(cue);
        }
        for (const FoundFile& file : audio_) {
            if (next(file.path)) scan_audio(file);
        }
        for (const FoundFile& image : images_) {
            if (next(image.path)) scan_image(image);
        }
        flush();

        result_.cancelled = cancelled();
        if (!result_.cancelled) finish();
        return std::move(result_);
    }

private:
    bool cancelled() const { return cancel_ && cancel_->load(std::memory_order_relaxed); }

    bool next(const std::filesystem::path& path) {
        if (cancelled()) return false;
        ++done_;
        if (progress_) progress_(done_, total_, path_to_utf8(path));
        return true;
    }

    void warn(const std::filesystem::path& path, const std::string& text) {
        result_.warnings.push_back(relative(path) + ": " + text);
    }

    std::string relative(const std::filesystem::path& path) const {
        const std::u8string text = path.lexically_relative(root_).generic_u8string();
        return {reinterpret_cast<const char*>(text.data()), text.size()};
    }

    // The folder an album lives in. The discs of a set in subfolders belong to their parent.
    std::filesystem::path album_folder(const std::filesystem::path& file) const {
        const std::filesystem::path folder = file.parent_path();
        return folder != root_ && disc_folder_number(folder) ? folder.parent_path() : folder;
    }

    void list_files() {
        std::error_code code;
        std::filesystem::recursive_directory_iterator it(root_, std::filesystem::directory_options::skip_permission_denied,
                                                         code);
        for (const std::filesystem::recursive_directory_iterator end; !code && it != end; it.increment(code)) {
            const std::filesystem::directory_entry& entry = *it;
            std::error_code entry_code;
            if (entry.is_directory(entry_code)) {
                // Keeps the recycle bin and similar out when a whole drive is added.
                if (is_hidden_directory(entry.path())) it.disable_recursion_pending();
                continue;
            }
            if (!entry.is_regular_file(entry_code)) continue;

            const std::wstring extension = lower_extension(entry.path());
            const bool audio = is_audio_extension(extension);
            const bool cue = extension == L".cue";
            const bool image = extension == L".iso";
            if (!audio && !cue && !image) continue;

            FoundFile file{entry.path(),
                           {entry.file_size(entry_code), entry.last_write_time(entry_code).time_since_epoch().count()}};
            if (entry_code) continue;
            (audio ? audio_ : cue ? cues_ : images_).push_back(std::move(file));
        }
        if (code) throw std::runtime_error("cannot read the folder: " + code.message());

        const auto by_path = [](const FoundFile& a, const FoundFile& b) { return a.path < b.path; };
        std::sort(cues_.begin(), cues_.end(), by_path);
        std::sort(audio_.begin(), audio_.end(), by_path);
        std::sort(images_.begin(), images_.end(), by_path);
        for (const FoundFile& file : audio_) audio_keys_.emplace(path_key(file.path), &file);
    }

    const FoundFile* find_audio(const std::filesystem::path& path) const {
        const auto found = audio_keys_.find(path_key(path));
        return found == audio_keys_.end() ? nullptr : found->second;
    }

    const FoundFile* find_with_any_extension(std::filesystem::path path) const {
        for (const wchar_t* extension : kAudioExtensions) {
            path.replace_extension(extension);
            if (const FoundFile* file = find_audio(path)) return file;
        }
        return nullptr;
    }

    const FoundFile* find_cue_audio(const FoundFile& cue, const std::string& name, bool only_file) const {
        const std::filesystem::path named = (cue.path.parent_path() / path_from_utf8(name)).lexically_normal();
        if (const FoundFile* file = find_audio(named)) return file;
        // Sheets often keep the name of the .wav after the rip was compressed, or of the ripper's
        // temporary file. With a single FILE the sheet's own name settles it.
        if (const FoundFile* file = find_with_any_extension(named)) return file;
        return only_file ? find_with_any_extension(cue.path) : nullptr;
    }

    std::optional<FileRead> read_file(const FoundFile& file) {
        FileRead read;
        try {
            const std::unique_ptr<Decoder> decoder = open_decoder(file.path);
            read.format = decoder->format();
            read.mqa = has_mqa_sync(*decoder);
        } catch (const std::exception& error) {
            warn(file.path, error.what());
            return std::nullopt;
        }
        if (read.format.length == 0 || read.format.rate == 0) {
            warn(file.path, "no audio");
            return std::nullopt;
        }
        read.metadata = read_metadata(file.path);
        read.artwork = artwork_for(file.path, read.metadata);
        read.metadata.artwork = {};
        return read;
    }

    std::optional<ArtworkFile> artwork_for(const std::filesystem::path& path, const Metadata& metadata) {
        if (!options_.extract_artwork) return std::nullopt;
        if (std::optional<ArtworkFile> embedded = artwork_.store(metadata.artwork)) return embedded;
        const std::filesystem::path folder = path.parent_path();
        const std::wstring key = path_key(folder);
        auto found = folder_artwork_.find(key);
        if (found == folder_artwork_.end()) {
            std::vector<uint8_t> cover = read_folder_artwork(folder);
            if (cover.empty()) cover = read_folder_artwork(album_folder(path));
            found = folder_artwork_.emplace(key, artwork_.store(cover)).first;
        }
        return found->second;
    }

    Track make_track(const FoundFile& file, const FileRead& read) const {
        const Metadata& metadata = read.metadata;
        Track track;
        track.relative_path = relative(file.path);
        track.stamp = file.stamp;
        track.format = read.format;
        track.mqa = read.mqa;
        const std::filesystem::path folder = album_folder(file.path);
        track.album_folder = relative(folder);
        track.title = metadata.title.empty() ? path_to_utf8(file.path.stem()) : metadata.title;
        track.artist = metadata.artist;
        track.album = metadata.album.empty() ? path_to_utf8(folder.filename()) : metadata.album;
        track.album_artist = !metadata.album_artist.empty() ? metadata.album_artist
                             : metadata.compilation         ? kVariousArtists
                                                            : metadata.artist;
        track.composer = metadata.composer;
        track.genre = metadata.genre;
        track.comment = metadata.comment;
        track.codec = metadata.codec;
        track.track_number = metadata.track_number;
        track.disc_number = metadata.disc_number ? metadata.disc_number : disc_folder_number(file.path.parent_path());
        track.year = metadata.year;
        track.bitrate = metadata.bitrate;
        track.replay_gain = metadata.replay_gain;
        return track;
    }

    void add(Track track, const std::optional<ArtworkFile>& artwork, const std::string& cue_path = {},
             const FileStamp& cue_stamp = {}) {
        seen_.emplace(track.relative_path, track.part);
        pending_.push_back({std::move(track), cue_path, cue_stamp, artwork});
        if (pending_.size() >= kBatchTracks) flush();
    }

    void keep(const std::string& relative_path) {
        for (const int part : known_files_.at(relative_path).parts) seen_.emplace(relative_path, part);
    }

    void flush() {
        if (pending_.empty()) return;
        Transaction transaction(store_.database());
        for (const PendingTrack& pending : pending_) {
            const CueId cue = pending.cue_path.empty() ? 0 : store_.upsert_cue(folder_, pending.cue_path, pending.cue_stamp);
            store_.upsert_track(folder_, pending.track, cue, pending.artwork ? &*pending.artwork : nullptr);
        }
        store_.refresh_albums();
        transaction.commit();
        result_.tracks_written += pending_.size();
        pending_.clear();
    }

    void finish() {
        Transaction transaction(store_.database());
        result_.tracks_removed = store_.remove_unseen(folder_, seen_);
        store_.refresh_albums();
        store_.remove_orphans();
        store_.touch_folder(folder_);
        transaction.commit();
    }

    bool cue_unchanged(const FoundFile& cue, const std::map<std::string, const FoundFile*>& audio) const {
        if (options_.force) return false;
        const auto known_cue = known_cues_.find(relative(cue.path));
        if (known_cue == known_cues_.end() || known_cue->second.stamp != cue.stamp) return false;
        return std::all_of(audio.begin(), audio.end(), [&](const auto& entry) {
            const auto known = known_files_.find(relative(entry.second->path));
            return known != known_files_.end() && known->second.cue == known_cue->second.id &&
                   known->second.stamp == entry.second->stamp;
        });
    }

    void scan_cue(const FoundFile& cue) {
        CueSheet sheet;
        try {
            sheet = parse_cue_file(cue.path);
        } catch (const std::exception& error) {
            warn(cue.path, error.what());
            return;
        }

        std::set<std::string> names;
        for (const CueTrack& track : sheet.tracks) names.insert(track.file);

        std::map<std::string, const FoundFile*> audio;
        for (const std::string& name : names) {
            if (const FoundFile* file = find_cue_audio(cue, name, names.size() == 1)) {
                audio.emplace(name, file);
            } else {
                warn(cue.path, "cannot find \"" + name + "\"");
            }
        }
        if (audio.empty()) return;

        // Two sheets for one rip would list every track twice. The first one in path order wins.
        for (const auto& entry : audio) {
            if (claimed_.contains(path_key(entry.second->path))) {
                ++result_.duplicate_sheets;
                return;
            }
        }
        for (const auto& entry : audio) claimed_.insert(path_key(entry.second->path));
        ++result_.cue_sheets;

        if (cue_unchanged(cue, audio)) {
            ++result_.unchanged;
            for (const auto& entry : audio) keep(relative(entry.second->path));
            return;
        }

        std::map<std::string, std::optional<FileRead>> reads;
        for (const auto& [name, file] : audio) reads.emplace(name, read_file(*file));
        const std::string cue_path = relative(cue.path);
        for (const CueTrack& cue_track : sheet.tracks) {
            const auto file = audio.find(cue_track.file);
            if (file == audio.end() || !reads.at(cue_track.file)) continue;
            add_cue_track(cue, sheet, cue_track, *file->second, *reads.at(cue_track.file), cue_path);
        }
    }

    void add_cue_track(const FoundFile& cue, const CueSheet& sheet, const CueTrack& cue_track, const FoundFile& file,
                       const FileRead& read, const std::string& cue_path) {
        // One decoder position per sample frame, or per byte of each channel for DSD.
        const uint64_t per_second = read.format.dsd ? read.format.rate / 8 : read.format.rate;
        const uint64_t start = cue_track.start * per_second / kCueFramesPerSecond;
        const uint64_t cue_end = cue_track.end * per_second / kCueFramesPerSecond;
        const uint64_t end = cue_track.end ? std::min(cue_end, read.format.length) : read.format.length;
        if (end <= start) {
            warn(cue.path, "track " + std::to_string(cue_track.number) + " lies outside its file");
            return;
        }

        const Metadata& metadata = read.metadata;
        Track track = make_track(file, read);
        track.part = cue_track.number;
        track.track_number = cue_track.number;
        track.format.length = end - start;
        track.range_start = start;
        track.range_end = end;
        track.pre_emphasis = cue_track.pre_emphasis;
        track.title = first_nonempty({&cue_track.title, &metadata.title, &sheet.title, &track.title});
        track.artist = first_nonempty({&cue_track.performer, &metadata.artist, &sheet.performer});
        track.album_artist = first_nonempty({&metadata.album_artist, &sheet.performer, &metadata.artist});
        track.album = first_nonempty({&sheet.title, &track.album});
        track.composer = first_nonempty({&cue_track.songwriter, &metadata.composer});
        const std::string rem_genre = rem_value(sheet, "GENRE");
        track.genre = first_nonempty({&metadata.genre, &rem_genre});
        if (track.year == 0) track.year = std::atoi(rem_value(sheet, "DATE").c_str());
        if (track.disc_number == 0) track.disc_number = std::atoi(rem_value(sheet, "DISCNUMBER").c_str());
        apply_cue_replay_gain(sheet, cue_track, track.replay_gain);
        add(std::move(track), read.artwork, cue_path, cue.stamp);
    }

    // Lines in the sheet describe the track, or the album, better than tags on a file that holds them all.
    static void apply_cue_replay_gain(const CueSheet& sheet, const CueTrack& track, ReplayGainTags& tags) {
        const auto overlay = [](std::optional<double>& value, std::optional<double> line) {
            if (line) value = line;
        };
        const auto track_rem = [&track](const char* key) {
            const auto found = track.rem.find(key);
            return found == track.rem.end() ? std::string() : found->second;
        };
        overlay(tags.track_gain_db, parse_replay_gain(track_rem("REPLAYGAIN_TRACK_GAIN")));
        overlay(tags.track_peak, parse_replay_peak(track_rem("REPLAYGAIN_TRACK_PEAK")));
        overlay(tags.album_gain_db, parse_replay_gain(rem_value(sheet, "REPLAYGAIN_ALBUM_GAIN")));
        overlay(tags.album_peak, parse_replay_peak(rem_value(sheet, "REPLAYGAIN_ALBUM_PEAK")));
    }

    void scan_audio(const FoundFile& file) {
        if (claimed_.contains(path_key(file.path))) return;

        const std::string relative_path = relative(file.path);
        const auto known = known_files_.find(relative_path);
        if (!options_.force && known != known_files_.end() && known->second.stamp == file.stamp && known->second.cue == 0) {
            ++result_.unchanged;
            keep(relative_path);
            return;
        }

        const std::optional<FileRead> read = read_file(file);
        if (!read) return;
        if (!read->metadata.cue_sheet.empty()) {
            try {
                const CueSheet sheet = parse_cue(read->metadata.cue_sheet);
                for (const CueTrack& cue_track : sheet.tracks) add_cue_track(file, sheet, cue_track, file, *read, {});
                return;
            } catch (const std::exception& error) {
                warn(file.path, std::string("embedded sheet: ") + error.what());
            }
        }
        add(make_track(file, *read), read->artwork);
    }

    static size_t pick_area(const SacdDisc& disc, bool multichannel) {
        for (size_t i = 0; i < disc.areas.size(); ++i) {
            if (disc.areas[i].stereo != multichannel) return i;
        }
        return 0;
    }

    void scan_image(const FoundFile& image) {
        const std::string relative_path = relative(image.path);
        const auto known = known_files_.find(relative_path);
        if (!options_.force && known != known_files_.end() && known->second.stamp == image.stamp) {
            ++result_.unchanged;
            keep(relative_path);
            return;
        }
        if (!is_sacd_image(image.path)) {
            ++result_.ignored_images;
            return;
        }

        SacdDisc disc;
        try {
            disc = read_sacd_disc(image.path);
        } catch (const std::exception& error) {
            warn(image.path, error.what());
            return;
        }
        if (disc.areas.empty()) {
            warn(image.path, "the disc has no audio area");
            return;
        }

        const size_t area_index = pick_area(disc, options_.sacd_multichannel);
        const SacdArea& area = disc.areas[area_index];
        const std::optional<ArtworkFile> artwork = artwork_for(image.path, Metadata{});
        const std::string stem = path_to_utf8(image.path.stem());
        const std::string folder = relative(album_folder(image.path));
        for (size_t i = 0; i < area.tracks.size(); ++i) {
            const SacdTrack& sacd = area.tracks[i];
            Track track;
            track.relative_path = relative_path;
            track.album_folder = folder;
            track.part = static_cast<int>(i + 1);
            track.stamp = image.stamp;
            track.title = sacd.title.empty() ? "Track " + std::to_string(i + 1) : sacd.title;
            track.artist = first_nonempty({&sacd.performer, &disc.album_artist, &disc.disc_artist});
            track.album = first_nonempty({&disc.album_title, &disc.disc_title, &stem});
            track.album_artist = first_nonempty({&disc.album_artist, &disc.disc_artist, &sacd.performer});
            track.composer = sacd.composer;
            track.codec = area.dst ? "SACD DST" : "SACD";
            track.track_number = track.part;
            track.disc_number = disc.disc_number;
            track.year = disc.year;
            track.format = {true, area.rate, area.channels, 1,
                            uint64_t{sacd.duration_frames} * (area.rate / 8) / kCueFramesPerSecond};
            track.sacd_area = static_cast<int>(area_index);
            track.sacd_track = static_cast<int>(i);
            add(std::move(track), artwork);
        }
    }

    LibraryStore& store_;
    ArtworkCache& artwork_;
    std::filesystem::path root_;
    const ScanOptions& options_;
    const ScanProgress& progress_;
    const std::atomic<bool>* cancel_;

    ScanResult result_;
    FolderId folder_ = 0;
    uint64_t done_ = 0;
    uint64_t total_ = 0;
    std::vector<FoundFile> cues_;
    std::vector<FoundFile> audio_;
    std::vector<FoundFile> images_;
    std::unordered_map<std::wstring, const FoundFile*> audio_keys_;
    std::map<std::string, KnownFile> known_files_;
    std::map<std::string, KnownCue> known_cues_;
    std::set<std::wstring> claimed_;
    std::set<std::pair<std::string, int>> seen_;
    std::vector<PendingTrack> pending_;
    std::unordered_map<std::wstring, std::optional<ArtworkFile>> folder_artwork_;
};

}  // namespace

ScanResult scan_folder(LibraryStore& store, ArtworkCache& artwork, const std::filesystem::path& folder,
                       const ScanOptions& options, const ScanProgress& progress, const std::atomic<bool>* cancel) {
    const ComScope com;  // Media Foundation and the image codecs need it
    std::error_code code;
    const std::filesystem::path root = std::filesystem::weakly_canonical(folder, code);
    if (code || !std::filesystem::is_directory(root, code)) {
        throw std::runtime_error("the folder is not available: " + path_to_utf8(folder));
    }
    return FolderScan(store, artwork, root, options, progress, cancel).run();
}

}  // namespace arpegio
