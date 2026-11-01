#include "app/controller.h"

#include <windows.h>

#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

#include "library/paths.h"
#include "library/track_source.h"

using nlohmann::json;

namespace arpegio {

namespace {

constexpr double kMinVolumeDb = -60.0;  // the bottom of the slider, which is silence
constexpr int kVolumeSaveTicks = 5;

double volume_gain(double db) {
    return db <= kMinVolumeDb ? 0.0 : std::pow(10.0, db / 20.0);
}

double gain_db(double gain) {
    return 20.0 * std::log10(gain);
}

ReplayGainMode replay_gain_mode(const std::string& name) {
    if (name == "track") return ReplayGainMode::Track;
    if (name == "album") return ReplayGainMode::Album;
    return ReplayGainMode::Off;
}

const char* kind_key(OutputKind kind) {
    switch (kind) {
    case OutputKind::Asio: return "asio";
    case OutputKind::WasapiExclusive: return "exclusive";
    case OutputKind::WasapiShared: return "shared";
    }
    return "?";
}

std::string device_key(const DeviceInfo& device) {
    return std::string(kind_key(device.kind)) + ":" + path_to_utf8(device.id);
}

DsdPolicy dsd_policy(const std::string& name) {
    if (name == "native") return DsdPolicy::Native;
    if (name == "dop") return DsdPolicy::Dop;
    if (name == "pcm") return DsdPolicy::Pcm;
    return DsdPolicy::Auto;
}

RepeatMode repeat_mode(const std::string& name) {
    if (name == "queue") return RepeatMode::Queue;
    if (name == "track") return RepeatMode::Track;
    return RepeatMode::Off;
}

const char* state_name(PlayerState state) {
    switch (state) {
    case PlayerState::Playing: return "playing";
    case PlayerState::Paused: return "paused";
    case PlayerState::Stopped: break;
    }
    return "stopped";
}

const char* path_name(RenderPath path) {
    switch (path) {
    case RenderPath::Pcm: return "pcm";
    case RenderPath::DsdNative: return "dsdNative";
    case RenderPath::DsdDop: return "dop";
    case RenderPath::DsdToPcm: return "dsdToPcm";
    }
    return "?";
}

// The catalogue stores these in English and the interface is in Spanish.
std::string display_name(const std::string& name) {
    if (name == kVariousArtists) return "Varios artistas";
    if (name == "Unknown Artist") return "Artista desconocido";
    if (name == "Unknown Album") return "Álbum desconocido";
    if (name == "Unknown Title") return "Sin título";
    return name;
}

ArtworkId track_artwork(const Track& track) {
    return track.artwork_id ? track.artwork_id : track.album_artwork_id;
}

json track_json(const Track& track) {
    return {{"id", track.id},
            {"title", display_name(track.title)},
            {"artist", display_name(track.artist)},
            {"artistId", track.artist_id},
            {"album", display_name(track.album)},
            {"albumId", track.album_id},
            {"albumArtist", display_name(track.album_artist)},
            {"composer", track.composer},
            {"genre", track.genre},
            {"codec", track.codec},
            {"track", track.track_number},
            {"disc", track.disc_number},
            {"year", track.year},
            {"seconds", track.format.rate ? track.format.seconds() : 0.0},
            {"rate", track.format.rate},
            {"bits", track.format.bits},
            {"channels", track.format.channels},
            {"dsd", track.format.dsd},
            {"mqa", track.mqa},
            {"bitrate", track.bitrate},
            {"art", track_artwork(track)},
            {"favorite", track.favorite},
            {"folder", track.folder_id},
            {"path", track.relative_path},
            {"part", track.part}};
}

json tracks_json(const std::vector<Track>& tracks) {
    json out = json::array();
    for (const Track& track : tracks) out.push_back(track_json(track));
    return out;
}

json album_json(const Album& album) {
    return {{"id", album.id},
            {"title", display_name(album.title)},
            {"artist", display_name(album.album_artist)},
            {"year", album.year},
            {"art", album.artwork_id},
            {"tracks", album.track_count},
            {"seconds", album.seconds},
            {"codec", album.codec},
            {"rate", album.rate},
            {"bits", album.bits},
            {"dsd", album.dsd},
            {"mqa", album.section == AlbumSection::Mqa},
            {"section", section_name(album.section)},
            {"added", album.added_at}};
}

json albums_json(const std::vector<Album>& albums) {
    json out = json::array();
    for (const Album& album : albums) out.push_back(album_json(album));
    return out;
}

json plan_json(const RenderPlan& plan) {
    return {{"path", path_name(plan.path)},
            {"encoding", wire_name(plan.device.encoding)},
            {"rate", plan.device.rate},
            {"channels", plan.device.channels},
            {"validBits", plan.device.valid_bits},
            {"resampleFrom", plan.resample_from},
            {"mixChannels", plan.mix_channels},
            {"gainDb", gain_db(plan.gain)},
            {"ditherBits", plan.dither_bits},
            {"gainDitherBits", plan.gain_dither_bits},
            {"acceptsGain", plan.accepts_gain()},
            {"firRate", plan.path == RenderPath::DsdToPcm ? plan.dsd.fir_rate : 0},
            {"firDivider", plan.path == RenderPath::DsdToPcm ? plan.dsd.divider : 0},
            {"firCutoffHz", plan.path == RenderPath::DsdToPcm ? plan.dsd.filter.f3_hz : 0.0}};
}

json device_settings_json(const DeviceSettings& device) {
    return {{"dsd", device.dsd},
            {"dopConfirmed", device.dop_confirmed},
            {"dsdGainDb", device.dsd_gain_db},
            {"firstChannel", device.first_channel},
            {"periodMs", device.period_ms},
            {"bufferFrames", device.buffer_frames},
            {"volumeDb", device.volume_db}};
}

std::vector<DeviceInfo> list_real_devices() {
    std::vector<DeviceInfo> devices = list_asio_devices();
    for (const OutputKind kind : {OutputKind::WasapiExclusive, OutputKind::WasapiShared}) {
        std::vector<DeviceInfo> more = list_wasapi_devices(kind);
        devices.insert(devices.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
    }
    return devices;
}

}  // namespace

Controller::Controller(Options options)
    : settings_file_(options.data_folder / "settings.json"),
      emit_(std::move(options.emit)),
      list_devices_(options.list_devices ? std::move(options.list_devices) : list_real_devices),
      settings_(load_settings(settings_file_)),
      player_(std::move(options.output_factory)) {
    ScanOptions scan;
    scan.sacd_multichannel = settings_.sacd_multichannel;
    library_ = std::make_unique<Library>(options.data_folder / "library.db", options.data_folder / "cache", scan,
                                         options.watch_quiet_ms);

    queue_ = library_->store().queue();
    resume_index_ = std::min(settings_.queue_index, queue_.empty() ? size_t{0} : queue_.size() - 1);
    resume_position_ = resume_index_ == settings_.queue_index ? settings_.queue_position : 0.0;
    player_.set_queue(queue_items());
    player_.set_repeat(repeat_mode(settings_.repeat));

    register_handlers();
    begin_device_listing();
}

Controller::~Controller() {
    settings_.queue_index = resume_index_;
    settings_.queue_position = resume_position_;
    save_settings_quietly();
    player_.stop();
    if (listing_thread_.joinable()) listing_thread_.join();
}

void Controller::save_settings_quietly() {
    try {
        save_settings(settings_file_, settings_);
    } catch (const std::exception&) {
        // Losing a setting is no reason to interrupt what the user is doing.
    }
}

void Controller::set_window_placement(const WindowPlacement& placement) {
    settings_.window = placement;
}

void Controller::register_handlers() {
    handlers_ = {
        {"app.state",
         [this](const json&) {
             return json{{"queue", queue_json()},
                         {"status", status_json(player_.status())},
                         {"library", library_json(library_->status())},
                         {"folders", folders_json()},
                         {"playlists", playlists_json()},
                         {"devices", devices_json()},
                         {"sacdMultichannel", settings_.sacd_multichannel},
                         {"replayGain", settings_.replay_gain}};
         }},

        {"library.albums", [this](const json&) { return albums_json(library_->store().albums()); }},
        {"library.albumTracks",
         [this](const json& params) { return tracks_json(library_->store().album_tracks(params.at("id"))); }},
        {"library.artists",
         [this](const json&) {
             json out = json::array();
             for (const Artist& artist : library_->store().artists()) {
                 out.push_back({{"id", artist.id},
                                {"name", display_name(artist.name)},
                                {"albums", artist.album_count},
                                {"tracks", artist.track_count},
                                {"art", artist.artwork_id}});
             }
             return out;
         }},
        {"library.artist",
         [this](const json& params) {
             const ArtistId id = params.at("id");
             return json{{"albums", albums_json(library_->store().artist_albums(id))},
                         {"tracks", tracks_json(library_->store().artist_tracks(id))}};
         }},
        {"library.tracks", [this](const json&) { return tracks_json(library_->store().tracks()); }},
        {"library.folderTracks",
         [this](const json& params) { return tracks_json(library_->store().folder_tracks(params.at("id"))); }},
        {"library.search",
         [this](const json& params) {
             return tracks_json(library_->store().search(params.at("text").get<std::string>()));
         }},
        {"library.favorites", [this](const json&) { return tracks_json(library_->store().favorites()); }},
        {"library.setFavorite",
         [this](const json& params) {
             const TrackId id = params.at("id");
             const bool favorite = params.at("favorite");
             library_->store().set_favorite(id, favorite);
             for (Track& track : queue_) {
                 if (track.id == id) track.favorite = favorite;
             }
             return json{};
         }},
        {"library.folders", [this](const json&) { return folders_json(); }},
        {"library.addFolder",
         [this](const json& params) {
             library_->add_folder(path_from_utf8(params.at("path").get<std::string>()));
             return folders_json();
         }},
        {"library.removeFolder",
         [this](const json& params) {
             library_->remove_folder(path_from_utf8(params.at("path").get<std::string>()));
             return json{};
         }},
        {"library.rescan",
         [this](const json& params) {
             library_->rescan(params.value("force", false));
             return json{};
         }},
        {"library.setSacdMultichannel",
         [this](const json& params) {
             settings_.sacd_multichannel = params.at("enabled");
             save_settings_quietly();
             ScanOptions scan;
             scan.sacd_multichannel = settings_.sacd_multichannel;
             library_->set_options(scan);
             library_->rescan(true);  // discs already catalogued must be read again
             return json{};
         }},

        {"playlists.list", [this](const json&) { return playlists_json(); }},
        {"playlists.create",
         [this](const json& params) {
             const PlaylistId id = library_->store().create_playlist(params.at("name").get<std::string>());
             for (const TrackId track : params.value("tracks", std::vector<TrackId>{})) {
                 library_->store().add_to_playlist(id, track);
             }
             emit_("playlists", playlists_json());
             return json{{"id", id}};
         }},
        {"playlists.rename",
         [this](const json& params) {
             library_->store().rename_playlist(params.at("id"), params.at("name").get<std::string>());
             emit_("playlists", playlists_json());
             return json{};
         }},
        {"playlists.delete",
         [this](const json& params) {
             library_->store().delete_playlist(params.at("id"));
             emit_("playlists", playlists_json());
             return json{};
         }},
        {"playlists.tracks",
         [this](const json& params) { return tracks_json(library_->store().playlist_tracks(params.at("id"))); }},
        {"playlists.add",
         [this](const json& params) {
             const PlaylistId id = params.at("id");
             for (const TrackId track : params.at("tracks").get<std::vector<TrackId>>()) {
                 library_->store().add_to_playlist(id, track);
             }
             emit_("playlists", playlists_json());
             return json{};
         }},
        {"playlists.remove",
         [this](const json& params) {
             library_->store().remove_from_playlist(params.at("id"), params.at("index").get<size_t>());
             emit_("playlists", playlists_json());
             return json{};
         }},

        {"queue.get", [this](const json&) { return queue_json(); }},
        {"queue.play",
         [this](const json& params) {
             queue_ = tracks_from(params.at("tracks"));
             if (queue_.empty()) throw std::runtime_error("no hay pistas que reproducir");
             const size_t index = std::min(params.value("index", size_t{0}), queue_.size() - 1);
             player_.set_queue(queue_items());
             std::vector<TrackId> ids;
             for (const Track& track : queue_) ids.push_back(track.id);
             library_->store().set_queue(ids);
             resume_index_ = index;
             resume_position_ = 0.0;
             start(index, params.value("start", 0.0));
             emit_("queue", queue_json());
             return json{};
         }},
        {"queue.add",
         [this](const json& params) {
             const size_t current = current_index();
             std::vector<Track> added = tracks_from(params.at("tracks"));
             queue_.insert(queue_.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
             publish_queue(current);
             return json{};
         }},
        {"queue.playNext",
         [this](const json& params) {
             const size_t current = current_index();
             std::vector<Track> added = tracks_from(params.at("tracks"));
             const size_t at = queue_.empty() ? 0 : current + 1;
             queue_.insert(queue_.begin() + static_cast<ptrdiff_t>(at), std::make_move_iterator(added.begin()),
                           std::make_move_iterator(added.end()));
             publish_queue(current);
             return json{};
         }},
        {"queue.remove",
         [this](const json& params) {
             remove_from_queue(params.at("index"));
             return json{};
         }},
        {"queue.move",
         [this](const json& params) {
             move_in_queue(params.at("from"), params.at("to"));
             return json{};
         }},
        {"queue.shuffle",
         [this](const json&) {
             shuffle_queue();
             return json{};
         }},
        {"queue.clear",
         [this](const json&) {
             clear_queue();
             return json{};
         }},

        {"player.toggle",
         [this](const json&) {
             const PlayerState state = player_.status().state;
             if (state == PlayerState::Stopped) {
                 if (!queue_.empty()) start(resume_index_, resume_position_);
             } else {
                 player_.set_paused(state == PlayerState::Playing);
             }
             return json{};
         }},
        {"player.next",
         [this](const json&) {
             if (playing()) {
                 player_.next();
             } else if (resume_index_ + 1 < queue_.size()) {
                 ++resume_index_;
                 resume_position_ = 0.0;
                 last_status_ = nullptr;
             }
             return json{};
         }},
        {"player.previous",
         [this](const json&) {
             if (playing()) {
                 player_.previous();
             } else if (resume_position_ > 0.0 || resume_index_ == 0) {
                 resume_position_ = 0.0;
                 last_status_ = nullptr;
             } else {
                 --resume_index_;
                 last_status_ = nullptr;
             }
             return json{};
         }},
        {"player.seek",
         [this](const json& params) {
             const double seconds = params.at("seconds");
             if (playing()) {
                 player_.seek(seconds);
             } else {
                 resume_position_ = seconds;
                 last_status_ = nullptr;
             }
             return json{};
         }},
        {"player.jump",
         [this](const json& params) {
             start(params.at("index"), 0.0);
             return json{};
         }},
        {"player.setRepeat",
         [this](const json& params) {
             settings_.repeat = params.at("mode").get<std::string>();
             player_.set_repeat(repeat_mode(settings_.repeat));
             save_settings_quietly();
             last_status_ = nullptr;
             return json{};
         }},
        {"player.setVolume",
         [this](const json& params) {
             set_volume(params.at("db"));
             return json{};
         }},
        {"player.setMuted",
         [this](const json& params) {
             muted_ = params.at("muted");
             apply_volume();
             return json{};
         }},
        {"player.setReplayGain",
         [this](const json& params) {
             set_replay_gain(params.at("mode").get<std::string>());
             return json{};
         }},

        {"devices.list", [this](const json&) { return devices_json(); }},
        {"devices.refresh",
         [this](const json&) {
             // Probing loads every driver, and an ASIO driver in use may not survive a second copy.
             if (playing()) return json{{"busy", true}};
             begin_device_listing();
             return json{{"busy", false}};
         }},
        {"devices.select",
         [this](const json& params) {
             settings_.device = params.at("key").get<std::string>();
             save_settings_quietly();
             apply_output();
             emit_("devices", devices_json());
             return json{};
         }},
        {"devices.configure",
         [this](const json& params) {
             configure_device(params.at("key").get<std::string>(), params.at("settings"));
             return json{};
         }},
        {"devices.release",
         [this](const json&) {
             release_device();
             return json{};
         }},
    };
}

json Controller::handle(const std::string& method, const json& params) {
    const auto handler = handlers_.find(method);
    if (handler == handlers_.end()) throw std::runtime_error("unknown request: " + method);
    try {
        return handler->second(params);
    } catch (const json::exception& error) {
        throw std::runtime_error(method + ": " + error.what());
    }
}

std::vector<Track> Controller::tracks_from(const json& ids) {
    std::vector<Track> tracks;
    for (const TrackId id : ids.get<std::vector<TrackId>>()) {
        if (auto track = library_->store().track(id)) tracks.push_back(std::move(*track));
    }
    return tracks;
}

std::vector<QueueItem> Controller::queue_items() const {
    std::vector<QueueItem> items;
    items.reserve(queue_.size());
    const ReplayGainMode mode = replay_gain_mode(settings_.replay_gain);
    for (const Track& track : queue_) items.push_back(make_queue_item(track, mode));
    return items;
}

void Controller::publish_queue(size_t current) {
    if (playing()) {
        player_.update_queue(queue_items(), current);
    } else {
        player_.set_queue(queue_items());
        resume_index_ = current;
    }
    std::vector<TrackId> ids;
    ids.reserve(queue_.size());
    for (const Track& track : queue_) ids.push_back(track.id);
    library_->store().set_queue(ids);
    last_status_ = nullptr;
    emit_("queue", queue_json());
}

json Controller::queue_json() const {
    return {{"tracks", tracks_json(queue_)}, {"index", current_index()}};
}

bool Controller::playing() const {
    return player_.status().state != PlayerState::Stopped;
}

size_t Controller::current_index() const {
    const PlayerStatus status = player_.status();
    return status.state == PlayerState::Stopped ? resume_index_ : status.queue_index;
}

void Controller::start(size_t index, double seconds) {
    if (index >= queue_.size()) return;
    if (!devices_known_) {
        pending_start_ = {index, seconds};
        return;
    }
    if (!selected_device()) throw std::runtime_error("no hay ningún dispositivo de salida disponible");
    player_.play(index, seconds);
}

void Controller::remove_from_queue(size_t index) {
    if (index >= queue_.size()) return;
    const size_t current = current_index();
    const bool heard = playing() && index == current;
    queue_.erase(queue_.begin() + static_cast<ptrdiff_t>(index));

    if (heard) {
        // The position being heard has no meaning for the track that takes its place.
        player_.set_queue(queue_items());
        if (index < queue_.size()) player_.play(index);
        else player_.stop();
        publish_queue(std::min(index, queue_.empty() ? size_t{0} : queue_.size() - 1));
        return;
    }
    if (index == current) resume_position_ = 0.0;
    const size_t last = queue_.empty() ? 0 : queue_.size() - 1;
    publish_queue(std::min(index < current ? current - 1 : current, last));
}

void Controller::move_in_queue(size_t from, size_t to) {
    if (from >= queue_.size() || to >= queue_.size() || from == to) return;
    size_t current = current_index();
    if (from < to) {
        std::rotate(queue_.begin() + static_cast<ptrdiff_t>(from), queue_.begin() + static_cast<ptrdiff_t>(from) + 1,
                    queue_.begin() + static_cast<ptrdiff_t>(to) + 1);
    } else {
        std::rotate(queue_.begin() + static_cast<ptrdiff_t>(to), queue_.begin() + static_cast<ptrdiff_t>(from),
                    queue_.begin() + static_cast<ptrdiff_t>(from) + 1);
    }
    if (current == from) current = to;
    else if (from < current && current <= to) --current;
    else if (to <= current && current < from) ++current;
    publish_queue(current);
}

void Controller::shuffle_queue() {
    if (queue_.size() < 2) return;
    const size_t current = current_index();
    std::swap(queue_[0], queue_[current]);
    std::shuffle(queue_.begin() + 1, queue_.end(), std::mt19937(std::random_device{}()));
    publish_queue(0);
}

void Controller::clear_queue() {
    player_.stop();
    player_.wait_until_stopped(5000);
    queue_.clear();
    resume_position_ = 0.0;
    publish_queue(0);
}

void Controller::begin_device_listing() {
    if (listing_) return;
    if (listing_thread_.joinable()) listing_thread_.join();
    listing_ = true;
    listing_thread_ = std::thread([this] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<DeviceInfo> devices;
        try {
            devices = list_devices_();
        } catch (const std::exception&) {
            // No devices is what the page will show, which is also what happened.
        }
        if (SUCCEEDED(com)) CoUninitialize();
        std::lock_guard lock(listing_mutex_);
        listed_ = std::move(devices);
    });
}

void Controller::adopt_devices(std::vector<DeviceInfo> devices) {
    devices_ = std::move(devices);
    devices_known_ = true;

    // A first start goes to shared mode on the Windows default: it opens without taking the device from
    // other programs and follows the system volume. An output that went away is never replaced by
    // another on its own, because the music could come out of the wrong speakers at full level.
    if (settings_.device.empty()) {
        const auto usable = [](const DeviceInfo& device) { return device.error.empty() && !device.pcm_modes.empty(); };
        const auto best = std::find_if(devices_.begin(), devices_.end(), [&](const DeviceInfo& device) {
            return device.kind == OutputKind::WasapiShared && device.is_default && usable(device);
        });
        const auto any = std::find_if(devices_.begin(), devices_.end(), usable);
        if (best != devices_.end()) settings_.device = device_key(*best);
        else if (any != devices_.end()) settings_.device = device_key(*any);
        if (!settings_.device.empty()) save_settings_quietly();
    }
    if (!playing()) apply_output();
    emit_("devices", devices_json());

    if (pending_start_) {
        const auto [index, seconds] = *pending_start_;
        pending_start_.reset();
        try {
            start(index, seconds);
        } catch (const std::exception& error) {
            emit_("error", {{"message", error.what()}});
        }
    }
}

const DeviceInfo* Controller::selected_device() const {
    for (const DeviceInfo& device : devices_) {
        if (device_key(device) == settings_.device) return &device;
    }
    return nullptr;
}

DeviceSettings& Controller::settings_for(const std::string& key) {
    return settings_.devices[key];
}

void Controller::apply_output() {
    const DeviceInfo* device = selected_device();
    if (!device) return;

    const DeviceSettings& chosen = settings_for(settings_.device);
    OutputOptions options;
    options.first_channel = chosen.first_channel;
    options.period_ms = chosen.period_ms;
    options.buffer_frames = chosen.buffer_frames;
    PlaybackPolicy policy;
    policy.dsd = dsd_policy(chosen.dsd);
    policy.dop_confirmed = chosen.dop_confirmed;
    policy.dsd_to_pcm_gain_db = chosen.dsd_gain_db;

    // A stream keeps the device and rules it opened with, so a change needs a new one.
    const PlayerStatus status = player_.status();
    const bool active = status.state != PlayerState::Stopped;
    if (active) {
        player_.stop();
        player_.wait_until_stopped(5000);
    }
    player_.set_device(*device, options);
    player_.set_policy(policy);
    apply_volume();
    if (active) {
        player_.play(status.queue_index, status.position_seconds);
        player_.set_paused(status.state == PlayerState::Paused);
    }
}

void Controller::apply_volume() {
    const auto saved = settings_.devices.find(settings_.device);
    const double db = saved == settings_.devices.end() ? 0.0 : saved->second.volume_db;
    player_.set_volume(muted_ ? 0.0 : volume_gain(db));
    last_status_ = nullptr;
}

void Controller::set_volume(double db) {
    if (settings_.device.empty()) return;
    const PlayerStatus status = player_.status();
    if (status.stream_open && !status.plan.accepts_gain()) {
        throw std::runtime_error("con DSD nativo o DoP no hay volumen por software: usa el del DAC");
    }
    settings_for(settings_.device).volume_db = std::clamp(db, kMinVolumeDb, 0.0);
    muted_ = false;
    apply_volume();
    volume_ticks_to_save_ = kVolumeSaveTicks;
}

void Controller::set_replay_gain(const std::string& mode) {
    settings_.replay_gain = mode;
    save_settings_quietly();
    if (playing()) {
        // The ring holds up to a minute and a half at the old gain.
        player_.update_queue(queue_items(), current_index());
        player_.reload();
    } else {
        player_.set_queue(queue_items());
    }
}

// Closes the stream so other programs can open the device. Only play takes it back, from the same place.
void Controller::release_device() {
    const PlayerStatus status = player_.status();
    if (status.state == PlayerState::Stopped) return;
    player_.stop();
    player_.wait_until_stopped(5000);
    resume_index_ = status.queue_index;
    resume_position_ = status.position_seconds;
    was_active_ = false;  // otherwise tick() takes this stop for the end of the queue
}

void Controller::configure_device(const std::string& key, const json& changes) {
    DeviceSettings& device = settings_for(key);
    const DeviceSettings before = device;
    device.dsd = changes.value("dsd", device.dsd);
    device.dop_confirmed = changes.value("dopConfirmed", device.dop_confirmed);
    device.dsd_gain_db = changes.value("dsdGainDb", device.dsd_gain_db);
    device.first_channel = changes.value("firstChannel", device.first_channel);
    device.period_ms = changes.value("periodMs", device.period_ms);
    device.buffer_frames = changes.value("bufferFrames", device.buffer_frames);
    if (device == before) return;
    save_settings_quietly();
    if (key == settings_.device) apply_output();
    emit_("devices", devices_json());
}

json Controller::devices_json() {
    json list = json::array();
    for (const DeviceInfo& device : devices_) {
        const std::string key = device_key(device);
        json rates = json::array();
        for (const PcmMode& mode : device.pcm_modes) rates.push_back(mode.rate);
        const auto saved = settings_.devices.find(key);
        list.push_back({{"key", key},
                        {"kind", kind_key(device.kind)},
                        {"name", device.name},
                        {"channels", device.channels},
                        {"rates", std::move(rates)},
                        {"dsdRates", device.dsd_rates},
                        {"outputs", device.channel_names},
                        {"gainDb", device.hardware_gain_db},
                        {"bufferMin", device.buffer_min},
                        {"bufferMax", device.buffer_max},
                        {"bufferPreferred", device.buffer_preferred},
                        {"error", device.error},
                        {"settings", device_settings_json(saved == settings_.devices.end() ? DeviceSettings{} : saved->second)}});
    }
    return {{"known", devices_known_}, {"selected", settings_.device}, {"list", std::move(list)}};
}

json Controller::status_json(const PlayerStatus& status) const {
    const bool stopped = status.state == PlayerState::Stopped;
    const size_t index = stopped ? resume_index_ : status.queue_index;
    const DeviceInfo* device = selected_device();
    const auto saved = settings_.devices.find(settings_.device);
    json out = {{"state", state_name(status.state)},
                {"index", index},
                {"trackId", index < queue_.size() ? queue_[index].id : 0},
                {"position", stopped ? resume_position_ : status.position_seconds},
                {"duration", !stopped && status.duration_seconds > 0.0 ? status.duration_seconds
                             : index < queue_.size() && queue_[index].format.rate ? queue_[index].format.seconds() : 0.0},
                {"repeat", settings_.repeat},
                {"volumeDb", saved == settings_.devices.end() ? 0.0 : saved->second.volume_db},
                {"volumeFloorDb", kMinVolumeDb},
                {"muted", muted_},
                {"trackGainDb", gain_db(status.track_gain)},
                {"bitPerfect", status.bit_perfect},
                // A shared stream keeps nobody out, so there is nothing to release.
                {"deviceHeld", status.stream_open && device && device->kind != OutputKind::WasapiShared},
                {"windowsGainDb", status.hardware_gain_db},
                {"buffer", status.ring_capacity ? static_cast<double>(status.buffered_bytes) / status.ring_capacity : 0.0},
                {"underruns", status.underruns},
                {"late", status.output.late_callbacks},
                {"clipped", status.clipped_samples},
                {"reopened", status.reopened},
                {"error", status.error},
                {"plan", nullptr}};
    if (status.stream_open) out["plan"] = plan_json(status.plan);
    return out;
}

json Controller::library_json(const LibraryStatus& status) const {
    return {{"scanning", status.scanning},
            {"done", status.done},
            {"total", status.total},
            {"current", status.current},
            {"revision", status.revision},
            {"error", status.error},
            {"warnings", status.warnings}};
}

json Controller::folders_json() {
    json out = json::array();
    for (const Folder& folder : library_->store().folders()) {
        out.push_back({{"id", folder.id}, {"path", path_to_utf8(folder.path)}, {"name", path_to_utf8(folder.path.filename())}});
    }
    return out;
}

json Controller::playlists_json() {
    json out = json::array();
    for (const Playlist& playlist : library_->store().playlists()) {
        out.push_back({{"id", playlist.id}, {"name", playlist.name}, {"tracks", playlist.track_count}});
    }
    return out;
}

void Controller::tick() {
    {
        std::unique_lock lock(listing_mutex_);
        if (listed_) {
            std::vector<DeviceInfo> devices = std::move(*listed_);
            listed_.reset();
            lock.unlock();
            listing_ = false;
            adopt_devices(std::move(devices));
        }
    }

    const PlayerStatus status = player_.status();
    const bool active = status.state != PlayerState::Stopped;
    if (active) {
        resume_index_ = status.queue_index;
        resume_position_ = status.position_seconds;
    } else if (was_active_ && status.error.empty()) {
        // Reached the end of the queue. After a failure it stays on the track that failed.
        resume_index_ = 0;
        resume_position_ = 0.0;
    }
    if (active != was_active_ || (active && status.queue_index != settings_.queue_index)) {
        settings_.queue_index = resume_index_;
        settings_.queue_position = resume_position_;
        save_settings_quietly();
    }
    was_active_ = active;

    if (volume_ticks_to_save_ > 0 && --volume_ticks_to_save_ == 0) save_settings_quietly();

    json current = status_json(status);
    if (current != last_status_) {
        emit_("status", current);
        last_status_ = std::move(current);
    }

    json library = library_json(library_->status());
    if (library != last_library_) {
        emit_("library", library);
        last_library_ = std::move(library);
    }
}

std::optional<ArtworkResponse> Controller::artwork(ArtworkId id, bool thumbnail) {
    const std::optional<ArtworkFile> file = library_->store().artwork(id);
    if (!file) return std::nullopt;
    if (thumbnail) {
        const std::filesystem::path path = library_->artwork().thumbnail_path(*file);
        return ArtworkResponse{path, path == library_->artwork().original_path(*file) ? file->mime : "image/jpeg"};
    }
    return ArtworkResponse{library_->artwork().original_path(*file), file->mime};
}

std::string Controller::now_playing_title() const {
    const PlayerStatus status = player_.status();
    if (status.state == PlayerState::Stopped || status.queue_index >= queue_.size()) return {};
    const Track& track = queue_[status.queue_index];
    return display_name(track.title) + " - " + display_name(track.artist);
}

}  // namespace arpegio
