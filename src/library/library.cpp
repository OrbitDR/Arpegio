#include "library/library.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include "library/paths.h"

namespace arpegio {

namespace {

// True when one folder contains the other or both are the same.
bool overlap(const std::wstring& a, const std::wstring& b) {
    const auto inside = [](const std::wstring& outer, const std::wstring& inner) {
        const std::wstring prefix = outer.ends_with(L'/') ? outer : outer + L'/';
        return inner.starts_with(prefix);
    };
    return a == b || inside(a, b) || inside(b, a);
}

}  // namespace

Library::Library(const std::filesystem::path& database, const std::filesystem::path& cache, ScanOptions options,
                 uint32_t quiet_ms)
    : store_(database), writer_(database), artwork_(cache), options_(options), quiet_ms_(quiet_ms) {
    const std::vector<Folder> folders = store_.folders();
    thread_ = std::thread([this] { worker(); });
    for (const Folder& folder : folders) {
        try {
            watch(folder.path);
        } catch (const std::exception&) {
            // A drive that is not connected now can still come back. The scan below reports it.
        }
        enqueue_scan(folder.path, false);
    }
}

Library::~Library() {
    watchers_.clear();
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    cancel_ = true;
    wake_.notify_all();
    thread_.join();
}

void Library::watch(const std::filesystem::path& folder) {
    watchers_[path_key(folder)] =
        std::make_unique<FolderWatcher>(folder, [this, folder] { enqueue_scan(folder, false); }, quiet_ms_);
}

void Library::add_folder(const std::filesystem::path& folder) {
    std::error_code code;
    const std::filesystem::path root = std::filesystem::weakly_canonical(folder, code);
    if (code || !std::filesystem::is_directory(root, code)) throw std::runtime_error("the folder does not exist");

    const std::wstring key = path_key(root);
    for (const Folder& known : store_.folders()) {
        if (overlap(key, path_key(known.path))) throw std::runtime_error("the folder overlaps " + path_to_utf8(known.path));
    }
    store_.add_folder(root);
    watch(root);
    enqueue_scan(root, false);
}

void Library::remove_folder(const std::filesystem::path& folder) {
    std::error_code code;
    const std::filesystem::path root = std::filesystem::weakly_canonical(folder, code);
    watchers_.erase(path_key(root));  // joins the watcher, so outside the lock: its callback takes it

    {
        std::lock_guard lock(mutex_);
        const std::wstring key = path_key(root);
        std::erase_if(jobs_, [&key](const Job& job) { return path_key(job.folder) == key; });
        jobs_.push_back({root, false, true});
        if (running_job_ && path_key(active_) == key) cancel_ = true;
    }
    wake_.notify_all();
}

void Library::rescan(bool force) {
    for (const Folder& folder : store_.folders()) enqueue_scan(folder.path, force);
}

void Library::enqueue_scan(const std::filesystem::path& folder, bool force) {
    {
        std::lock_guard lock(mutex_);
        const std::wstring key = path_key(folder);
        const auto queued =
            std::find_if(jobs_.begin(), jobs_.end(), [&key](const Job& job) { return path_key(job.folder) == key; });
        if (queued == jobs_.end()) {
            jobs_.push_back({folder, force, false});
        } else if (!queued->remove) {
            queued->force = queued->force || force;
        }
    }
    wake_.notify_all();
}

LibraryStatus Library::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

bool Library::wait_until_idle(uint32_t timeout_ms) {
    std::unique_lock lock(mutex_);
    return idle_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return jobs_.empty() && !running_job_; });
}

void Library::worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
            if (quit_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            active_ = job.folder;
            running_job_ = true;
            cancel_ = false;
            status_.scanning = !job.remove;
            status_.done = 0;
            status_.total = 0;
            status_.current.clear();
        }

        run_job(job);

        {
            std::lock_guard lock(mutex_);
            running_job_ = false;
            status_.scanning = false;
            status_.current.clear();
            ++status_.revision;
        }
        idle_.notify_all();
    }
}

void Library::run_job(const Job& job) {
    std::string error;
    std::vector<std::string> warnings;
    try {
        if (job.remove) {
            writer_.remove_folder(job.folder);
        } else {
            ScanOptions options = options_;
            options.force = job.force;
            const ScanProgress progress = [this](uint64_t done, uint64_t total, const std::string& path) {
                std::lock_guard lock(mutex_);
                status_.done = done;
                status_.total = total;
                status_.current = path;
            };
            warnings = scan_folder(writer_, artwork_, job.folder, options, progress, &cancel_).warnings;
        }
    } catch (const std::exception& failure) {
        error = failure.what();
    }

    std::lock_guard lock(mutex_);
    status_.error = std::move(error);
    if (!job.remove) status_.warnings = std::move(warnings);
}

}  // namespace arpegio
