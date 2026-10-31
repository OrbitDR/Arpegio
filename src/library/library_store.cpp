#include "library/library_store.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <sstream>
#include <stdexcept>

#include "library/paths.h"

namespace arpegio {

namespace {

constexpr const char* kUnknownArtist = "Unknown Artist";
constexpr const char* kUnknownAlbum = "Unknown Album";
constexpr const char* kUnknownTitle = "Unknown Title";

// Applied in order. Each entry moves the database one version forward; released ones never change.
constexpr const char* kMigrations[] = {
    R"SQL(
CREATE TABLE folders (
    folder_id INTEGER PRIMARY KEY,
    path TEXT NOT NULL UNIQUE COLLATE NOCASE,
    last_scan REAL
);

CREATE TABLE artists (
    artist_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE COLLATE NOCASE
);

CREATE TABLE artwork (
    artwork_id INTEGER PRIMARY KEY,
    hash TEXT NOT NULL UNIQUE,
    file TEXT NOT NULL,
    mime TEXT NOT NULL,
    width INTEGER NOT NULL,
    height INTEGER NOT NULL
);

-- One title in one folder is one album. Neither the year nor the artist can be part of that: tags
-- differ between the tracks of a compilation, and an artist can have several self-titled albums.
CREATE TABLE albums (
    album_id INTEGER PRIMARY KEY,
    folder_id INTEGER NOT NULL REFERENCES folders(folder_id) ON DELETE CASCADE,
    directory TEXT NOT NULL,
    title TEXT NOT NULL COLLATE NOCASE,
    album_artist_id INTEGER NOT NULL REFERENCES artists(artist_id),
    year INTEGER NOT NULL DEFAULT 0,
    artwork_id INTEGER REFERENCES artwork(artwork_id) ON DELETE SET NULL,
    UNIQUE (folder_id, directory, title)
);

CREATE TABLE cues (
    cue_id INTEGER PRIMARY KEY,
    folder_id INTEGER NOT NULL REFERENCES folders(folder_id) ON DELETE CASCADE,
    relative_path TEXT NOT NULL,
    file_size INTEGER NOT NULL,
    modified INTEGER NOT NULL,
    UNIQUE (folder_id, relative_path)
);

CREATE TABLE tracks (
    track_id INTEGER PRIMARY KEY,
    folder_id INTEGER NOT NULL REFERENCES folders(folder_id) ON DELETE CASCADE,
    relative_path TEXT NOT NULL,
    part INTEGER NOT NULL,
    file_size INTEGER NOT NULL,
    modified INTEGER NOT NULL,

    title TEXT NOT NULL,
    artist_id INTEGER NOT NULL REFERENCES artists(artist_id),
    album_artist_id INTEGER NOT NULL REFERENCES artists(artist_id),
    album_id INTEGER NOT NULL REFERENCES albums(album_id),
    composer TEXT NOT NULL,
    genre TEXT NOT NULL,
    comment TEXT NOT NULL,
    codec TEXT NOT NULL,
    track_no INTEGER NOT NULL,
    disc_no INTEGER NOT NULL,
    year INTEGER NOT NULL,

    is_dsd INTEGER NOT NULL,
    rate INTEGER NOT NULL,
    channels INTEGER NOT NULL,
    bits INTEGER NOT NULL,
    length INTEGER NOT NULL,
    bitrate INTEGER NOT NULL,
    pre_emphasis INTEGER NOT NULL,

    cue_id INTEGER REFERENCES cues(cue_id) ON DELETE CASCADE,
    range_start INTEGER NOT NULL,
    range_end INTEGER NOT NULL,
    sacd_area INTEGER NOT NULL,
    sacd_track INTEGER NOT NULL,

    artwork_id INTEGER REFERENCES artwork(artwork_id) ON DELETE SET NULL,
    added_at REAL NOT NULL,
    UNIQUE (folder_id, relative_path, part)
);

CREATE INDEX idx_tracks_album ON tracks(album_id);
CREATE INDEX idx_tracks_artist ON tracks(artist_id);
CREATE INDEX idx_tracks_cue ON tracks(cue_id);
CREATE INDEX idx_albums_artist ON albums(album_artist_id);

CREATE TABLE favorites (
    track_id INTEGER PRIMARY KEY REFERENCES tracks(track_id) ON DELETE CASCADE,
    added_at REAL NOT NULL
);

CREATE TABLE playlists (
    playlist_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    created_at REAL NOT NULL,
    updated_at REAL NOT NULL
);

CREATE TABLE playlist_tracks (
    playlist_id INTEGER NOT NULL REFERENCES playlists(playlist_id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    track_id INTEGER NOT NULL REFERENCES tracks(track_id) ON DELETE CASCADE,
    PRIMARY KEY (playlist_id, position)
);
CREATE INDEX idx_playlist_tracks_track ON playlist_tracks(track_id);

CREATE TABLE queue_items (
    position INTEGER PRIMARY KEY,
    track_id INTEGER NOT NULL REFERENCES tracks(track_id) ON DELETE CASCADE
);

-- The rowid is the track_id. The trigger also covers deletes that cascade from folders and CUE sheets.
CREATE VIRTUAL TABLE tracks_fts USING fts5(
    title, artist, album, album_artist, composer, genre,
    tokenize = 'unicode61 remove_diacritics 2'
);

CREATE TRIGGER tracks_fts_delete AFTER DELETE ON tracks BEGIN
    DELETE FROM tracks_fts WHERE rowid = old.track_id;
END;
)SQL",
};

constexpr const char* kTrackQuery = R"SQL(
    SELECT t.track_id, t.folder_id, f.path, t.relative_path, t.part, t.file_size, t.modified,
           t.title, ar.name, al.title, aa.name, t.composer, t.genre, t.comment, t.codec,
           t.track_no, t.disc_no, t.year,
           t.is_dsd, t.rate, t.channels, t.bits, t.length, t.bitrate, t.pre_emphasis,
           t.range_start, t.range_end, t.sacd_area, t.sacd_track,
           COALESCE(t.artwork_id, 0), COALESCE(al.artwork_id, 0),
           EXISTS (SELECT 1 FROM favorites v WHERE v.track_id = t.track_id)
    FROM tracks t
    JOIN folders f ON f.folder_id = t.folder_id
    JOIN artists ar ON ar.artist_id = t.artist_id
    JOIN albums al ON al.album_id = t.album_id
    JOIN artists aa ON aa.artist_id = al.album_artist_id
)SQL";

constexpr const char* kAlbumQuery = R"SQL(
    SELECT al.album_id, al.title, aa.name, al.year, COALESCE(al.artwork_id, 0),
           (SELECT COUNT(*) FROM tracks t WHERE t.album_id = al.album_id)
    FROM albums al
    JOIN artists aa ON aa.artist_id = al.album_artist_id
)SQL";

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string with_query(const char* query, std::string_view suffix) {
    std::string sql(query);
    sql += suffix;
    return sql;
}

// Every word becomes a quoted prefix, so punctuation in the search box cannot be read as FTS syntax.
std::string fts_query(std::string_view text) {
    std::istringstream input{std::string(text)};
    std::string word;
    std::string out;
    while (input >> word) {
        word.erase(std::remove(word.begin(), word.end(), '"'), word.end());
        if (word.empty()) continue;
        if (!out.empty()) out.push_back(' ');
        out += '"' + word + "\"*";
    }
    return out;
}

std::string like_pattern(std::string_view text) {
    std::string out = "%";
    for (const char c : text) {
        if (c == '%' || c == '_' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('%');
    return out;
}

void migrate(Database& database) {
    const int latest = static_cast<int>(std::size(kMigrations));
    const int version = database.user_version();
    if (version > latest) throw std::runtime_error("the database comes from a newer version of Arpegio");
    for (int next = version; next < latest; ++next) {
        Transaction transaction(database);
        database.exec(kMigrations[next]);
        database.set_user_version(next + 1);
        transaction.commit();
    }
}

}  // namespace

LibraryStore::LibraryStore(const std::filesystem::path& database) : database_(database) {
    database_.exec("PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA foreign_keys = ON;");
    migrate(database_);
}

FolderId LibraryStore::add_folder(const std::filesystem::path& folder) {
    return database_
        .query("INSERT INTO folders(path) VALUES (?) ON CONFLICT(path) DO UPDATE SET path = path RETURNING folder_id",
               path_to_utf8(folder))
        .single();
}

void LibraryStore::remove_folder(const std::filesystem::path& folder) {
    Transaction transaction(database_);
    database_.query("DELETE FROM folders WHERE path = ?", path_to_utf8(folder)).run();
    remove_orphans();
    transaction.commit();
}

std::vector<Folder> LibraryStore::folders() {
    std::vector<Folder> out;
    Statement statement = database_.prepare("SELECT folder_id, path FROM folders ORDER BY path");
    while (statement.step()) out.push_back({statement.integer(0), path_from_utf8(statement.text(1))});
    return out;
}

std::map<std::string, KnownFile> LibraryStore::known_files(FolderId folder) {
    std::map<std::string, KnownFile> out;
    Statement statement = database_.query(
        "SELECT relative_path, file_size, modified, COALESCE(cue_id, 0), part FROM tracks WHERE folder_id = ?", folder);
    while (statement.step()) {
        KnownFile& file = out[statement.text(0)];
        file.stamp = {static_cast<uint64_t>(statement.integer(1)), statement.integer(2)};
        file.cue = statement.integer(3);
        file.parts.push_back(static_cast<int>(statement.integer(4)));
    }
    return out;
}

std::map<std::string, KnownCue> LibraryStore::known_cues(FolderId folder) {
    std::map<std::string, KnownCue> out;
    Statement statement =
        database_.query("SELECT relative_path, cue_id, file_size, modified FROM cues WHERE folder_id = ?", folder);
    while (statement.step()) {
        out[statement.text(0)] = {statement.integer(1), {static_cast<uint64_t>(statement.integer(2)), statement.integer(3)}};
    }
    return out;
}

CueId LibraryStore::upsert_cue(FolderId folder, const std::string& relative_path, const FileStamp& stamp) {
    return database_
        .query(
            "INSERT INTO cues(folder_id, relative_path, file_size, modified) VALUES (?, ?, ?, ?) "
            "ON CONFLICT(folder_id, relative_path) DO UPDATE SET file_size = excluded.file_size, "
            "modified = excluded.modified RETURNING cue_id",
            folder, relative_path, stamp.size, stamp.modified)
        .single();
}

ArtistId LibraryStore::ensure_artist(std::string_view name) {
    return database_
        .query("INSERT INTO artists(name) VALUES (?) ON CONFLICT(name) DO UPDATE SET name = name RETURNING artist_id", name)
        .single();
}

AlbumId LibraryStore::ensure_album(FolderId folder, const std::string& directory, std::string_view title, ArtistId credit) {
    return database_
        .query(
            "INSERT INTO albums(folder_id, directory, title, album_artist_id) VALUES (?, ?, ?, ?) "
            "ON CONFLICT(folder_id, directory, title) DO UPDATE SET title = title RETURNING album_id",
            folder, directory, title, credit)
        .single();
}

ArtworkId LibraryStore::ensure_artwork(const ArtworkFile& artwork) {
    return database_
        .query(
            "INSERT INTO artwork(hash, file, mime, width, height) VALUES (?, ?, ?, ?, ?) "
            "ON CONFLICT(hash) DO UPDATE SET hash = hash RETURNING artwork_id",
            artwork.hash, artwork.file, artwork.mime, artwork.width, artwork.height)
        .single();
}

TrackId LibraryStore::upsert_track(FolderId folder, const Track& track, CueId cue, const ArtworkFile* artwork) {
    const std::string artist = !track.artist.empty()         ? track.artist
                               : !track.album_artist.empty() ? track.album_artist
                                                             : kUnknownArtist;
    const std::string album_artist = track.album_artist.empty() ? artist : track.album_artist;
    const std::string album = track.album.empty() ? kUnknownAlbum : track.album;
    const std::string title = track.title.empty() ? kUnknownTitle : track.title;

    const ArtistId artist_id = ensure_artist(artist);
    const ArtistId album_artist_id = ensure_artist(album_artist);
    const AlbumId album_id = ensure_album(folder, track.album_folder, album, album_artist_id);
    const std::optional<CueId> cue_id = cue ? std::optional(cue) : std::nullopt;
    const std::optional<ArtworkId> artwork_id = artwork ? std::optional(ensure_artwork(*artwork)) : std::nullopt;

    // A retagged track leaves its old album behind, and that one changes too.
    Statement previous =
        database_.query("SELECT album_id FROM tracks WHERE folder_id = ? AND relative_path = ? AND part = ?", folder,
                        track.relative_path, track.part);
    if (previous.step()) changed_albums_.insert(previous.integer(0));
    changed_albums_.insert(album_id);

    const TrackId id =
        database_
            .query(R"SQL(
        INSERT INTO tracks(folder_id, relative_path, part, file_size, modified, title, artist_id, album_artist_id, album_id,
                           composer, genre, comment, codec, track_no, disc_no, year, is_dsd, rate, channels, bits, length,
                           bitrate, pre_emphasis, cue_id, range_start, range_end, sacd_area, sacd_track, artwork_id,
                           added_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(folder_id, relative_path, part) DO UPDATE SET
            file_size = excluded.file_size, modified = excluded.modified, title = excluded.title,
            artist_id = excluded.artist_id, album_artist_id = excluded.album_artist_id, album_id = excluded.album_id,
            composer = excluded.composer,
            genre = excluded.genre, comment = excluded.comment, codec = excluded.codec, track_no = excluded.track_no,
            disc_no = excluded.disc_no, year = excluded.year, is_dsd = excluded.is_dsd, rate = excluded.rate,
            channels = excluded.channels, bits = excluded.bits, length = excluded.length, bitrate = excluded.bitrate,
            pre_emphasis = excluded.pre_emphasis, cue_id = excluded.cue_id, range_start = excluded.range_start,
            range_end = excluded.range_end, sacd_area = excluded.sacd_area, sacd_track = excluded.sacd_track,
            artwork_id = excluded.artwork_id
        RETURNING track_id)SQL",
                   folder, track.relative_path, track.part, track.stamp.size, track.stamp.modified, title, artist_id,
                   album_artist_id, album_id, track.composer, track.genre, track.comment, track.codec, track.track_number,
                   track.disc_number, track.year, track.format.dsd, track.format.rate, track.format.channels,
                   track.format.bits, track.format.length, track.bitrate, track.pre_emphasis, cue_id, track.range_start,
                   track.range_end, track.sacd_area, track.sacd_track, artwork_id, now_seconds())
            .single();

    database_.query("DELETE FROM tracks_fts WHERE rowid = ?", id).run();
    database_
        .query(
            "INSERT INTO tracks_fts(rowid, title, artist, album, album_artist, composer, genre) VALUES (?, ?, ?, ?, ?, ?, "
            "?)",
            id, title, artist, album, album_artist, track.composer, track.genre)
        .run();
    return id;
}

uint64_t LibraryStore::remove_unseen(FolderId folder, const std::set<std::pair<std::string, int>>& seen) {
    std::vector<TrackId> stale;
    {
        Statement statement =
            database_.query("SELECT track_id, relative_path, part, album_id FROM tracks WHERE folder_id = ?", folder);
        while (statement.step()) {
            if (seen.contains({statement.text(1), static_cast<int>(statement.integer(2))})) continue;
            stale.push_back(statement.integer(0));
            changed_albums_.insert(statement.integer(3));
        }
    }
    Statement remove = database_.prepare("DELETE FROM tracks WHERE track_id = ?");
    for (const TrackId id : stale) remove.bind(1, id).run();
    return stale.size();
}

// The artist the tags name on most of the tracks. When no artist has more than half of them the album
// is a compilation, even if nobody tagged it as one.
ArtistId LibraryStore::album_credit(AlbumId album) {
    Statement credits = database_.query(
        "SELECT album_artist_id, COUNT(*) FROM tracks WHERE album_id = ? GROUP BY album_artist_id ORDER BY COUNT(*) DESC",
        album);
    ArtistId credit = 0;
    int64_t most = 0;
    int64_t total = 0;
    while (credits.step()) {
        if (credit == 0) {
            credit = credits.integer(0);
            most = credits.integer(1);
        }
        total += credits.integer(1);
    }
    return credit == 0 || most * 2 > total ? credit : ensure_artist(kVariousArtists);
}

void LibraryStore::refresh_albums() {
    for (const AlbumId album : changed_albums_) {
        const ArtistId credit = album_credit(album);
        if (credit == 0) continue;  // no tracks left, remove_orphans() takes it
        database_
            .query(R"SQL(
            UPDATE albums SET album_artist_id = ?1,
                year = (SELECT MAX(year) FROM tracks WHERE album_id = ?2),
                artwork_id = (SELECT artwork_id FROM tracks WHERE album_id = ?2 AND artwork_id IS NOT NULL
                              ORDER BY disc_no, track_no, relative_path, part LIMIT 1)
            WHERE album_id = ?2)SQL",
                   credit, album)
            .run();
    }
    changed_albums_.clear();
}

void LibraryStore::remove_orphans() {
    database_.exec(R"SQL(
        DELETE FROM cues WHERE cue_id NOT IN (SELECT cue_id FROM tracks WHERE cue_id IS NOT NULL);
        DELETE FROM albums WHERE album_id NOT IN (SELECT album_id FROM tracks);
        DELETE FROM artists WHERE artist_id NOT IN (
            SELECT artist_id FROM tracks UNION SELECT album_artist_id FROM tracks UNION SELECT album_artist_id FROM albums);
    )SQL");
}

void LibraryStore::touch_folder(FolderId folder) {
    database_.query("UPDATE folders SET last_scan = ? WHERE folder_id = ?", now_seconds(), folder).run();
}

std::vector<Track> LibraryStore::read_tracks(Statement& statement) {
    std::vector<Track> out;
    while (statement.step()) {
        Track track;
        track.id = statement.integer(0);
        track.folder_id = statement.integer(1);
        track.relative_path = statement.text(3);
        track.path = path_from_utf8(statement.text(2)) / path_from_utf8(track.relative_path);
        track.part = static_cast<int>(statement.integer(4));
        track.stamp = {static_cast<uint64_t>(statement.integer(5)), statement.integer(6)};
        track.title = statement.text(7);
        track.artist = statement.text(8);
        track.album = statement.text(9);
        track.album_artist = statement.text(10);
        track.composer = statement.text(11);
        track.genre = statement.text(12);
        track.comment = statement.text(13);
        track.codec = statement.text(14);
        track.track_number = static_cast<int>(statement.integer(15));
        track.disc_number = static_cast<int>(statement.integer(16));
        track.year = static_cast<int>(statement.integer(17));
        track.format.dsd = statement.integer(18) != 0;
        track.format.rate = static_cast<uint32_t>(statement.integer(19));
        track.format.channels = static_cast<int>(statement.integer(20));
        track.format.bits = static_cast<int>(statement.integer(21));
        track.format.length = static_cast<uint64_t>(statement.integer(22));
        track.bitrate = static_cast<int>(statement.integer(23));
        track.pre_emphasis = statement.integer(24) != 0;
        track.range_start = static_cast<uint64_t>(statement.integer(25));
        track.range_end = static_cast<uint64_t>(statement.integer(26));
        track.sacd_area = static_cast<int>(statement.integer(27));
        track.sacd_track = static_cast<int>(statement.integer(28));
        track.artwork_id = statement.integer(29);
        track.album_artwork_id = statement.integer(30);
        track.favorite = statement.integer(31) != 0;
        out.push_back(std::move(track));
    }
    return out;
}

std::vector<Album> LibraryStore::read_albums(Statement& statement) {
    std::vector<Album> out;
    while (statement.step()) {
        out.push_back({statement.integer(0), statement.text(1), statement.text(2), static_cast<int>(statement.integer(3)),
                       statement.integer(4), static_cast<int>(statement.integer(5))});
    }
    return out;
}

std::vector<Track> LibraryStore::tracks() {
    Statement statement = database_.prepare(with_query(kTrackQuery, " ORDER BY f.path, t.relative_path, t.part"));
    return read_tracks(statement);
}

std::optional<Track> LibraryStore::track(TrackId id) {
    Statement statement = database_.query(with_query(kTrackQuery, " WHERE t.track_id = ?"), id);
    std::vector<Track> found = read_tracks(statement);
    if (found.empty()) return std::nullopt;
    return std::move(found.front());
}

std::vector<Track> LibraryStore::album_tracks(AlbumId album) {
    Statement statement = database_.query(
        with_query(kTrackQuery, " WHERE t.album_id = ? ORDER BY t.disc_no, t.track_no, t.relative_path, t.part"), album);
    return read_tracks(statement);
}

std::vector<Track> LibraryStore::artist_tracks(ArtistId artist) {
    Statement statement = database_.query(
        with_query(kTrackQuery,
                   " WHERE t.artist_id = ? ORDER BY al.year, al.title, t.disc_no, t.track_no, t.relative_path, t.part"),
        artist);
    return read_tracks(statement);
}

std::vector<Track> LibraryStore::folder_tracks(FolderId folder) {
    Statement statement =
        database_.query(with_query(kTrackQuery, " WHERE t.folder_id = ? ORDER BY t.relative_path, t.part"), folder);
    return read_tracks(statement);
}

std::vector<Album> LibraryStore::albums() {
    Statement statement = database_.prepare(with_query(kAlbumQuery, " ORDER BY al.title, al.year"));
    return read_albums(statement);
}

std::vector<Album> LibraryStore::artist_albums(ArtistId artist) {
    Statement statement = database_.query(with_query(kAlbumQuery,
                                                     " WHERE al.album_artist_id = ?1 OR al.album_id IN "
                                                     "(SELECT album_id FROM tracks WHERE artist_id = ?1) "
                                                     "ORDER BY al.year, al.title"),
                                          artist);
    return read_albums(statement);
}

std::vector<Artist> LibraryStore::artists() {
    Statement statement = database_.prepare(R"SQL(
        SELECT ar.artist_id, ar.name,
               (SELECT COUNT(*) FROM albums al WHERE al.album_artist_id = ar.artist_id),
               (SELECT COUNT(*) FROM tracks t WHERE t.artist_id = ar.artist_id)
        FROM artists ar ORDER BY ar.name)SQL");
    std::vector<Artist> out;
    while (statement.step()) {
        out.push_back({statement.integer(0), statement.text(1), static_cast<int>(statement.integer(2)),
                       static_cast<int>(statement.integer(3))});
    }
    return out;
}

std::vector<Track> LibraryStore::search(std::string_view text, size_t limit) {
    const std::string query = fts_query(text);
    if (query.empty()) return {};
    {
        Statement statement = database_.query(with_query(kTrackQuery,
                                                         " JOIN tracks_fts ON tracks_fts.rowid = t.track_id "
                                                         "WHERE tracks_fts MATCH ? ORDER BY rank LIMIT ?"),
                                              query, limit);
        std::vector<Track> found = read_tracks(statement);
        if (!found.empty()) return found;
    }

    // The tokenizer drops symbols, so a title like "÷" only exists for a plain comparison.
    Statement statement = database_.query(with_query(kTrackQuery,
                                                     " WHERE t.title LIKE ?1 ESCAPE '\\' OR ar.name LIKE ?1 ESCAPE '\\' "
                                                     "OR al.title LIKE ?1 ESCAPE '\\' ORDER BY t.title LIMIT ?2"),
                                          like_pattern(text), limit);
    return read_tracks(statement);
}

std::optional<ArtworkFile> LibraryStore::artwork(ArtworkId id) {
    Statement statement = database_.query("SELECT hash, file, mime, width, height FROM artwork WHERE artwork_id = ?", id);
    if (!statement.step()) return std::nullopt;
    return ArtworkFile{statement.text(0), statement.text(1), statement.text(2), static_cast<int>(statement.integer(3)),
                       static_cast<int>(statement.integer(4))};
}

void LibraryStore::set_favorite(TrackId id, bool favorite) {
    if (favorite) {
        database_.query("INSERT OR IGNORE INTO favorites(track_id, added_at) VALUES (?, ?)", id, now_seconds()).run();
    } else {
        database_.query("DELETE FROM favorites WHERE track_id = ?", id).run();
    }
}

std::vector<Track> LibraryStore::favorites() {
    Statement statement = database_.prepare(
        with_query(kTrackQuery, " JOIN favorites fav ON fav.track_id = t.track_id ORDER BY fav.added_at DESC"));
    return read_tracks(statement);
}

PlaylistId LibraryStore::create_playlist(std::string_view name) {
    const double now = now_seconds();
    return database_
        .query("INSERT INTO playlists(name, created_at, updated_at) VALUES (?, ?, ?) RETURNING playlist_id", name, now, now)
        .single();
}

void LibraryStore::delete_playlist(PlaylistId id) {
    database_.query("DELETE FROM playlists WHERE playlist_id = ?", id).run();
}

void LibraryStore::add_to_playlist(PlaylistId id, TrackId track) {
    Transaction transaction(database_);
    database_
        .query(
            "INSERT INTO playlist_tracks(playlist_id, position, track_id) "
            "SELECT ?1, COALESCE(MAX(position), -1) + 1, ?2 FROM playlist_tracks WHERE playlist_id = ?1",
            id, track)
        .run();
    database_.query("UPDATE playlists SET updated_at = ? WHERE playlist_id = ?", now_seconds(), id).run();
    transaction.commit();
}

void LibraryStore::remove_from_playlist(PlaylistId id, size_t index) {
    // Positions keep gaps after a removal, so the caller's index is resolved against the current order.
    Transaction transaction(database_);
    database_
        .query(
            "DELETE FROM playlist_tracks WHERE playlist_id = ?1 AND position = "
            "(SELECT position FROM playlist_tracks WHERE playlist_id = ?1 ORDER BY position LIMIT 1 OFFSET ?2)",
            id, index)
        .run();
    database_.query("UPDATE playlists SET updated_at = ? WHERE playlist_id = ?", now_seconds(), id).run();
    transaction.commit();
}

std::vector<Playlist> LibraryStore::playlists() {
    Statement statement = database_.prepare(
        "SELECT p.playlist_id, p.name, (SELECT COUNT(*) FROM playlist_tracks t WHERE t.playlist_id = p.playlist_id) "
        "FROM playlists p ORDER BY p.name COLLATE NOCASE");
    std::vector<Playlist> out;
    while (statement.step()) {
        out.push_back({statement.integer(0), statement.text(1), static_cast<int>(statement.integer(2))});
    }
    return out;
}

std::vector<Track> LibraryStore::playlist_tracks(PlaylistId id) {
    Statement statement = database_.query(
        with_query(kTrackQuery,
                   " JOIN playlist_tracks pt ON pt.track_id = t.track_id WHERE pt.playlist_id = ? ORDER BY pt.position"),
        id);
    return read_tracks(statement);
}

void LibraryStore::set_queue(const std::vector<TrackId>& tracks) {
    Transaction transaction(database_);
    database_.exec("DELETE FROM queue_items");
    Statement insert = database_.prepare("INSERT INTO queue_items(position, track_id) VALUES (?, ?)");
    for (size_t i = 0; i < tracks.size(); ++i) insert.bind(1, i).bind(2, tracks[i]).run();
    transaction.commit();
}

std::vector<Track> LibraryStore::queue() {
    Statement statement =
        database_.prepare(with_query(kTrackQuery, " JOIN queue_items q ON q.track_id = t.track_id ORDER BY q.position"));
    return read_tracks(statement);
}

}  // namespace arpegio
