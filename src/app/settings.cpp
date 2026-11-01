#include "app/settings.h"

#include <windows.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

using nlohmann::json;

namespace arpegio {

namespace {

DeviceSettings read_device(const json& node) {
    const DeviceSettings defaults;
    DeviceSettings device;
    device.dsd = node.value("dsd", defaults.dsd);
    device.dop_confirmed = node.value("dopConfirmed", defaults.dop_confirmed);
    device.dsd_gain_db = node.value("dsdGainDb", defaults.dsd_gain_db);
    device.first_channel = node.value("firstChannel", defaults.first_channel);
    device.period_ms = node.value("periodMs", defaults.period_ms);
    device.buffer_frames = node.value("bufferFrames", defaults.buffer_frames);
    device.volume_db = node.value("volumeDb", defaults.volume_db);
    return device;
}

json write_device(const DeviceSettings& device) {
    return {{"dsd", device.dsd},
            {"dopConfirmed", device.dop_confirmed},
            {"dsdGainDb", device.dsd_gain_db},
            {"firstChannel", device.first_channel},
            {"periodMs", device.period_ms},
            {"bufferFrames", device.buffer_frames},
            {"volumeDb", device.volume_db}};
}

}  // namespace

Settings load_settings(const std::filesystem::path& file) {
    Settings settings;
    std::ifstream input(file, std::ios::binary);
    if (!input) return settings;
    try {
        const json root = json::parse(input);
        settings.device = root.value("device", settings.device);
        settings.sacd_multichannel = root.value("sacdMultichannel", settings.sacd_multichannel);
        settings.repeat = root.value("repeat", settings.repeat);
        settings.replay_gain = root.value("replayGain", settings.replay_gain);
        settings.resampler = root.value("resampler", settings.resampler);
        settings.queue_index = root.value("queueIndex", settings.queue_index);
        settings.queue_position = root.value("queuePosition", settings.queue_position);
        if (const auto devices = root.find("devices"); devices != root.end()) {
            for (const auto& [key, node] : devices->items()) settings.devices[key] = read_device(node);
        }
        if (const auto window = root.find("window"); window != root.end()) {
            WindowPlacement placement;
            placement.x = window->value("x", 0);
            placement.y = window->value("y", 0);
            placement.width = window->value("width", 0);
            placement.height = window->value("height", 0);
            placement.maximized = window->value("maximized", false);
            settings.window = placement;
        }
    } catch (const json::exception&) {
        return {};
    }
    return settings;
}

void save_settings(const std::filesystem::path& file, const Settings& settings) {
    json root = {{"device", settings.device},
                 {"sacdMultichannel", settings.sacd_multichannel},
                 {"repeat", settings.repeat},
                 {"replayGain", settings.replay_gain},
                 {"resampler", settings.resampler},
                 {"queueIndex", settings.queue_index},
                 {"queuePosition", settings.queue_position},
                 {"devices", json::object()}};
    for (const auto& [key, device] : settings.devices) root["devices"][key] = write_device(device);
    if (settings.window) {
        const WindowPlacement& window = *settings.window;
        root["window"] = {{"x", window.x},
                          {"y", window.y},
                          {"width", window.width},
                          {"height", window.height},
                          {"maximized", window.maximized}};
    }

    std::filesystem::path temporary = file;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << root.dump(2, ' ', false, json::error_handler_t::replace);
        if (!output) throw std::runtime_error("cannot write the settings");
    }
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("cannot replace the settings file");
    }
}

}  // namespace arpegio
