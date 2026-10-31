#include "output/sta_thread.h"

#include <objbase.h>

namespace arpegio {

StaThread::StaThread() {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto ready = ready_.get_future();
    thread_ = std::thread([this] { main(); });
    ready.get();
}

StaThread::~StaThread() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    SetEvent(wake_);
    thread_.join();
    CloseHandle(wake_);
}

void StaThread::post(std::function<void()> work) {
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(work));
    }
    SetEvent(wake_);
}

void StaThread::main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    window_ = CreateWindowExW(0, L"STATIC", L"arpegio-asio", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    ready_.set_value();

    for (;;) {
        MsgWaitForMultipleObjectsEx(1, &wake_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        std::deque<std::function<void()>> pending;
        bool quit = false;
        {
            std::lock_guard lock(mutex_);
            pending.swap(queue_);
            quit = quit_;
        }
        for (auto& work : pending) work();
        if (quit) break;
    }

    DestroyWindow(window_);
    if (SUCCEEDED(com)) CoUninitialize();
}

}  // namespace arpegio
