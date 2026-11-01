// What Windows shows outside the window: the media flyout and the lock screen, which also route the keyboard's
// media keys, and the buttons under the taskbar thumbnail. If Windows refuses any of it the program carries on
// without, because none of it is needed to play music.
#pragma once

#include <windows.h>

#include <memory>

#include "app/controller.h"

namespace arpegio {

enum class MediaCommand { Play, Pause, Toggle, Next, Previous };

class MediaControls {
public:
    // Windows calls from threads of its own, so everything comes back to the window as messages:
    // `command` with a MediaCommand in wparam, and `seek` with the position in milliseconds in lparam.
    MediaControls(HWND window, UINT command, UINT seek);
    ~MediaControls();
    MediaControls(const MediaControls&) = delete;
    MediaControls& operator=(const MediaControls&) = delete;

    // Cheap when nothing changed, so it can run on every tick.
    void update(const NowPlaying& now);

    void on_taskbar_button_created();   // the message registered as "TaskbarButtonCreated"
    void on_thumbnail_button(int id);   // WM_COMMAND with THBN_CLICKED
    void on_theme_changed();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace arpegio
