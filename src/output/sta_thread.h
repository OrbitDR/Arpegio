// Single-threaded apartment with a message pump and a hidden window.
// ASIO drivers register ThreadingModel=Apartment, so every IASIO call runs here.
#pragma once

#include <windows.h>

#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace arpegio {

class StaThread {
public:
    StaThread();
    ~StaThread();
    StaThread(const StaThread&) = delete;
    StaThread& operator=(const StaThread&) = delete;

    HWND window() const { return window_; }

    // Runs `work` on the STA thread and waits for it. Exceptions cross back to the caller.
    template <typename F>
    auto run(F&& work) {
        using Result = std::invoke_result_t<F>;
        std::packaged_task<Result()> task(std::forward<F>(work));
        auto result = task.get_future();
        post([&task] { task(); });
        return result.get();
    }

private:
    void post(std::function<void()> work);
    void main();

    std::thread thread_;
    HANDLE wake_ = nullptr;
    HWND window_ = nullptr;
    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
    bool quit_ = false;
    std::promise<void> ready_;
};

}  // namespace arpegio
