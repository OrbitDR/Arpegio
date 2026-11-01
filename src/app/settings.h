// What the application remembers between runs, in a JSON file next to the catalogue.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace arpegio {

struct DeviceSettings {
    std::string dsd = "auto";  // auto, native, dop or pcm
    bool dop_confirmed = false;
    double dsd_gain_db = 3.0;
    int first_channel = 0;
    uint32_t period_ms = 0;  // WASAPI exclusive, 0 keeps the default
    int buffer_frames = 0;   // ASIO, 0 keeps the driver's preferred size
    double volume_db = 0.0;  // software volume; 0 is untouched and each device keeps its own

    bool operator==(const DeviceSettings&) const = default;
};

struct WindowPlacement {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool maximized = false;
};

struct Settings {
    std::string device;  // key of the selected output, empty until one is chosen
    std::map<std::string, DeviceSettings> devices;
    bool sacd_multichannel = false;
    std::string repeat = "off";  // off, queue or track
    std::string replay_gain = "off";  // off, track or album
    size_t queue_index = 0;
    double queue_position = 0.0;
    std::optional<WindowPlacement> window;
};

// A file that is missing or damaged gives the defaults: settings never stop the program from starting.
Settings load_settings(const std::filesystem::path& file);

// Writes through a temporary file, so a crash cannot leave half a file. Throws std::runtime_error.
void save_settings(const std::filesystem::path& file, const Settings& settings);

}  // namespace arpegio
