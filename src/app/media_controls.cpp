#include "app/media_controls.h"

#include <objbase.h>
#include <shcore.h>
#include <shlwapi.h>
#include <shobjidl_core.h>
#include <systemmediatransportcontrolsinterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

#include "library/paths.h"

using Microsoft::WRL::ComPtr;
using winrt::Windows::Foundation::TimeSpan;

namespace wm = winrt::Windows::Media;
namespace streams = winrt::Windows::Storage::Streams;

namespace arpegio {

namespace {

constexpr double kTimelineSeconds = 5.0;  // Windows moves its own clock between updates
constexpr double kSeekTolerance = 1.5;    // a position further than this from the clock was a jump

enum ThumbnailButton { kPreviousButton = 1, kToggleButton, kNextButton };

struct Point {
    float x;
    float y;
};

using Polygon = std::vector<Point>;  // convex, in the 24 by 24 box of the page's icons
using Shape = std::vector<Polygon>;

const Shape kPlayShape = {{{8, 5}, {19, 12}, {8, 19}}};
const Shape kPauseShape = {{{6, 5}, {10, 5}, {10, 19}, {6, 19}}, {{14, 5}, {18, 5}, {18, 19}, {14, 19}}};
const Shape kNextShape = {{{6, 6}, {14.5f, 12}, {6, 18}}, {{16, 6}, {18, 6}, {18, 18}, {16, 18}}};
const Shape kPreviousShape = {{{6, 6}, {8, 6}, {8, 18}, {6, 18}}, {{18, 6}, {18, 18}, {9.5f, 12}}};

bool inside(const Polygon& polygon, float x, float y) {
    for (size_t i = 0; i < polygon.size(); ++i) {
        const Point a = polygon[i];
        const Point b = polygon[(i + 1) % polygon.size()];
        if ((b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x) < 0) return false;
    }
    return true;
}

// Drawn here because the colour depends on the Windows theme.
HICON draw_icon(const Shape& shape, int size, COLORREF color) {
    constexpr int kSubpixels = 4;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HBITMAP pixels = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!pixels) return nullptr;

    auto* out = static_cast<uint32_t*>(bits);
    const float scale = 24.0f / size / kSubpixels;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int covered = 0;
            for (int sub_y = 0; sub_y < kSubpixels; ++sub_y) {
                for (int sub_x = 0; sub_x < kSubpixels; ++sub_x) {
                    const float px = ((x * kSubpixels) + sub_x + 0.5f) * scale;
                    const float py = ((y * kSubpixels) + sub_y + 0.5f) * scale;
                    covered += std::any_of(shape.begin(), shape.end(), [&](const Polygon& part) { return inside(part, px, py); });
                }
            }
            const uint32_t alpha = covered * 255 / (kSubpixels * kSubpixels);
            const uint32_t red = GetRValue(color) * alpha / 255;
            const uint32_t green = GetGValue(color) * alpha / 255;
            const uint32_t blue = GetBValue(color) * alpha / 255;
            out[y * size + x] = alpha << 24 | red << 16 | green << 8 | blue;
        }
    }

    // With an alpha channel the mask is only a formality, but it must exist.
    const std::vector<uint8_t> empty_mask(static_cast<size_t>((size + 15) / 16 * 2) * size);
    const HBITMAP mask = CreateBitmap(size, size, 1, 1, empty_mask.data());
    ICONINFO icon{};
    icon.fIcon = TRUE;
    icon.hbmColor = pixels;
    icon.hbmMask = mask;
    const HICON result = CreateIconIndirect(&icon);
    DeleteObject(mask);
    DeleteObject(pixels);
    return result;
}

bool system_uses_light_theme() {
    DWORD light = 0;
    DWORD size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light != 0;
}

TimeSpan seconds_span(double seconds) {
    return std::chrono::duration_cast<TimeSpan>(std::chrono::duration<double>(std::max(seconds, 0.0)));
}

winrt::hstring wide(const std::string& text) {
    return winrt::hstring(path_from_utf8(text).wstring());
}

streams::RandomAccessStreamReference thumbnail_from(const std::filesystem::path& file) {
    ComPtr<IStream> stream;
    winrt::check_hresult(SHCreateStreamOnFileEx(file.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL,
                                                FALSE, nullptr, &stream));
    streams::IRandomAccessStream random{nullptr};
    winrt::check_hresult(CreateRandomAccessStreamOverStream(stream.Get(), BSOS_DEFAULT,
                                                            winrt::guid_of<streams::IRandomAccessStream>(),
                                                            winrt::put_abi(random)));
    return streams::RandomAccessStreamReference::CreateFromStream(random);
}

}  // namespace

struct MediaControls::Impl {
    HWND window;
    UINT command;
    wm::SystemMediaTransportControls controls{nullptr};

    ComPtr<ITaskbarList3> taskbar;
    HICON play_icon = nullptr;
    HICON pause_icon = nullptr;
    HICON next_icon = nullptr;
    HICON previous_icon = nullptr;

    bool first_update = true;
    int64_t shown_track = -1;
    PlayerState shown_state = PlayerState::Stopped;
    double shown_duration = 0.0;
    double clock_position = 0.0;    // where Windows' clock stands
    ULONGLONG clock_set_at = 0;
    bool shown_next = false;
    bool shown_previous = false;

    void send(MediaCommand value) const { PostMessageW(window, command, static_cast<WPARAM>(value), 0); }

    void show_metadata(const NowPlaying& now) {
        auto display = controls.DisplayUpdater();
        display.ClearAll();
        display.Type(wm::MediaPlaybackType::Music);
        auto music = display.MusicProperties();
        music.Title(wide(now.title));
        music.Artist(wide(now.artist));
        music.AlbumTitle(wide(now.album));
        if (!now.artwork.empty()) {
            try {
                display.Thumbnail(thumbnail_from(now.artwork));
            } catch (const winrt::hresult_error&) {
                // The cover is decoration: the title still goes out.
            }
        }
        display.Update();
    }

    void show_timeline(const NowPlaying& now) {
        wm::SystemMediaTransportControlsTimelineProperties timeline;
        timeline.StartTime(TimeSpan::zero());
        timeline.MinSeekTime(TimeSpan::zero());
        timeline.EndTime(seconds_span(now.duration));
        timeline.MaxSeekTime(seconds_span(now.duration));
        timeline.Position(seconds_span(now.position));
        controls.UpdateTimelineProperties(timeline);
        clock_position = now.position;
        clock_set_at = GetTickCount64();
        shown_duration = now.duration;
    }

    double clock_now() const {
        if (shown_state != PlayerState::Playing) return clock_position;
        return clock_position + static_cast<double>(GetTickCount64() - clock_set_at) / 1000.0;
    }

    void make_icons() {
        const UINT dpi = GetDpiForWindow(window);
        const int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
        const COLORREF color = system_uses_light_theme() ? RGB(0x20, 0x20, 0x20) : RGB(0xFF, 0xFF, 0xFF);
        destroy_icons();
        play_icon = draw_icon(kPlayShape, size, color);
        pause_icon = draw_icon(kPauseShape, size, color);
        next_icon = draw_icon(kNextShape, size, color);
        previous_icon = draw_icon(kPreviousShape, size, color);
    }

    void destroy_icons() {
        for (HICON icon : {play_icon, pause_icon, next_icon, previous_icon}) {
            if (icon) DestroyIcon(icon);
        }
        play_icon = pause_icon = next_icon = previous_icon = nullptr;
    }

    std::array<THUMBBUTTON, 3> thumbnail_buttons() const {
        const bool playing = shown_state == PlayerState::Playing;
        std::array<THUMBBUTTON, 3> buttons{};
        for (THUMBBUTTON& button : buttons) button.dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;

        buttons[0].iId = kPreviousButton;
        buttons[0].hIcon = previous_icon;
        buttons[0].dwFlags = shown_previous ? THBF_ENABLED : THBF_DISABLED;
        wcscpy_s(buttons[0].szTip, L"Anterior");

        buttons[1].iId = kToggleButton;
        buttons[1].hIcon = playing ? pause_icon : play_icon;
        buttons[1].dwFlags = shown_track > 0 ? THBF_ENABLED : THBF_DISABLED;
        wcscpy_s(buttons[1].szTip, playing ? L"Pausar" : L"Reproducir");

        buttons[2].iId = kNextButton;
        buttons[2].hIcon = next_icon;
        buttons[2].dwFlags = shown_next ? THBF_ENABLED : THBF_DISABLED;
        wcscpy_s(buttons[2].szTip, L"Siguiente");
        return buttons;
    }

    void refresh_thumbnail_buttons() {
        if (!taskbar) return;
        std::array<THUMBBUTTON, 3> buttons = thumbnail_buttons();
        taskbar->ThumbBarUpdateButtons(window, static_cast<UINT>(buttons.size()), buttons.data());
    }
};

MediaControls::MediaControls(HWND window, UINT command, UINT seek) : impl_(std::make_unique<Impl>()) {
    impl_->window = window;
    impl_->command = command;
    try {
        const auto interop = winrt::get_activation_factory<wm::SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
        winrt::check_hresult(interop->GetForWindow(window, winrt::guid_of<wm::SystemMediaTransportControls>(),
                                                   winrt::put_abi(impl_->controls)));
        auto& controls = impl_->controls;
        controls.IsEnabled(true);
        controls.IsPlayEnabled(true);
        controls.IsPauseEnabled(true);
        controls.IsStopEnabled(false);

        Impl* self = impl_.get();
        controls.ButtonPressed([self](const auto&, const wm::SystemMediaTransportControlsButtonPressedEventArgs& args) {
            switch (args.Button()) {
            case wm::SystemMediaTransportControlsButton::Play: self->send(MediaCommand::Play); break;
            case wm::SystemMediaTransportControlsButton::Pause: self->send(MediaCommand::Pause); break;
            case wm::SystemMediaTransportControlsButton::Next: self->send(MediaCommand::Next); break;
            case wm::SystemMediaTransportControlsButton::Previous: self->send(MediaCommand::Previous); break;
            default: break;
            }
        });
        controls.PlaybackPositionChangeRequested(
            [window, seek](const auto&, const wm::PlaybackPositionChangeRequestedEventArgs& args) {
                const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(args.RequestedPlaybackPosition());
                PostMessageW(window, seek, 0, static_cast<LPARAM>(milliseconds.count()));
            });
    } catch (const winrt::hresult_error&) {
        impl_->controls = nullptr;
    }
}

MediaControls::~MediaControls() {
    if (impl_->taskbar) impl_->taskbar.Reset();
    impl_->destroy_icons();
}

void MediaControls::update(const NowPlaying& now) {
    Impl& self = *impl_;
    const int64_t track = now.has_track ? now.track_id : 0;
    const bool track_changed = self.first_update || track != self.shown_track;
    const bool state_changed = self.first_update || now.state != self.shown_state;
    const bool buttons_changed = self.first_update || now.can_go_next != self.shown_next || now.can_go_back != self.shown_previous;
    const bool timeline_due = track_changed || state_changed || now.duration != self.shown_duration ||
                              std::abs(now.position - self.clock_now()) > kSeekTolerance ||
                              (now.state == PlayerState::Playing && now.position >= self.clock_position + kTimelineSeconds);
    if (!track_changed && !state_changed && !buttons_changed && !timeline_due) return;

    self.first_update = false;
    self.shown_track = track;
    self.shown_next = now.can_go_next;
    self.shown_previous = now.can_go_back;
    const PlayerState previous_state = self.shown_state;
    self.shown_state = now.state;
    if (track_changed || state_changed || buttons_changed) self.refresh_thumbnail_buttons();
    if (!self.controls) return;

    try {
        if (buttons_changed) {
            self.controls.IsNextEnabled(now.can_go_next);
            self.controls.IsPreviousEnabled(now.can_go_back);
        }
        if (track_changed) {
            self.controls.IsEnabled(now.has_track);  // with nothing to offer the session goes away
            if (now.has_track) self.show_metadata(now);
        }
        if ((state_changed || track_changed) && now.has_track) {
            self.controls.PlaybackStatus(now.state == PlayerState::Playing  ? wm::MediaPlaybackStatus::Playing
                                         : now.state == PlayerState::Paused ? wm::MediaPlaybackStatus::Paused
                                                                            : wm::MediaPlaybackStatus::Stopped);
        }
        if (timeline_due && now.has_track) self.show_timeline(now);
    } catch (const winrt::hresult_error&) {
        // Windows may refuse an update, and the next change sends everything again.
        self.first_update = true;
        self.shown_state = previous_state;
    }
}

void MediaControls::on_taskbar_button_created() {
    Impl& self = *impl_;
    if (!self.taskbar) {
        ComPtr<ITaskbarList3> taskbar;
        if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar)))) return;
        if (FAILED(taskbar->HrInit())) return;
        self.taskbar = std::move(taskbar);
    }
    self.make_icons();
    // After Explorer restarts the buttons are gone, and a second add on a window that has them is refused.
    std::array<THUMBBUTTON, 3> buttons = self.thumbnail_buttons();
    if (FAILED(self.taskbar->ThumbBarAddButtons(self.window, static_cast<UINT>(buttons.size()), buttons.data()))) {
        self.taskbar->ThumbBarUpdateButtons(self.window, static_cast<UINT>(buttons.size()), buttons.data());
    }
}

void MediaControls::on_thumbnail_button(int id) {
    switch (id) {
    case kPreviousButton: impl_->send(MediaCommand::Previous); break;
    case kToggleButton: impl_->send(MediaCommand::Toggle); break;
    case kNextButton: impl_->send(MediaCommand::Next); break;
    default: break;
    }
}

void MediaControls::on_theme_changed() {
    if (!impl_->taskbar) return;
    impl_->make_icons();
    impl_->refresh_thumbnail_buttons();
}

}  // namespace arpegio
