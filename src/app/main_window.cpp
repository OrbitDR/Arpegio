#include "app/main_window.h"

#include <dwmapi.h>
#include <shlwapi.h>
#include <shobjidl_core.h>
#include <wrl.h>

#include <stdexcept>
#include <string_view>

#include "library/paths.h"
#include "resource.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using nlohmann::json;

namespace arpegio {

namespace {

// `.example` never resolves, and `.local` would wait for a network lookup.
constexpr const wchar_t* kAppHost = L"arpegio.example";
constexpr std::wstring_view kAppOrigin = L"https://arpegio.example/";
constexpr const wchar_t* kAppUrl = L"https://arpegio.example/index.html";
constexpr const wchar_t* kArtFilter = L"https://art.arpegio.example/*";
constexpr std::wstring_view kArtThumbnail = L"https://art.arpegio.example/thumb/";
constexpr std::wstring_view kArtOriginal = L"https://art.arpegio.example/full/";

constexpr UINT_PTR kTickTimer = 1;
constexpr UINT kTickMs = 200;
constexpr UINT kPickFolderMessage = WM_APP + 1;

constexpr COLORREF kCaptionColor = RGB(0x12, 0x13, 0x16);
constexpr COREWEBVIEW2_COLOR kPageColor = {255, 0x0E, 0x0F, 0x11};
constexpr int kDefaultWidth = 1320;
constexpr int kDefaultHeight = 860;
constexpr int kMinimumWidth = 900;
constexpr int kMinimumHeight = 600;

enum MediaKey { kPlayPause = 1, kNext, kPrevious };

constexpr const char* kPickFolderRequest = "library.pickFolder";

// Owns a string WebView2 allocated for us.
struct CoString {
    LPWSTR text = nullptr;
    ~CoString() { CoTaskMemFree(text); }
};

std::string to_utf8(std::wstring_view text) {
    return path_to_utf8(std::filesystem::path(text));
}

std::wstring to_wide(std::string_view text) {
    return path_from_utf8(text).wstring();
}

}  // namespace

MainWindow::MainWindow(HINSTANCE instance, AppOptions options) : options_(std::move(options)) {
    std::filesystem::create_directories(options_.data_folder);

    Controller::Options controller;
    controller.data_folder = options_.data_folder;
    controller.emit = [this](const std::string& event, const json& data) { post({{"event", event}, {"data", data}}); };
    controller_ = std::make_unique<Controller>(std::move(controller));

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = CreateSolidBrush(RGB(kPageColor.R, kPageColor.G, kPageColor.B));
    window_class.lpszClassName = kWindowClass;
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_ARPEGIO));
    window_class.hIconSm = window_class.hIcon;
    if (!RegisterClassExW(&window_class)) throw std::runtime_error("cannot register the window class");

    const std::optional<WindowPlacement>& placement = controller_->settings().window;
    const bool saved = placement && placement->width >= kMinimumWidth && placement->height >= kMinimumHeight;
    window_ = CreateWindowExW(0, kWindowClass, L"Arpegio", WS_OVERLAPPEDWINDOW, saved ? placement->x : CW_USEDEFAULT,
                              saved ? placement->y : CW_USEDEFAULT, saved ? placement->width : kDefaultWidth,
                              saved ? placement->height : kDefaultHeight, nullptr, nullptr, instance, this);
    if (!window_) throw std::runtime_error("cannot create the window");

    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF caption = kCaptionColor;
    DwmSetWindowAttribute(window_, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));

    ShowWindow(window_, saved && placement->maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    UpdateWindow(window_);

    // Fails when another program holds a key. The page's own buttons still work.
    RegisterHotKey(window_, kPlayPause, MOD_NOREPEAT, VK_MEDIA_PLAY_PAUSE);
    RegisterHotKey(window_, kNext, MOD_NOREPEAT, VK_MEDIA_NEXT_TRACK);
    RegisterHotKey(window_, kPrevious, MOD_NOREPEAT, VK_MEDIA_PREV_TRACK);

    SetTimer(window_, kTickTimer, kTickMs, nullptr);
    create_webview();
}

MainWindow::~MainWindow() {
    if (window_) DestroyWindow(window_);
}

int MainWindow::run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK MainWindow::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        auto* self = static_cast<MainWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return self ? self->on_message(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT MainWindow::on_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_SIZE:
        if (host_) {
            // A hidden page stops rendering, which leaves the machine quieter for the music.
            host_->put_IsVisible(wparam != SIZE_MINIMIZED);
            resize_webview();
        }
        return 0;
    case WM_MOVE:
        if (host_) host_->NotifyParentWindowPositionChanged();
        return 0;
    case WM_SETFOCUS:
        if (host_) host_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        const UINT dpi = GetDpiForWindow(window_);
        limits->ptMinTrackSize.x = MulDiv(kMinimumWidth, dpi, 96);
        limits->ptMinTrackSize.y = MulDiv(kMinimumHeight, dpi, 96);
        return 0;
    }
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_TIMER:
        if (wparam == kTickTimer) tick();
        return 0;
    case WM_HOTKEY:
        media_key(static_cast<int>(wparam));
        return 0;
    case kPickFolderMessage:
        pick_folder(static_cast<int64_t>(wparam));
        return 0;
    case WM_CLOSE:
        save_placement();
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        KillTimer(window_, kTickTimer);
        if (host_) host_->Close();
        webview_.Reset();
        host_.Reset();
        controller_.reset();
        window_ = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window_, message, wparam, lparam);
    }
}

void MainWindow::create_webview() {
    const std::filesystem::path profile = options_.data_folder / "webview";
    const HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, profile.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT created, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(created)) {
                    fail(L"No se pudo iniciar WebView2.", created);
                    return S_OK;
                }
                environment_ = environment;
                return environment->CreateCoreWebView2Controller(
                    window_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                 [this](HRESULT ready, ICoreWebView2Controller* host) -> HRESULT {
                                     if (FAILED(ready)) fail(L"No se pudo crear la vista web.", ready);
                                     else on_webview_ready(host);
                                     return S_OK;
                                 })
                                 .Get());
            })
            .Get());
    if (FAILED(result)) {
        fail(L"No se encontró WebView2 Runtime. Instálalo desde "
             L"https://developer.microsoft.com/microsoft-edge/webview2/",
             result);
    }
}

void MainWindow::fail(const std::wstring& text, HRESULT result) {
    wchar_t code[32];
    swprintf_s(code, L"\n\nCódigo: 0x%08lX", static_cast<unsigned long>(result));
    MessageBoxW(window_, (text + code).c_str(), L"Arpegio", MB_ICONERROR);
    DestroyWindow(window_);
}

void MainWindow::on_webview_ready(ICoreWebView2Controller* host) {
    if (!window_) return;  // closed while WebView2 was starting
    host_ = host;
    host_->get_CoreWebView2(&webview_);

    ComPtr<ICoreWebView2Controller2> colored;
    if (SUCCEEDED(host_.As(&colored))) colored->put_DefaultBackgroundColor(kPageColor);
    ComPtr<ICoreWebView2Controller4> droppable;
    if (SUCCEEDED(host_.As(&droppable))) droppable->put_AllowExternalDrop(FALSE);

    configure_webview();
    resize_webview();
    webview_->Navigate(kAppUrl);
}

void MainWindow::configure_webview() {
    ComPtr<ICoreWebView2Settings> settings;
    webview_->get_Settings(&settings);
    const BOOL dev = options_.dev_tools ? TRUE : FALSE;
    settings->put_AreDevToolsEnabled(dev);
    settings->put_AreDefaultContextMenusEnabled(dev);
    settings->put_AreHostObjectsAllowed(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
    settings->put_IsBuiltInErrorPageEnabled(FALSE);
    ComPtr<ICoreWebView2Settings3> keys;
    if (SUCCEEDED(settings.As(&keys))) keys->put_AreBrowserAcceleratorKeysEnabled(dev);
    ComPtr<ICoreWebView2Settings4> forms;
    if (SUCCEEDED(settings.As(&forms))) {
        forms->put_IsGeneralAutofillEnabled(FALSE);
        forms->put_IsPasswordAutosaveEnabled(FALSE);
    }
    ComPtr<ICoreWebView2Settings5> pinch;
    if (SUCCEEDED(settings.As(&pinch))) pinch->put_IsPinchZoomEnabled(FALSE);
    ComPtr<ICoreWebView2Settings6> swipe;
    if (SUCCEEDED(settings.As(&swipe))) swipe->put_IsSwipeNavigationEnabled(FALSE);

    ComPtr<ICoreWebView2_3> mapped;
    if (FAILED(webview_.As(&mapped))) throw std::runtime_error("this WebView2 Runtime is too old");
    mapped->SetVirtualHostNameToFolderMapping(kAppHost, options_.ui_folder.c_str(),
                                              COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);

    // The page is the application: it must never turn into some other page.
    EventRegistrationToken token{};
    webview_->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                                         [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                             CoString uri;
                                             args->get_Uri(&uri.text);
                                             if (!std::wstring_view(uri.text).starts_with(kAppOrigin)) args->put_Cancel(TRUE);
                                             return S_OK;
                                         })
                                         .Get(),
                                     &token);
    webview_->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                         [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                             args->put_Handled(TRUE);
                                             return S_OK;
                                         })
                                         .Get(),
                                     &token);
    webview_->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                         [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                             CoString source;
                                             CoString text;
                                             args->get_Source(&source.text);
                                             if (std::wstring_view(source.text).starts_with(kAppOrigin) &&
                                                 SUCCEEDED(args->get_WebMessageAsJson(&text.text))) {
                                                 on_web_message(text.text);
                                             }
                                             return S_OK;
                                         })
                                         .Get(),
                                     &token);
    webview_->AddWebResourceRequestedFilter(kArtFilter, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE);
    webview_->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                           [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                                               on_artwork_request(args);
                                               return S_OK;
                                           })
                                           .Get(),
                                       &token);
}

void MainWindow::resize_webview() {
    RECT bounds{};
    GetClientRect(window_, &bounds);
    host_->put_Bounds(bounds);
}

void MainWindow::on_web_message(const std::wstring& text) {
    int64_t request = 0;
    try {
        const json message = json::parse(to_utf8(text));
        request = message.at("id");
        const std::string method = message.at("method");
        if (method == kPickFolderRequest) {
            // A dialog runs its own message loop, which must not happen inside a WebView2 event.
            PostMessageW(window_, kPickFolderMessage, static_cast<WPARAM>(request), 0);
            return;
        }
        reply(request, controller_->handle(method, message.value("params", json::object())));
    } catch (const std::exception& error) {
        reply_error(request, error.what());
    }
}

void MainWindow::on_artwork_request(ICoreWebView2WebResourceRequestedEventArgs* args) {
    ComPtr<ICoreWebView2WebResourceRequest> request;
    CoString uri;
    if (FAILED(args->get_Request(&request)) || FAILED(request->get_Uri(&uri.text))) return;

    const std::wstring_view text = uri.text;
    const bool thumbnail = text.starts_with(kArtThumbnail);
    const std::wstring_view prefix = thumbnail ? kArtThumbnail : kArtOriginal;
    std::optional<ArtworkResponse> artwork;
    if (text.starts_with(prefix)) artwork = controller_->artwork(_wtoi64(uri.text + prefix.size()), thumbnail);

    ComPtr<IStream> stream;
    ComPtr<ICoreWebView2WebResourceResponse> response;
    if (artwork && SUCCEEDED(SHCreateStreamOnFileEx(artwork->file.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE,
                                                    FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream))) {
        const std::wstring headers = L"Content-Type: " + to_wide(artwork->mime);
        environment_->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers.c_str(), &response);
    } else {
        environment_->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &response);
    }
    args->put_Response(response.Get());
}

void MainWindow::pick_folder(int64_t request) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        reply_error(request, "no se pudo abrir el selector de carpetas");
        return;
    }
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Añadir carpeta de música");

    ComPtr<IShellItem> item;
    CoString path;
    if (dialog->Show(window_) != S_OK || FAILED(dialog->GetResult(&item)) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path.text))) {
        reply(request, nullptr);  // cancelled
        return;
    }
    try {
        reply(request, controller_->handle("library.addFolder", {{"path", to_utf8(path.text)}}));
    } catch (const std::exception& error) {
        reply_error(request, error.what());
    }
}

void MainWindow::reply(int64_t request, const json& result) {
    post({{"id", request}, {"result", result}});
}

void MainWindow::reply_error(int64_t request, const std::string& text) {
    post({{"id", request}, {"error", text}});
}

void MainWindow::post(const json& message) {
    if (!webview_) return;
    // Tags come from files, so one bad byte must not lose a whole message.
    const std::string text = message.dump(-1, ' ', false, json::error_handler_t::replace);
    webview_->PostWebMessageAsJson(to_wide(text).c_str());
}

void MainWindow::tick() {
    if (!controller_) return;
    controller_->tick();
    const std::string title = controller_->now_playing_title();
    if (title != title_shown_) {
        title_shown_ = title;
        SetWindowTextW(window_, title.empty() ? L"Arpegio" : to_wide(title + "  ·  Arpegio").c_str());
    }
}

void MainWindow::save_placement() {
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(window_, &placement)) return;
    const RECT& rect = placement.rcNormalPosition;
    controller_->set_window_placement({rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                                       placement.showCmd == SW_SHOWMAXIMIZED});
}

void MainWindow::media_key(int key) {
    try {
        const char* method = key == kPlayPause ? "player.toggle" : key == kNext ? "player.next" : "player.previous";
        controller_->handle(method, json::object());
    } catch (const std::exception& error) {
        post({{"event", "error"}, {"data", {{"message", error.what()}}}});
    }
}

}  // namespace arpegio
