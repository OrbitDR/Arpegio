// The catalogue in SQLite: folders, artists, albums, tracks, favourites, playlists and the saved queue.
// One LibraryStore is one connection and belongs to one thread. The database runs in WAL mode, so a
// scan on its own connection never blocks readers. Failures throw std::runtime_error.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "decode/decoder.h"
#include "library/artwork.h"
#include "library/database.h"

namespace arpegio {

using TrackId = int64_t;
using AlbumId = int64_t;
using ArtistId = int64_t;
using FolderId = int64_t;
using ArtworkId = int64_t;
using PlaylistId = int64_t;
using CueId = int64_t;

constexpr const char* kVariousArtists = "Various Artists";

// The kind of file an album is, for browsing. The order settles a tie between two kinds of tracks.
enum class AlbumSection { Mqa, Dsd, HighRes, Cd, Lossy };

const char* section_name(AlbumSection section);

struct FileStamp {
    uint64_t size = 0;
    int64_t modified = 0;  // file time ticks, compared for equality only

    bool operator==(const FileStamp&) const = default;
};

struct Track {
    TrackId id = 0;
    FolderId folder_id = 0;
    AlbumId album_id = 0;    // read only
    ArtistId artist_id = 0;  // read only
    std::filesystem::path path;  // absolute
    std::string relative_path;   // to the folder, with forward slashes
    int part = 0;                // 0 for a whole file, otherwise its CUE or SACD track number
    FileStamp stamp;

    std::string title;
    std::string artist;
    std::string album;
    std::string album_artist;  // read: the album's credit; written: what this track's tags say
    std::string album_folder;  // written only: tracks with one title and folder make one album
    std::string composer;
    std::string genre;
    std::string comment;
    std::string codec;
    int track_number = 0;
    int disc_number = 0;
    int year = 0;

    SourceFormat format;  // `length` covers this track, not the whole file
    int bitrate = 0;      // kbit/s
    bool pre_emphasis = false;
    bool mqa = false;

    uint64_t range_start = 0;  // in `format` units; a range_end of 0 means the whole file
    uint64_t range_end = 0;
    int sacd_area = -1;  // >= 0 for a track inside a SACD image
    int sacd_track = -1;

    ArtworkId artwork_id = 0;
    ArtworkId album_artwork_id = 0;
    bool favorite = false;
};

struct Album {
    AlbumId id = 0;
    std::string title;
    std::string album_artist;
    int year = 0;
    ArtworkId artwork_id = 0;
    int track_count = 0;
    double seconds = 0.0;
    std::string codec;  // of its best track, like the three below
    uint32_t rate = 0;
    int bits = 0;
    bool dsd = false;
    AlbumSection section = AlbumSection::Cd;  // the one most of its tracks fall in
    double added_at = 0.0;  // seconds since 1970, of its newest track
};

struct Artist {
    ArtistId id = 0;
    std::string name;
    int album_count = 0;  // albums credited to this artist
    int track_count = 0;  // tracks performed by this artist
    ArtworkId artwork_id = 0;
};

struct Folder {
    FolderId id = 0;
    std::filesystem::path path;
};

struct Playlist {
    PlaylistId id = 0;
    std::string name;
    int track_count = 0;
};

// What a scan needs to know to skip a file that did not change.
struct KnownFile {
    FileStamp stamp;
    CueId cue = 0;
    std::vector<int> parts;
};

struct KnownCue {
    CueId id = 0;
    FileStamp stamp;
};

class LibraryStore {
public:
    explicit LibraryStore(const std::filesystem::path& database);

    Database& database() { return database_; }
    int schema_version() { return database_.user_version(); }

    // Folders
    FolderId add_folder(const std::filesystem::path& folder);
    void remove_folder(const std::filesystem::path& folder);
    std::vector<Folder> folders();

    // Writes from a scan. Callers wrap them in a Transaction.
    std::map<std::string, KnownFile> known_files(FolderId folder);
    std::map<std::string, KnownCue> known_cues(FolderId folder);
    CueId upsert_cue(FolderId folder, const std::string& relative_path, const FileStamp& stamp);
    TrackId upsert_track(FolderId folder, const Track& track, CueId cue, const ArtworkFile* artwork);
    uint64_t remove_unseen(FolderId folder, const std::set<std::pair<std::string, int>>& seen);
    void refresh_albums();  // works out credit, year and cover of the albums touched since the last call
    void remove_orphans();  // CUE sheets, albums and artists nothing refers to any more
    void touch_folder(FolderId folder);

    // Browsing
    std::vector<Track> tracks();
    std::optional<Track> track(TrackId id);
    std::vector<Track> album_tracks(AlbumId album);
    std::vector<Track> artist_tracks(ArtistId artist);
    std::vector<Track> folder_tracks(FolderId folder);
    std::vector<Album> albums();
    std::vector<Album> artist_albums(ArtistId artist);
    std::vector<Artist> artists();
    std::vector<Track> search(std::string_view text, size_t limit = 200);
    std::optional<ArtworkFile> artwork(ArtworkId id);

    // Favourites
    void set_favorite(TrackId id, bool favorite);
    std::vector<Track> favorites();

    // Playlists
    PlaylistId create_playlist(std::string_view name);
    void rename_playlist(PlaylistId id, std::string_view name);
    void delete_playlist(PlaylistId id);
    void add_to_playlist(PlaylistId id, TrackId track);
    void remove_from_playlist(PlaylistId id, size_t index);  // index in playlist_tracks()
    std::vector<Playlist> playlists();
    std::vector<Track> playlist_tracks(PlaylistId id);

    // Saved queue, restored at the next start
    void set_queue(const std::vector<TrackId>& tracks);
    std::vector<Track> queue();

private:
    ArtistId ensure_artist(std::string_view name);
    AlbumId ensure_album(FolderId folder, const std::string& directory, std::string_view title, ArtistId credit);
    ArtworkId ensure_artwork(const ArtworkFile& artwork);
    ArtistId album_credit(AlbumId album);
    std::vector<Track> read_tracks(Statement& statement);
    std::vector<Album> read_albums(Statement& statement);

    Database database_;
    std::set<AlbumId> changed_albums_;
};

}  // namespace arpegio
