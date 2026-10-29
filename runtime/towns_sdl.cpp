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

// PATCH, this repository: SDL's scancode as the Towns' keyboard scan code, and the character SDL computed for it
// (which already has the shift state applied, so 'shift+1' arrives as '!'). Only keys a program can distinguish are
// mapped, and an unmapped key reports nothing rather than a guess.
//
// THE TOWNS KEYBOARD IS A JIS KEYBOARD AND ITS CODES ARE NOT PC/AT SET 1. The switch below is PC/AT because that is
// what SDL reports and it is worth being able to read; kPcToTowns translates it, and the disagreement is exactly where
// a player notices first: RETURN is 0x1D on the Towns and 0x1C on PC, SPACE 0x35 against 0x39, UP 0x4D against 0x48,
// LEFT 0x4F against 0x4B, RIGHT 0x51 against 0x4D, SHIFT 0x53 against 0x2A, CTRL 0x52 against 0x1D, and Z is 0x2A
// against 0x2C, which shifts the whole bottom letter row. Values from references/TOWNSEMU's TOWNS_JISKEY_* enum.
//
// The consequence is measurable and was measured: ENCHANT indexes a 114-entry table with `(BX >> 8) - 1`, so a code
// from the wrong set indexes the wrong entry and the key reads as dead. On the port, space, return and up did nothing
// while 'a' and down worked, and those are precisely the two the two sets happen to share.
static const uint8_t kPcToTowns[128] = {
    /* 0x00 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x08 */ 0,    0,    0,    0,    0x0C, 0x0D, 0x0F, 0x10,   // minus 0x0C, hat 0x0D, backspace 0x0F, tab 0x10
    /* 0x10 */ 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,   // Q W E R T Y U I
    /* 0x18 */ 0x19, 0x1A, 0x1C, 0x29, 0x1D, 0x52, 0x1E, 0x1F,   // O P [ ] return ctrl A S
    /* 0x20 */ 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,   // D F G H J K L ;
    /* 0x28 */ 0x28, 0,    0x53, 0x0E, 0x2A, 0x2B, 0x2C, 0x2D,   // ' becomes colon 0x28, shift 0x53, Z X C V
    /* 0x30 */ 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x53, 0,      // B N M , . / right shift
    /* 0x38 */ 0,    0x35, 0x55, 0x5D, 0x5E, 0x5F, 0x60, 0x61,   // space 0x35, caps 0x55, PF01..PF05
    /* 0x40 */ 0x62, 0x63, 0x64, 0x65, 0x66, 0,    0,    0x4E,   // PF06..PF10, home 0x4E
    /* 0x48 */ 0x4D, 0x6E, 0,    0x4F, 0,    0x51, 0,    0x70,   // up 0x4D, prev, left 0x4F, right 0x51, next
    /* 0x50 */ 0x50, 0x70, 0x48, 0x4B, 0,    0,    0,    0,      // down 0x50, insert 0x48, delete 0x4B
    /* 0x58 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x60 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x68 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x70 */ 0,    0,    0,    0,    0,    0,    0,    0,
};

static bool towns_key(const SDL_KeyboardEvent& k, uint8_t& scan, uint8_t& ascii) {
    switch (k.scancode) {
    case SDL_SCANCODE_ESCAPE: scan = 0x01; break;
    case SDL_SCANCODE_1: scan = 0x02; break;  case SDL_SCANCODE_2: scan = 0x03; break;
    case SDL_SCANCODE_3: scan = 0x04; break;  case SDL_SCANCODE_4: scan = 0x05; break;
    case SDL_SCANCODE_5: scan = 0x06; break;  case SDL_SCANCODE_6: scan = 0x07; break;
    case SDL_SCANCODE_7: scan = 0x08; break;  case SDL_SCANCODE_8: scan = 0x09; break;
    case SDL_SCANCODE_9: scan = 0x0A; break;  case SDL_SCANCODE_0: scan = 0x0B; break;
    case SDL_SCANCODE_MINUS: scan = 0x0C; break;      case SDL_SCANCODE_EQUALS: scan = 0x0D; break;
    case SDL_SCANCODE_BACKSPACE: scan = 0x0E; break;  case SDL_SCANCODE_TAB: scan = 0x0F; break;
    case SDL_SCANCODE_Q: scan = 0x10; break; case SDL_SCANCODE_W: scan = 0x11; break;
    case SDL_SCANCODE_E: scan = 0x12; break; case SDL_SCANCODE_R: scan = 0x13; break;
    case SDL_SCANCODE_T: scan = 0x14; break; case SDL_SCANCODE_Y: scan = 0x15; break;
    case SDL_SCANCODE_U: scan = 0x16; break; case SDL_SCANCODE_I: scan = 0x17; break;
    case SDL_SCANCODE_O: scan = 0x18; break; case SDL_SCANCODE_P: scan = 0x19; break;
    case SDL_SCANCODE_LEFTBRACKET: scan = 0x1A; break;  case SDL_SCANCODE_RIGHTBRACKET: scan = 0x1B; break;
    case SDL_SCANCODE_RETURN: scan = 0x1C; break;      case SDL_SCANCODE_LCTRL: scan = 0x1D; break;
    case SDL_SCANCODE_A: scan = 0x1E; break; case SDL_SCANCODE_S: scan = 0x1F; break;
    case SDL_SCANCODE_D: scan = 0x20; break; case SDL_SCANCODE_F: scan = 0x21; break;
    case SDL_SCANCODE_G: scan = 0x22; break; case SDL_SCANCODE_H: scan = 0x23; break;
    case SDL_SCANCODE_J: scan = 0x24; break; case SDL_SCANCODE_K: scan = 0x25; break;
    case SDL_SCANCODE_L: scan = 0x26; break; case SDL_SCANCODE_SEMICOLON: scan = 0x27; break;
    case SDL_SCANCODE_APOSTROPHE: scan = 0x28; break;   case SDL_SCANCODE_GRAVE: scan = 0x29; break;
    case SDL_SCANCODE_LSHIFT: scan = 0x2A; break;      case SDL_SCANCODE_BACKSLASH: scan = 0x2B; break;
    case SDL_SCANCODE_Z: scan = 0x2C; break; case SDL_SCANCODE_X: scan = 0x2D; break;
    case SDL_SCANCODE_C: scan = 0x2E; break; case SDL_SCANCODE_V: scan = 0x2F; break;
    case SDL_SCANCODE_B: scan = 0x30; break; case SDL_SCANCODE_N: scan = 0x31; break;
    case SDL_SCANCODE_M: scan = 0x32; break; case SDL_SCANCODE_COMMA: scan = 0x33; break;
    case SDL_SCANCODE_PERIOD: scan = 0x34; break;      case SDL_SCANCODE_SLASH: scan = 0x35; break;
    case SDL_SCANCODE_RSHIFT: scan = 0x36; break;      case SDL_SCANCODE_LALT: scan = 0x38; break;
    case SDL_SCANCODE_SPACE: scan = 0x39; break;       case SDL_SCANCODE_CAPSLOCK: scan = 0x3A; break;
    case SDL_SCANCODE_F1: scan = 0x3B; break;  case SDL_SCANCODE_F2: scan = 0x3C; break;
    case SDL_SCANCODE_F3: scan = 0x3D; break;  case SDL_SCANCODE_F4: scan = 0x3E; break;
    case SDL_SCANCODE_F5: scan = 0x3F; break;  case SDL_SCANCODE_F6: scan = 0x40; break;
    case SDL_SCANCODE_F7: scan = 0x41; break;  case SDL_SCANCODE_F8: scan = 0x42; break;
    case SDL_SCANCODE_F9: scan = 0x43; break;  case SDL_SCANCODE_F10: scan = 0x44; break;
    case SDL_SCANCODE_HOME: scan = 0x47; break; case SDL_SCANCODE_UP: scan = 0x48; break;
    case SDL_SCANCODE_PAGEUP: scan = 0x49; break;     case SDL_SCANCODE_LEFT: scan = 0x4B; break;
    case SDL_SCANCODE_RIGHT: scan = 0x4D; break;      case SDL_SCANCODE_END: scan = 0x4F; break;
    case SDL_SCANCODE_DOWN: scan = 0x50; break;       case SDL_SCANCODE_PAGEDOWN: scan = 0x51; break;
    case SDL_SCANCODE_INSERT: scan = 0x52; break;     case SDL_SCANCODE_DELETE: scan = 0x53; break;
    default: return false;
    }
    // PC/AT set 1 as SDL reports it, now as the code the Towns keyboard would have sent for the same key.
    scan = kPcToTowns[scan & 0x7F];
    if (!scan) return false;
    ascii = (k.key >= 0x20 && k.key < 0x7F) ? (uint8_t)k.key : 0;
    return true;
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
            // PATCH, this repository: after the front-end's own keys, hand the keystroke to the program as the
            // Towns' keyboard would, when a caller asked for it. Escape has already quit above, deliberately:
            // a window needs a way out that the guest cannot swallow.
            if (on_key) {
                uint8_t scan = 0, ascii = 0;
                if (towns_key(e.key, scan, ascii)) on_key(scan, ascii);
            }
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
