#include "library/folder_watcher.h"

#include <stdexcept>
#include <vector>

namespace arpegio {

namespace {

constexpr DWORD kNotifyFilter =
    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
constexpr DWORD kBufferBytes = 64 * 1024;

}  // namespace

FolderWatcher::FolderWatcher(const std::filesystem::path& folder, std::function<void()> on_change, uint32_t quiet_ms)
    : on_change_(std::move(on_change)), quiet_ms_(quiet_ms) {
    directory_ = CreateFileW(folder.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (directory_ == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot watch the folder");
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    changed_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { run(); });
}

FolderWatcher::~FolderWatcher() {
    SetEvent(stop_);
    thread_.join();
    CloseHandle(changed_);
    CloseHandle(stop_);
    CloseHandle(directory_);
}

void FolderWatcher::run() {
    // The contents are not read: a rescan compares every file anyway, so a full buffer loses nothing.
    std::vector<DWORD> buffer(kBufferBytes / sizeof(DWORD));
    OVERLAPPED overlapped{};
    overlapped.hEvent = changed_;
    const HANDLE events[] = {stop_, changed_};
    bool pending = false;

    for (;;) {
        ResetEvent(changed_);
        if (!ReadDirectoryChangesW(directory_, buffer.data(), kBufferBytes, TRUE, kNotifyFilter, nullptr, &overlapped,
                                   nullptr)) {
            return;
        }

        for (;;) {
            const DWORD wait = WaitForMultipleObjects(2, events, FALSE, pending ? quiet_ms_ : INFINITE);
            if (wait == WAIT_TIMEOUT) {
                pending = false;
                on_change_();
                continue;
            }
            if (wait == WAIT_OBJECT_0 + 1) break;
            // Windows keeps writing to the buffer until the cancelled read has really finished.
            CancelIoEx(directory_, &overlapped);
            DWORD ignored = 0;
            GetOverlappedResult(directory_, &overlapped, &ignored, TRUE);
            return;
        }

        DWORD bytes = 0;
        if (!GetOverlappedResult(directory_, &overlapped, &bytes, FALSE) && GetLastError() != ERROR_NOTIFY_ENUM_DIR) return;
        pending = true;
    }
}

}  // namespace arpegio
