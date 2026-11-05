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
constexpr UINT kMediaCommandMessage = WM_APP + 2;
constexpr UINT kMediaSeekMessage = WM_APP + 3;

constexpr COREWEBVIEW2_COLOR kPageColor = {255, 0x0E, 0x0F, 0x11};
constexpr COREWEBVIEW2_COLOR kClear = {0, 0, 0, 0};
constexpr int kDefaultWidth = 1320;
constexpr int kDefaultHeight = 860;
constexpr int kMinimumWidth = 900;
constexpr int kMinimumHeight = 600;

// Undocumented: with visual styles, DefWindowProc draws the caption and the frame through these. On a window
// without a caption it would draw the top of the frame over the page, as Chromium also found.
constexpr UINT kDrawThemedCaption = 0x00AE;  // WM_NCUAHDRAWCAPTION
constexpr UINT kDrawThemedFrame = 0x00AF;    // WM_NCUAHDRAWFRAME

constexpr const char* kPickFolderRequest = "library.pickFolder";
constexpr std::string_view kWindowRequests = "window.";

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
    if (!options_.list_devices) controller.list_devices = [] { return std::vector<DeviceInfo>{}; };
    controller_ = std::make_unique<Controller>(std::move(controller));

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
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
    apply_frame();

    // Before the window is shown: that is when Windows announces the taskbar button.
    taskbar_button_created_ = RegisterWindowMessageW(L"TaskbarButtonCreated");
    media_ = std::make_unique<MediaControls>(window_, kMediaCommandMessage, kMediaSeekMessage);

    ShowWindow(window_, saved && placement->maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    UpdateWindow(window_);

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
    if (message == taskbar_button_created_ && media_) {
        media_->on_taskbar_button_created();
        return 0;
    }
    switch (message) {
    case WM_NCCALCSIZE:
        return on_calc_size(wparam, lparam);
    // Windows paints its frame as if the window still had a top border, and that border stayed over the clear
    // title bar as a grey strip. Activation is told not to paint, the theme's own drawing is refused, and after
    // the frame painting that cannot be refused, which also draws the shadow, the strip is painted clear again.
    case WM_NCACTIVATE:
        return DefWindowProcW(window_, message, wparam, -1);
    case kDrawThemedCaption:
    case kDrawThemedFrame:
        return 0;
    case WM_NCPAINT: {
        const LRESULT result = DefWindowProcW(window_, message, wparam, lparam);
        clear_top_border();
        return result;
    }
    case WM_ERASEBKGND: {
        RECT client{};
        GetClientRect(window_, &client);
        paint_background(reinterpret_cast<HDC>(wparam), client);
        return 1;
    }
    case WM_ACTIVATE:
        active_ = LOWORD(wparam) != WA_INACTIVE;
        post_window_state();
        return DefWindowProcW(window_, message, wparam, lparam);
    case WM_SIZE:
        if (host_) {
            // A hidden page stops rendering, which leaves the machine quieter for the music.
            host_->put_IsVisible(wparam != SIZE_MINIMIZED);
            resize_webview();
        }
        if (wparam != SIZE_MINIMIZED && maximized_shown_ != (IsZoomed(window_) != FALSE)) post_window_state();
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
        // Without a caption, Windows would maximize over the taskbar.
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) {
            const RECT& work = monitor.rcWork;
            const int border = frame_thickness();
            limits->ptMaxPosition = {work.left - monitor.rcMonitor.left - border, work.top - monitor.rcMonitor.top - border};
            limits->ptMaxSize = {work.right - work.left + 2 * border, work.bottom - work.top + 2 * border};
        }
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
    case WM_COMMAND:
        if (HIWORD(wparam) == THBN_CLICKED && media_) media_->on_thumbnail_button(LOWORD(wparam));
        return 0;
    case WM_SETTINGCHANGE:
        if (media_ && lparam && CompareStringOrdinal(reinterpret_cast<const wchar_t*>(lparam), -1, L"ImmersiveColorSet", -1,
                                                     TRUE) == CSTR_EQUAL) {
            media_->on_theme_changed();
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    case WM_APPCOMMAND:
        // Only when no media session takes the keys, such as before anything was queued.
        switch (GET_APPCOMMAND_LPARAM(lparam)) {
        case APPCOMMAND_MEDIA_PLAY_PAUSE: media_command(MediaCommand::Toggle); return TRUE;
        case APPCOMMAND_MEDIA_PLAY: media_command(MediaCommand::Play); return TRUE;
        case APPCOMMAND_MEDIA_PAUSE: media_command(MediaCommand::Pause); return TRUE;
        case APPCOMMAND_MEDIA_NEXTTRACK: media_command(MediaCommand::Next); return TRUE;
        case APPCOMMAND_MEDIA_PREVIOUSTRACK: media_command(MediaCommand::Previous); return TRUE;
        default: return DefWindowProcW(window_, message, wparam, lparam);
        }
    case kPickFolderMessage:
        pick_folder(static_cast<int64_t>(wparam));
        return 0;
    case kMediaCommandMessage:
        media_command(static_cast<MediaCommand>(wparam));
        return 0;
    case kMediaSeekMessage:
        media_seek(static_cast<double>(lparam) / 1000.0);
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
        media_.reset();
        controller_.reset();
        window_ = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window_, message, wparam, lparam);
    }
}

// No caption: over the clear page Windows would draw its own buttons next to the page's. The thick frame stays
// for the borders, the shadow, snapping and the system menu.
void MainWindow::apply_frame() {
    SetWindowLongPtrW(window_, GWL_STYLE, GetWindowLongPtrW(window_, GWL_STYLE) & ~static_cast<LONG_PTR>(WS_CAPTION));
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    // Windows 11 22H2 and later blur what lies behind the window, and the page lets it through where it is
    // clear. Older versions keep the page opaque.
    const DWM_SYSTEMBACKDROP_TYPE material = DWMSBT_TRANSIENTWINDOW;
    backdrop_ = SUCCEEDED(DwmSetWindowAttribute(window_, DWMWA_SYSTEMBACKDROP_TYPE, &material, sizeof(material)));
    if (backdrop_) {
        const MARGINS whole{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(window_, &whole);
    }
    SetWindowPos(window_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT MainWindow::on_calc_size(WPARAM wparam, LPARAM lparam) {
    if (!wparam) return DefWindowProcW(window_, WM_NCCALCSIZE, wparam, lparam);
    auto* sizes = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
    const RECT proposed = sizes->rgrc[0];
    const LRESULT result = DefWindowProcW(window_, WM_NCCALCSIZE, wparam, lparam);
    // Windows keeps its borders on the sides and the bottom, outside what is seen, and the page takes the
    // rest. A maximized window reaches past the screen by a border on every side, the top too.
    const LONG border = sizes->rgrc[0].left - proposed.left;
    sizes->rgrc[0].top = proposed.top + (IsZoomed(window_) ? border : 0);
    return result;
}

// How far the invisible resizing border reaches out of the window.
int MainWindow::frame_thickness() const {
    RECT frame{};
    AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE)), FALSE, 0,
                             GetDpiForWindow(window_));
    return -frame.left;
}

// Black is clear where Windows draws its material.
void MainWindow::paint_background(HDC context, const RECT& area) {
    const COLORREF color = backdrop_ ? RGB(0, 0, 0) : RGB(kPageColor.R, kPageColor.G, kPageColor.B);
    SetDCBrushColor(context, color);
    FillRect(context, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

// The band under the page where Windows would have the top of its frame.
void MainWindow::clear_top_border() {
    RECT band{};
    GetClientRect(window_, &band);
    band.bottom = band.top + frame_thickness();
    if (HDC context = GetDC(window_)) {
        paint_background(context, band);
        ReleaseDC(window_, context);
    }
}

json MainWindow::window_state() const {
    return {{"maximized", IsZoomed(window_) != FALSE}, {"active", active_}, {"backdrop", backdrop_}};
}

// Posted, not run here: minimizing, closing or resizing inside a WebView2 event would reenter it.
json MainWindow::window_command(const std::string& method, const json& params) {
    if (method == "window.minimize") {
        PostMessageW(window_, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    } else if (method == "window.maximize") {
        PostMessageW(window_, WM_SYSCOMMAND, IsZoomed(window_) ? SC_RESTORE : SC_MAXIMIZE, 0);
    } else if (method == "window.close") {
        PostMessageW(window_, WM_CLOSE, 0, 0);
    } else if (method == "window.resize") {
        // The window has no border above the page, so the page lends its top edge: a press there becomes
        // the press on a border that Windows resizes from. Only while the button is still down, or the
        // window would follow the pointer until the next click.
        const std::string edge = params.value("edge", "top");
        const WPARAM hit = edge == "topleft" ? HTTOPLEFT : edge == "topright" ? HTTOPRIGHT : HTTOP;
        const int button = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
        POINT cursor{};
        if (!IsZoomed(window_) && (GetAsyncKeyState(button) & 0x8000) && GetCursorPos(&cursor)) {
            ReleaseCapture();
            PostMessageW(window_, WM_NCLBUTTONDOWN, hit, MAKELPARAM(cursor.x, cursor.y));
        }
    } else if (method != "window.state") {
        throw std::runtime_error("unknown method: " + method);
    }
    return window_state();
}

void MainWindow::post_window_state() {
    maximized_shown_ = IsZoomed(window_) != FALSE;
    post({{"event", "window"}, {"data", window_state()}});
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
    if (SUCCEEDED(host_.As(&colored))) colored->put_DefaultBackgroundColor(backdrop_ ? kClear : kPageColor);
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
    // The page's title bar marks itself with `app-region: drag` and then moves the window like a caption.
    ComPtr<ICoreWebView2Settings9> regions;
    if (SUCCEEDED(settings.As(&regions))) regions->put_IsNonClientRegionSupportEnabled(TRUE);

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
        if (method.starts_with(kWindowRequests)) {
            reply(request, window_command(method, message.value("params", json::object())));
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
        // The page reads how bright a cover is, to darken the player bar behind its text.
        const std::wstring headers =
            L"Content-Type: " + to_wide(artwork->mime) + L"\nAccess-Control-Allow-Origin: https://arpegio.example";
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
    if (media_) media_->update(controller_->now_playing());
}

void MainWindow::save_placement() {
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(window_, &placement)) return;
    const RECT& rect = placement.rcNormalPosition;
    controller_->set_window_placement({rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                                       placement.showCmd == SW_SHOWMAXIMIZED});
}

void MainWindow::media_command(MediaCommand command) {
    if (!controller_) return;
    try {
        const bool playing = controller_->now_playing().state == PlayerState::Playing;
        switch (command) {
        case MediaCommand::Play:
            if (!playing) controller_->handle("player.toggle", json::object());
            break;
        case MediaCommand::Pause:
            if (playing) controller_->handle("player.toggle", json::object());
            break;
        case MediaCommand::Toggle: controller_->handle("player.toggle", json::object()); break;
        case MediaCommand::Next: controller_->handle("player.next", json::object()); break;
        case MediaCommand::Previous: controller_->handle("player.previous", json::object()); break;
        }
    } catch (const std::exception& error) {
        post({{"event", "error"}, {"data", {{"message", error.what()}}}});
    }
}

void MainWindow::media_seek(double seconds) {
    if (!controller_) return;
    try {
        controller_->handle("player.seek", {{"seconds", seconds}});
    } catch (const std::exception& error) {
        post({{"event", "error"}, {"data", {{"message", error.what()}}}});
    }
}

}  // namespace arpegio
