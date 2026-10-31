#include <windows.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/controller.h"
#include "app/settings.h"
#include "library/paths.h"
#include "test_support.h"

using namespace arpegio;
using nlohmann::json;

namespace {

// Takes samples as fast as it can. Nothing here touches a real device.
class SilentOutput final : public Output {
public:
    explicit SilentOutput(std::atomic<int>* opened) : opened_(opened) {}
    ~SilentOutput() override { close(); }

    DeviceFormat open(const DeviceFormat& requested, FrameSource& source) override {
        format_ = requested;
        source_ = &source;
        ++*opened_;
        return requested;
    }

    void start() override {
        running_ = true;
        thread_ = std::thread([this] {
            constexpr size_t kFrames = 4096;
            std::vector<uint8_t> block(kFrames * format_.frame_bytes());
            while (running_) {
                source_->pull(block.data(), kFrames);
                Sleep(1);
            }
        });
    }

    void stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    void close() override { stop(); }
    bool needs_reopen() const override { return false; }
    std::string fault() const override { return {}; }
    OutputStats stats() const override { return {}; }

private:
    std::atomic<int>* opened_;
    DeviceFormat format_{};
    FrameSource* source_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

DeviceInfo fake_device(OutputKind kind, const wchar_t* id, const char* name, bool is_default = false) {
    DeviceInfo device;
    device.kind = kind;
    device.id = id;
    device.name = name;
    device.channels = 2;
    device.is_default = is_default;
    for (uint32_t rate : {44100u, 48000u, 96000u}) device.pcm_modes.push_back({rate, WireEncoding::S32, 32});
    return device;
}

// A silent 16-bit stereo WAV of `seconds`.
void write_wav(const std::filesystem::path& path, double seconds) {
    constexpr uint32_t kRate = 44100;
    const uint32_t data_bytes = static_cast<uint32_t>(seconds * kRate) * 4;
    std::ofstream file(path, std::ios::binary);
    const auto u32 = [&file](uint32_t value) { file.write(reinterpret_cast<const char*>(&value), 4); };
    const auto u16 = [&file](uint16_t value) { file.write(reinterpret_cast<const char*>(&value), 2); };
    file.write("RIFF", 4);
    u32(36 + data_bytes);
    file.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(kRate);
    u32(kRate * 4);
    u16(4);
    u16(16);
    file.write("data", 4);
    u32(data_bytes);
    const std::vector<char> silence(data_bytes, 0);
    file.write(silence.data(), silence.size());
}

struct Harness {
    std::filesystem::path root;
    std::filesystem::path data;
    std::filesystem::path music;
    std::atomic<int> opened{0};
    std::map<std::string, json> last_event;
    std::vector<std::string> events;
    std::unique_ptr<Controller> controller;

    Harness() {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        static std::atomic<int> counter{0};
        root = std::filesystem::path(temp) /
               (L"arpegio-controller-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(counter++));
        data = root / L"data";
        music = root / L"música";
        std::filesystem::create_directories(data);
        std::filesystem::create_directories(music);
    }

    ~Harness() {
        controller.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    void open() {
        Controller::Options options;
        options.data_folder = data;
        options.emit = [this](const std::string& event, const json& payload) {
            events.push_back(event);
            last_event[event] = payload;
        };
        options.output_factory = [this](const DeviceInfo&, const OutputOptions&) -> std::unique_ptr<Output> {
            return std::make_unique<SilentOutput>(&opened);
        };
        options.list_devices = [] {
            return std::vector<DeviceInfo>{
                fake_device(OutputKind::Asio, L"{asio}", "asio device"),
                fake_device(OutputKind::WasapiExclusive, L"{speakers}", "speakers"),
                fake_device(OutputKind::WasapiShared, L"{other}", "other"),
                fake_device(OutputKind::WasapiShared, L"{speakers}", "speakers", true),
            };
        };
        options.watch_quiet_ms = 50;
        controller = std::make_unique<Controller>(std::move(options));
    }

    json call(const std::string& method, json params = json::object()) { return controller->handle(method, params); }

    template <typename Condition>
    bool until(Condition condition, int timeout_ms = 15000) {
        for (int waited = 0; waited < timeout_ms; waited += 5) {
            controller->tick();
            if (condition()) return true;
            Sleep(5);
        }
        return false;
    }

    bool devices_ready() {
        return until([this] { return last_event.contains("devices") && last_event["devices"]["known"].get<bool>(); });
    }

    // Three tracks in one folder, catalogued and ready to play.
    std::vector<int64_t> library_with_tracks(double seconds = 30.0) {
        for (int i = 1; i <= 3; ++i) write_wav(music / (L"pista " + std::to_wstring(i) + L".wav"), seconds);
        open();
        CHECK(devices_ready());
        call("library.addFolder", {{"path", path_to_utf8(music)}});
        CHECK(until([this] { return !call("library.tracks").empty(); }));
        std::vector<int64_t> ids;
        for (const json& track : call("library.tracks")) ids.push_back(track.at("id"));
        CHECK(ids.size() == 3);
        return ids;
    }

    json status() {
        controller->tick();
        return call("app.state").at("status");
    }

    bool until_state(const std::string& state) {
        return until([&] { return status().at("state") == state; });
    }
};

std::vector<int64_t> queued_ids(const json& queue) {
    std::vector<int64_t> ids;
    for (const json& track : queue.at("tracks")) ids.push_back(track.at("id"));
    return ids;
}

}  // namespace

TEST(controller_picks_the_shared_default_on_a_first_start) {
    Harness h;
    h.open();
    CHECK(h.devices_ready());
    CHECK(h.last_event["devices"]["selected"] == "shared:{speakers}");
    CHECK(h.last_event["devices"]["list"].size() == 4);
}

TEST(controller_keeps_the_chosen_output_between_runs) {
    Harness h;
    h.open();
    CHECK(h.devices_ready());
    h.call("devices.select", {{"key", "asio:{asio}"}});
    h.call("devices.configure", {{"key", "asio:{asio}"}, {"settings", {{"dsd", "pcm"}, {"dopConfirmed", true}, {"bufferFrames", 512}}}});
    h.controller.reset();

    h.last_event.clear();
    h.open();
    CHECK(h.devices_ready());
    const json devices = h.last_event["devices"];
    CHECK(devices["selected"] == "asio:{asio}");
    bool found = false;
    for (const json& device : devices["list"]) {
        if (device["key"] != "asio:{asio}") continue;
        found = true;
        CHECK(device["settings"]["dsd"] == "pcm");
        CHECK(device["settings"]["dopConfirmed"] == true);
        CHECK(device["settings"]["bufferFrames"] == 512);
    }
    CHECK(found);
}

TEST(controller_does_not_jump_to_another_output_when_the_chosen_one_is_gone) {
    Harness h;
    Settings settings;
    settings.device = "asio:{unplugged}";
    save_settings(h.data / "settings.json", settings);
    const auto ids = h.library_with_tracks();

    bool refused = false;
    try {
        h.call("queue.play", {{"tracks", ids}});
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);
    CHECK(h.opened == 0);
    CHECK(h.last_event["devices"]["selected"] == "asio:{unplugged}");
}

TEST(controller_plays_a_queue_and_reports_the_track) {
    Harness h;
    const auto ids = h.library_with_tracks();
    h.call("queue.play", {{"tracks", ids}, {"index", 1}});
    CHECK(h.until_state("playing"));
    const json status = h.status();
    CHECK(status.at("index") == 1);
    CHECK(status.at("trackId") == ids[1]);
    CHECK(status.at("duration").get<double>() > 29.0);
    CHECK(h.last_event["queue"]["index"] == 1);

    h.until([&] { return !h.status().at("plan").is_null(); });
    const json plan = h.status().at("plan");
    CHECK(plan.at("path") == "pcm");
    CHECK(plan.at("rate") == 44100);
}

TEST(controller_toggle_pauses_and_resumes) {
    Harness h;
    const auto ids = h.library_with_tracks();
    h.call("queue.play", {{"tracks", ids}});
    CHECK(h.until_state("playing"));
    h.call("player.toggle");
    CHECK(h.until_state("paused"));
    h.call("player.toggle");
    CHECK(h.until_state("playing"));
}

TEST(controller_restores_the_queue_and_the_position_at_the_next_start) {
    Harness h;
    const auto ids = h.library_with_tracks();
    h.call("queue.play", {{"tracks", ids}, {"index", 2}});
    CHECK(h.until_state("playing"));
    h.until([&] { return h.status().at("position").get<double>() > 0.05; });
    h.call("player.toggle");
    CHECK(h.until_state("paused"));
    const double position = h.status().at("position");
    h.controller.reset();

    h.last_event.clear();
    h.open();
    CHECK(queued_ids(h.call("queue.get")) == ids);
    const json status = h.status();
    CHECK(status.at("state") == "stopped");
    CHECK(status.at("index") == 2);
    CHECK_NEAR(status.at("position").get<double>(), position, 0.5);
    CHECK(h.opened == 1);

    // Nothing starts by itself, and play continues from there.
    CHECK(h.devices_ready());
    h.call("player.toggle");
    CHECK(h.until_state("playing"));
    CHECK(h.status().at("index") == 2);
    CHECK(h.status().at("position").get<double>() >= position);
}

TEST(controller_ignores_a_saved_position_past_the_end_of_the_queue) {
    Harness h;
    Settings settings;
    settings.queue_index = 40;
    settings.queue_position = 12.0;
    save_settings(h.data / "settings.json", settings);
    h.open();
    const json status = h.status();
    CHECK(status.at("index") == 0);
    CHECK(status.at("position") == 0.0);
}

TEST(controller_starts_a_request_made_before_the_devices_are_known) {
    Harness h;
    for (int i = 1; i <= 3; ++i) write_wav(h.music / (L"pista " + std::to_wstring(i) + L".wav"), 30.0);
    h.open();
    CHECK(h.devices_ready());
    h.call("library.addFolder", {{"path", path_to_utf8(h.music)}});
    CHECK(h.until([&] { return !h.call("library.tracks").empty(); }));
    h.call("queue.play", {{"tracks", {h.call("library.tracks")[0]["id"]}}});
    CHECK(h.until_state("playing"));
    h.controller.reset();

    // The queue comes back before the listing finishes, as it does on every start.
    h.last_event.clear();
    h.open();
    h.call("player.toggle");
    CHECK(h.until_state("playing"));
}

TEST(controller_edits_the_queue_around_the_current_track) {
    Harness h;
    const auto ids = h.library_with_tracks(120.0);
    h.call("queue.play", {{"tracks", {ids[0], ids[1]}}});
    CHECK(h.until([&] { return h.status().at("position").get<double>() > 0.05; }));
    CHECK(h.opened == 1);

    h.call("queue.add", {{"tracks", {ids[2]}}});
    CHECK(queued_ids(h.call("queue.get")) == (std::vector<int64_t>{ids[0], ids[1], ids[2]}));
    h.call("queue.playNext", {{"tracks", {ids[2]}}});
    CHECK(queued_ids(h.call("queue.get")) == (std::vector<int64_t>{ids[0], ids[2], ids[1], ids[2]}));
    h.call("queue.move", {{"from", 3}, {"to", 0}});
    CHECK(queued_ids(h.call("queue.get")) == (std::vector<int64_t>{ids[2], ids[0], ids[2], ids[1]}));
    CHECK(h.status().at("index") == 1);
    CHECK(h.status().at("trackId") == ids[0]);

    h.call("queue.remove", {{"index", 0}});
    CHECK(queued_ids(h.call("queue.get")) == (std::vector<int64_t>{ids[0], ids[2], ids[1]}));
    CHECK(h.status().at("index") == 0);
    CHECK(h.status().at("trackId") == ids[0]);
    CHECK(h.opened == 1);
}

TEST(controller_removing_the_track_being_heard_moves_to_the_next_one) {
    Harness h;
    const auto ids = h.library_with_tracks(120.0);
    h.call("queue.play", {{"tracks", ids}});
    CHECK(h.until_state("playing"));
    h.until([&] { return h.status().at("position").get<double>() > 0.05; });

    h.call("queue.remove", {{"index", 0}});
    CHECK(h.until([&] { return h.status().at("trackId") == ids[1]; }));
    CHECK(h.status().at("index") == 0);
    // It starts from the beginning: the position belonged to the removed track.
    h.call("player.toggle");
    CHECK(h.until_state("paused"));
    CHECK(h.status().at("position").get<double>() < 60.0);
}

TEST(controller_shuffle_keeps_the_current_track_in_front) {
    Harness h;
    const auto ids = h.library_with_tracks(120.0);
    h.call("queue.play", {{"tracks", ids}, {"index", 2}});
    CHECK(h.until_state("playing"));
    h.call("queue.shuffle");
    const auto queued = queued_ids(h.call("queue.get"));
    CHECK(queued.size() == 3);
    CHECK(queued[0] == ids[2]);
    CHECK(h.status().at("index") == 0);
}

TEST(controller_clear_stops_everything) {
    Harness h;
    const auto ids = h.library_with_tracks();
    h.call("queue.play", {{"tracks", ids}});
    CHECK(h.until_state("playing"));
    h.call("queue.clear");
    CHECK(h.until_state("stopped"));
    CHECK(h.call("queue.get").at("tracks").empty());
    CHECK(h.status().at("trackId") == 0);
}

TEST(controller_changing_the_output_settings_restarts_the_stream_where_it_was) {
    Harness h;
    const auto ids = h.library_with_tracks(120.0);
    h.call("queue.play", {{"tracks", ids}, {"index", 1}});
    CHECK(h.until_state("playing"));
    h.until([&] { return h.status().at("position").get<double>() > 0.05; });
    CHECK(h.opened == 1);

    h.call("devices.configure", {{"key", "shared:{speakers}"}, {"settings", {{"periodMs", 20}}}});
    CHECK(h.until([&] { return h.opened == 2; }));
    CHECK(h.status().at("index") == 1);

    // The same values change nothing, so the music is left alone.
    h.call("devices.configure", {{"key", "shared:{speakers}"}, {"settings", {{"periodMs", 20}}}});
    Sleep(30);
    CHECK(h.opened == 2);
}

TEST(controller_does_not_probe_devices_during_playback) {
    Harness h;
    const auto ids = h.library_with_tracks(120.0);
    h.call("queue.play", {{"tracks", ids}});
    CHECK(h.until_state("playing"));
    CHECK(h.call("devices.refresh").at("busy") == true);
}

TEST(controller_keeps_favorites_and_playlists) {
    Harness h;
    const auto ids = h.library_with_tracks();
    h.call("library.setFavorite", {{"id", ids[1]}, {"favorite", true}});
    CHECK(h.call("library.favorites").size() == 1);

    const int64_t playlist = h.call("playlists.create", {{"name", "Noche"}, {"tracks", {ids[0], ids[2]}}}).at("id");
    CHECK(h.last_event["playlists"][0]["name"] == "Noche");
    CHECK(h.last_event["playlists"][0]["tracks"] == 2);
    h.call("playlists.rename", {{"id", playlist}, {"name", "Tarde"}});
    CHECK(h.last_event["playlists"][0]["name"] == "Tarde");
    h.call("playlists.remove", {{"id", playlist}, {"index", 0}});
    const json tracks = h.call("playlists.tracks", {{"id", playlist}});
    CHECK(tracks.size() == 1);
    CHECK(tracks[0]["id"] == ids[2]);
    h.call("playlists.delete", {{"id", playlist}});
    CHECK(h.last_event["playlists"].empty());
}

TEST(controller_describes_albums_with_their_best_format) {
    Harness h;
    h.library_with_tracks();
    const json albums = h.call("library.albums");
    CHECK(albums.size() == 1);
    CHECK(albums[0]["tracks"] == 3);
    CHECK_NEAR(albums[0]["seconds"].get<double>(), 90.0, 0.1);
    CHECK(albums[0]["rate"] == 44100);
    CHECK(albums[0]["bits"] == 16);
    CHECK(albums[0]["dsd"] == false);
    CHECK(albums[0]["codec"] == "WAV");
    CHECK(albums[0]["added"].get<double>() > 0.0);
}

TEST(controller_reports_bad_requests_as_errors) {
    Harness h;
    h.open();
    bool unknown = false;
    bool malformed = false;
    try {
        h.call("library.noSuchThing");
    } catch (const std::runtime_error&) {
        unknown = true;
    }
    try {
        h.call("library.albumTracks", {{"id", "not a number"}});
    } catch (const std::runtime_error&) {
        malformed = true;
    }
    CHECK(unknown);
    CHECK(malformed);
}

TEST(controller_events_only_when_something_changed) {
    Harness h;
    h.open();
    CHECK(h.devices_ready());
    h.until([&] { return h.last_event.contains("status"); });
    h.events.clear();
    for (int i = 0; i < 20; ++i) h.controller->tick();
    CHECK(h.events.empty());
}

TEST(settings_survive_a_damaged_file) {
    Harness h;
    {
        std::ofstream file(h.data / "settings.json", std::ios::binary);
        file << "{ this is not json";
    }
    const Settings settings = load_settings(h.data / "settings.json");
    CHECK(settings.device.empty());
    CHECK(settings.repeat == "off");
}

TEST(settings_round_trip) {
    Harness h;
    Settings settings;
    settings.device = "asio:{x}";
    settings.devices["asio:{x}"].dop_confirmed = true;
    settings.devices["asio:{x}"].dsd_gain_db = 1.5;
    settings.sacd_multichannel = true;
    settings.repeat = "track";
    settings.queue_index = 7;
    settings.queue_position = 12.5;
    settings.window = WindowPlacement{10, 20, 1000, 800, true};
    save_settings(h.data / "settings.json", settings);

    const Settings loaded = load_settings(h.data / "settings.json");
    CHECK(loaded.device == settings.device);
    CHECK(loaded.devices.at("asio:{x}") == settings.devices.at("asio:{x}"));
    CHECK(loaded.sacd_multichannel);
    CHECK(loaded.repeat == "track");
    CHECK(loaded.queue_index == 7);
    CHECK_NEAR(loaded.queue_position, 12.5, 1e-9);
    CHECK(loaded.window && loaded.window->maximized && loaded.window->width == 1000);
}
