// The window: a Win32 frame filled by one WebView2, and the messages between the page and the Controller.
#pragma once

#include <windows.h>

#include <objbase.h>

#include <WebView2.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "app/controller.h"
#include "app/media_controls.h"

namespace arpegio {

struct AppOptions {
    std::filesystem::path data_folder;
    std::filesystem::path ui_folder;
    bool dev_tools = false;
};

class MainWindow {
public:
    static constexpr const wchar_t* kWindowClass = L"ArpegioMainWindow";

    MainWindow(HINSTANCE instance, AppOptions options);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    int run();

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT on_message(UINT message, WPARAM wparam, LPARAM lparam);

    void create_webview();
    void on_webview_ready(ICoreWebView2Controller* host);
    void fail(const std::wstring& text, HRESULT result);
    void configure_webview();
    void resize_webview();

    void on_web_message(const std::wstring& text);
    void on_artwork_request(ICoreWebView2WebResourceRequestedEventArgs* args);
    void pick_folder(int64_t request);
    void reply(int64_t request, const nlohmann::json& result);
    void reply_error(int64_t request, const std::string& text);
    void post(const nlohmann::json& message);

    void tick();
    void save_placement();
    void media_command(MediaCommand command);
    void media_seek(double seconds);

    AppOptions options_;
    HWND window_ = nullptr;
    UINT taskbar_button_created_ = 0;
    std::unique_ptr<Controller> controller_;
    std::unique_ptr<MediaControls> media_;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> host_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
    std::string title_shown_;
};

}  // namespace arpegio
