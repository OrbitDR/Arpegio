// The application behind the interface. The page sends named requests with JSON parameters and gets
// JSON back, and tick() pushes what changed as events. Nothing here knows about windows or WebView2, so
// the whole application can run in a test with a fake output. Everything belongs to the thread that
// created the object, except the device listing, which needs a multithreaded apartment of its own.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/settings.h"
#include "engine/player.h"
#include "library/library.h"

namespace arpegio {

struct ArtworkResponse {
    std::filesystem::path file;
    std::string mime;
};

class Controller {
public:
    using Emit = std::function<void(const std::string& event, const nlohmann::json& data)>;
    using ListDevices = std::function<std::vector<DeviceInfo>()>;

    struct Options {
        std::filesystem::path data_folder;
        Emit emit;
        OutputFactory output_factory = make_output;
        ListDevices list_devices;     // empty lists the real ones; runs on its own MTA thread
        uint32_t watch_quiet_ms = 2000;
    };

    explicit Controller(Options options);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    // Runs one request. Throws std::runtime_error with a message meant for the page.
    nlohmann::json handle(const std::string& method, const nlohmann::json& params);

    // Call a few times per second. Emits the events `status`, `library`, `devices` and `queue`.
    void tick();

    std::optional<ArtworkResponse> artwork(ArtworkId id, bool thumbnail);

    const Settings& settings() const { return settings_; }
    void set_window_placement(const WindowPlacement& placement);

    std::string now_playing_title() const;  // for the window title, empty when nothing plays

private:
    using Handler = std::function<nlohmann::json(const nlohmann::json&)>;

    void register_handlers();
    void save_settings_quietly();

    // Queue
    std::vector<Track> tracks_from(const nlohmann::json& ids);
    std::vector<QueueItem> queue_items() const;
    void publish_queue(size_t current);
    nlohmann::json queue_json() const;
    size_t current_index() const;
    bool playing() const;
    void start(size_t index, double seconds);
    void remove_from_queue(size_t index);
    void move_in_queue(size_t from, size_t to);
    void shuffle_queue();
    void clear_queue();

    // Output
    void begin_device_listing();
    void adopt_devices(std::vector<DeviceInfo> devices);
    const DeviceInfo* selected_device() const;
    void apply_output();
    void release_device();
    DeviceSettings& settings_for(const std::string& key);
    nlohmann::json devices_json();
    void configure_device(const std::string& key, const nlohmann::json& changes);

    nlohmann::json status_json(const PlayerStatus& status) const;
    nlohmann::json library_json(const LibraryStatus& status) const;
    nlohmann::json folders_json();
    nlohmann::json playlists_json();

    std::filesystem::path settings_file_;
    Emit emit_;
    ListDevices list_devices_;
    Settings settings_;
    std::unique_ptr<Library> library_;
    Player player_;
    std::unordered_map<std::string, Handler> handlers_;

    std::vector<Track> queue_;
    size_t resume_index_ = 0;  // where play starts when nothing is playing
    double resume_position_ = 0.0;
    std::optional<std::pair<size_t, double>> pending_start_;  // asked for before the devices were known
    bool was_active_ = false;
    nlohmann::json last_status_;
    nlohmann::json last_library_;

    std::vector<DeviceInfo> devices_;
    bool devices_known_ = false;
    std::mutex listing_mutex_;
    std::thread listing_thread_;
    std::optional<std::vector<DeviceInfo>> listed_;  // guarded by listing_mutex_
    bool listing_ = false;
};

}  // namespace arpegio
