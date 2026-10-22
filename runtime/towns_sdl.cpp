// fmtownskit runtime -- a window for the machine, on SDL3 (see towns_sdl.h).
#include "towns_sdl.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <string>
#include <vector>

namespace towns {

static constexpr int kLeadFrames = 3;       // the sound queued ahead of the speakers, in frames

struct SdlHost::Impl {
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    SDL_Texture* tex = nullptr;
    int tw = 0, th = 0;
    SDL_AudioStream* audio = nullptr;
    std::vector<SDL_Gamepad*> pads;
    std::vector<uint32_t> rgb;
    std::vector<int16_t> sound;
    uint64_t next_ns = 0;                   // the pace without sound: the next frame's time
    std::string title;
    int shown_rate = 0;                     // the CPU's rate the title shows
    float mx = 0, my = 0;                   // the host mouse's movement not yet given, in frame pixels
    ~Impl() {
        for (SDL_Gamepad* g : pads) SDL_CloseGamepad(g);
        if (audio) SDL_DestroyAudioStream(audio);
        if (tex) SDL_DestroyTexture(tex);
        if (ren) SDL_DestroyRenderer(ren);
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
    }
};

SdlHost::SdlHost() = default;
SdlHost::~SdlHost() = default;

bool SdlHost::open(const SdlOptions& o, std::string& err) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) { err = SDL_GetError(); return false; }
    p_ = std::make_unique<Impl>();
    p_->title = o.title;
    int w = 640, h = 480, scale = o.scale;
    if (scale <= 0) {
        SDL_Rect r;
        scale = 1;
        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &r))
            while (w * (scale + 1) <= r.w && h * (scale + 1) + 40 <= r.h) ++scale;
    }
    if (!SDL_CreateWindowAndRenderer(o.title.c_str(), w * scale, h * scale, SDL_WINDOW_RESIZABLE, &p_->win, &p_->ren)) {
        err = SDL_GetError();
        return false;
    }
    if (o.fullscreen) SDL_SetWindowFullscreen(p_->win, true);
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, kAudioRate};
    p_->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (p_->audio) SDL_ResumeAudioStreamDevice(p_->audio);
    else std::fprintf(stderr, "no sound: %s\n", SDL_GetError());
    return true;
}

static unsigned held_keys() {
    const bool* k = SDL_GetKeyboardState(nullptr);
    unsigned v = 0;
    if (k[SDL_SCANCODE_UP]) v |= kPadUp;
    if (k[SDL_SCANCODE_DOWN]) v |= kPadDown;
    if (k[SDL_SCANCODE_LEFT]) v |= kPadLeft;
    if (k[SDL_SCANCODE_RIGHT]) v |= kPadRight;
    if (k[SDL_SCANCODE_X]) v |= kPadA;
    if (k[SDL_SCANCODE_Z]) v |= kPadB;
    if (k[SDL_SCANCODE_RETURN] && !(SDL_GetModState() & SDL_KMOD_ALT)) v |= kPadRun;
    if (k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_BACKSPACE]) v |= kPadSelect;
    return v;
}

static unsigned held_pad(SDL_Gamepad* g) {
    unsigned v = 0;
    const int dead = 16384;
    int x = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -dead) v |= kPadUp;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > dead) v |= kPadDown;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -dead) v |= kPadLeft;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > dead) v |= kPadRight;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_EAST)) v |= kPadA;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH)) v |= kPadB;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START)) v |= kPadRun;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK)) v |= kPadSelect;
    return v;
}

void SdlHost::vsync(Machine& m) {
    Impl& s = *p_;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT: throw Stop{0};
        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat) break;
            if (e.key.scancode == SDL_SCANCODE_ESCAPE) throw Stop{0};
            if (e.key.scancode == SDL_SCANCODE_F11 || (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)))
                SDL_SetWindowFullscreen(s.win, !(SDL_GetWindowFlags(s.win) & SDL_WINDOW_FULLSCREEN));
            if (e.key.scancode == SDL_SCANCODE_F5) m.set_cpu_rate(m.cpu_rate - 5);
            if (e.key.scancode == SDL_SCANCODE_F6) m.set_cpu_rate(m.cpu_rate + 5);
            if (e.key.scancode == SDL_SCANCODE_F7) m.set_cpu_rate(100);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            // the frame's pixels moved, as the mouse in the second port's counts
            SDL_ConvertEventToRenderCoordinates(s.ren, &e);
            s.mx += e.motion.xrel;
            s.my += e.motion.yrel;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
            uint8_t bit = e.button.button == SDL_BUTTON_LEFT ? 0x10 : e.button.button == SDL_BUTTON_RIGHT ? 0x20 : 0;
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) m.mouse.buttons &= (uint8_t)~bit;
            else m.mouse.buttons |= bit;
            break;
        }
        case SDL_EVENT_WINDOW_MOUSE_ENTER: SDL_HideCursor(); break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE: SDL_ShowCursor(); break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (SDL_Gamepad* g = SDL_OpenGamepad(e.gdevice.which)) s.pads.push_back(g);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            for (size_t i = 0; i < s.pads.size(); ++i)
                if (SDL_GetGamepadID(s.pads[i]) == e.gdevice.which) { SDL_CloseGamepad(s.pads[i]); s.pads.erase(s.pads.begin() + (long)i); break; }
            break;
        default: break;
        }
    }
    if (m.cpu_rate != s.shown_rate) {
        s.shown_rate = m.cpu_rate;
        std::string t = s.title + "  -  CPU " + std::to_string(m.cpu_rate) + "%";
        SDL_SetWindowTitle(s.win, t.c_str());
    }
    // the keyboard and the first gamepad are the first pad, a second gamepad the second
    unsigned p0 = held_keys(), p1 = 0;
    if (s.pads.size() > 0) p0 |= held_pad(s.pads[0]);
    if (s.pads.size() > 1) p1 = held_pad(s.pads[1]);
    m.set_pad(0, p0);
    if (!m.mouse.present) m.set_pad(1, p1);
    int dx = (int)s.mx, dy = (int)s.my;
    m.mouse.x = (int16_t)(m.mouse.x + dx);
    m.mouse.y = (int16_t)(m.mouse.y + dy);
    s.mx -= (float)dx; s.my -= (float)dy;

    // the picture only (the CRTC's layer windows, not MAME's whole frame with its borders),
    // centred on a canvas of at least 640x480: every mode at the same scale
    int fw, fh, x, y, w, h;
    m.render(s.rgb, fw, fh);
    m.display_area(x, y, w, h);
    int cw = std::max(w, 640), ch = std::max(h, 480);
    if (w != s.tw || h != s.th) {
        if (s.tex) SDL_DestroyTexture(s.tex);
        s.tex = SDL_CreateTexture(s.ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureScaleMode(s.tex, SDL_SCALEMODE_NEAREST);
        SDL_SetRenderLogicalPresentation(s.ren, cw, ch, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
        s.tw = w; s.th = h;
    }
    SDL_UpdateTexture(s.tex, nullptr, s.rgb.data() + (size_t)y * fw + x, fw * 4);
    SDL_SetRenderDrawColor(s.ren, 0, 0, 0, 255);
    SDL_RenderClear(s.ren);
    SDL_FRect dst{(float)((cw - w) / 2), (float)((ch - h) / 2), (float)w, (float)h};
    SDL_RenderTexture(s.ren, s.tex, nullptr, &dst);
    SDL_RenderPresent(s.ren);

    m.audio_take(s.sound);
    bool unpaced = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_TAB];
    if (s.audio) {
        const int frame_bytes = kAudioRate / 60 * 4;
        if (unpaced) {
            if (SDL_GetAudioStreamQueued(s.audio) < kLeadFrames * frame_bytes)
                SDL_PutAudioStreamData(s.audio, s.sound.data(), (int)(s.sound.size() * 2));
            return;
        }
        SDL_PutAudioStreamData(s.audio, s.sound.data(), (int)(s.sound.size() * 2));
        while (SDL_GetAudioStreamQueued(s.audio) > kLeadFrames * frame_bytes) SDL_DelayNS(500000);
        return;
    }
    if (unpaced) return;
    uint64_t t = SDL_GetTicksNS();
    if (!s.next_ns || t > s.next_ns + 100000000) s.next_ns = t;
    s.next_ns += 1000000000ull / 60;
    if (s.next_ns > t) SDL_DelayPrecise(s.next_ns - t);
}

}  // namespace towns
