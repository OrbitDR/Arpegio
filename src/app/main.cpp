// Arpegio, the desktop application.
//   --data FOLDER  catalogue, cache and settings, by default %LOCALAPPDATA%\Arpegio
//   --ui FOLDER    the page, by default the ui folder next to the executable
//   --devtools     allows the browser's developer tools
//   --no-devices   lists no outputs, so the page can be worked on while another program plays: listing loads
//                  every ASIO driver, and a second copy of one in use can bring it down
#include <windows.h>

#include <objbase.h>
#include <shellapi.h>
#include <shlobj_core.h>
#include <shobjidl_core.h>

#include <algorithm>
#include <filesystem>
#include <string>

#include "app/main_window.h"

using namespace arpegio;

namespace {

// The installer gives the same one to its shortcut, which ties together the taskbar button, the name Windows
// shows beside the media controls, and the shortcut's icon.
constexpr const wchar_t* kAppUserModelId = L"OrbitDR.Arpegio";

std::filesystem::path default_data_folder() {
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder))) {
        throw std::runtime_error("cannot find the local application data folder");
    }
    std::filesystem::path path = std::filesystem::path(folder) / L"Arpegio";
    CoTaskMemFree(folder);
    return path;
}

std::filesystem::path executable_folder() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (size < path.size()) {
            path.resize(size);
            break;
        }
        path.resize(path.size() * 2);
    }
    return std::filesystem::path(path).parent_path();
}

AppOptions parse_options() {
    AppOptions options;
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 1; i < count; ++i) {
        const std::wstring argument = arguments[i];
        const bool has_value = i + 1 < count;
        if (argument == L"--data" && has_value) options.data_folder = arguments[++i];
        else if (argument == L"--ui" && has_value) options.ui_folder = arguments[++i];
        else if (argument == L"--devtools") options.dev_tools = true;
        else if (argument == L"--no-devices") options.list_devices = false;
    }
    LocalFree(arguments);
    if (options.data_folder.empty()) options.data_folder = default_data_folder();
    if (options.ui_folder.empty()) options.ui_folder = executable_folder() / L"ui";
    return options;
}

// Two copies would fight over the catalogue and over the audio device, so a second start brings the first
// to the front. Each data folder counts as its own application.
bool already_running(const std::filesystem::path& data_folder, HANDLE& guard) {
    std::wstring name = L"Arpegio-" + data_folder.wstring();
    std::replace(name.begin(), name.end(), L'\\', L'/');
    guard = CreateMutexW(nullptr, TRUE, name.c_str());
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    if (HWND window = FindWindowW(MainWindow::kWindowClass, nullptr)) {
        ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(window);
    }
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int status = 1;
    HANDLE guard = nullptr;
    try {
        const AppOptions options = parse_options();
        if (already_running(options.data_folder, guard)) {
            status = 0;
        } else {
            MainWindow window(instance, options);
            status = window.run();
        }
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "Arpegio", MB_ICONERROR);
    }

    if (guard) CloseHandle(guard);
    if (SUCCEEDED(com)) CoUninitialize();
    return status;
}
