// fmtownskit runtime -- a window for the machine, on SDL3 (zlib licence): optional, towns.cmake
// builds fmt_towns_sdl when SDL3 is found.
//
// At each VSYNC (Machine::on_vsync calls vsync()): the host's events, the keyboard and the
// gamepads read as the Towns pad, the picture drawn at an integer scale (the frame's part the
// CRTC's layer windows cover, not MAME's whole frame with its borders, centred on a canvas of at
// least 640x480: a 640x400 and a 640x480 mode at the same scale), the frame's sound queued
// at kAudioRate, and the guest held to the sound's pace (it runs ahead until about three frames
// of sound wait in the queue), so the game keeps the time the sound card keeps.
//
//   keyboard: arrows, X = A, Z = B, Enter = Run, Right Shift or Backspace = Select;
//             F11 or Alt+Enter full screen, Tab held runs unpaced, Escape quits
//   gamepad:  d-pad or left stick, East = A, South = B, Start = Run, Back = Select
//             (a second gamepad is the second pad when the machine has no mouse there)
//   mouse:    the mouse in the second port (its movement in the frame's pixels, the left and
//             right buttons), the host's pointer hidden over the window
#pragma once
#include "towns.h"
#include <memory>
#include <string>

namespace towns {

struct SdlOptions {
    std::string title = "FM Towns";
    int scale = 0;                  // 0: the largest whole scale the desktop has room for
    bool fullscreen = false;
};

class SdlHost {
public:
    SdlHost();
    ~SdlHost();
    bool open(const SdlOptions& o, std::string& err);
    void vsync(Machine& m);         // throws Stop{0} when the window is closed
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace towns
