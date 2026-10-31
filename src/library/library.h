// The library as the application sees it: a catalogue to read from, plus a background thread that
// keeps it up to date with the watched folders. Each thread has its own database connection, so
// store() stays usable while a scan runs.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "library/artwork.h"
#include "library/folder_watcher.h"
#include "library/library_store.h"
#include "library/scanner.h"

namespace arpegio {

struct LibraryStatus {
    bool scanning = false;
    uint64_t done = 0;
    uint64_t total = 0;
    std::string current;
    uint64_t revision = 0;              // grows every time the catalogue may have changed
    std::string error;                  // of the last job that failed, empty after one that worked
    std::vector<std::string> warnings;  // of the last scan
};

class Library {
public:
    // Rescans every stored folder in the background, which also catches changes made while closed.
    Library(const std::filesystem::path& database, const std::filesystem::path& cache, ScanOptions options = {},
            uint32_t quiet_ms = 2000);
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    // For the thread that owns this object.
    LibraryStore& store() { return store_; }
    const ArtworkCache& artwork() const { return artwork_; }

    // Throws std::runtime_error when the folder does not exist or overlaps one already in the library:
    // nested folders would list the same files twice.
    void add_folder(const std::filesystem::path& folder);
    void remove_folder(const std::filesystem::path& folder);
    void rescan(bool force = false);

    // Applies from the next scan on.
    void set_options(const ScanOptions& options);

    LibraryStatus status() const;
    bool wait_until_idle(uint32_t timeout_ms);

private:
    struct Job {
        std::filesystem::path folder;
        bool force = false;
        bool remove = false;
    };

    void watch(const std::filesystem::path& folder);
    void enqueue_scan(const std::filesystem::path& folder, bool force);
    void worker();
    void run_job(const Job& job);

    LibraryStore store_;
    LibraryStore writer_;   // worker thread only
    ArtworkCache artwork_;  // written by the worker; its path lookups are safe from any thread
    uint32_t quiet_ms_;

    mutable std::mutex mutex_;
    ScanOptions options_;   // guarded by mutex_
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<Job> jobs_;  // guarded by mutex_
    std::filesystem::path active_;
    bool running_job_ = false;
    bool quit_ = false;
    LibraryStatus status_;
    std::atomic<bool> cancel_{false};

    std::map<std::wstring, std::unique_ptr<FolderWatcher>> watchers_;  // caller's thread only
    std::thread thread_;
};

}  // namespace arpegio
