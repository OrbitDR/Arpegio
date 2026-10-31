// Watches one directory tree and calls back once changes have been quiet for a while, so copying an
// album produces one rescan instead of hundreds and no half-written file gets read.
#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <thread>

namespace arpegio {

class FolderWatcher {
public:
    // Throws std::runtime_error when the folder cannot be watched. The callback runs on the watcher's
    // thread, so it should only queue work.
    FolderWatcher(const std::filesystem::path& folder, std::function<void()> on_change, uint32_t quiet_ms);
    ~FolderWatcher();
    FolderWatcher(const FolderWatcher&) = delete;
    FolderWatcher& operator=(const FolderWatcher&) = delete;

private:
    void run();

    std::function<void()> on_change_;
    uint32_t quiet_ms_;
    HANDLE directory_ = INVALID_HANDLE_VALUE;
    HANDLE stop_ = nullptr;
    HANDLE changed_ = nullptr;
    std::thread thread_;
};

}  // namespace arpegio
