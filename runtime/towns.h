// fmtownskit runtime -- the FM Towns under recompiled code: what a program run by RUN386
// sees of the machine.
//
// The CPU is x86.h's; this is the rest. Linear memory as RUN386 leaves it (paging on, the
// Towns' addresses 1:1: the Marty's remapping is invisible to a program, see towns.py), the
// descriptors a program loads (towns.SELECTORS), the I/O ports the program and TBIOS touch,
// the interrupt controllers with the program's own protected-mode handlers, VSYNC and the
// sprite controller on a guest clock, and the services a Towns program calls by interrupt:
// DOS and Phar Lap (int 21h), the CD BIOS (int 93h), answered here over the disc image.
//
// One machine at a time (g_towns); the recompiled code reaches it through x86.h's services.
#pragma once
#include "x86.h"
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace towns {

constexpr uint32_t kClock = 16000000;           // the 386DX of the Model 1/2 (MAME's fmtowns)
constexpr uint32_t kFrameClocks = kClock / 60;  // VSYNC at 60 Hz (MAME's screen)

// ---- the machine a run stands in for ------------------------------------------------------
// What RUN386 and TownsOS leave differently by model, RAM and the disc's own boot (a driver
// kept at the top of RAM moves the limits), read from the oracle's dumps: towns.PROFILES has
// the figures read so far and where. The default is MAME's fmtowns under the first game; a
// port whose boot differs declares its own from its dumps.
struct Profile {
    const char* name;
    uint32_t ram_mb;            // port 0x5E8
    uint32_t prog_base;         // the program's segment (0x0C, 0x14), linear
    uint32_t prog_phys;         // ... and physical (Phar Lap 250Fh makes real-mode addresses of it)
    uint32_t prog_limit_all;    // its limit when the P3 asks for all memory (max extra 0xFFFFFFFF;
                                // 0: not read, such a program is refused)
    uint32_t ram_limit;         // selector 0x40: all of RAM
};
extern const Profile kTowns2M;  // MAME's fmtowns as it comes: 2 MB

// ---- the disc: a cue sheet and its track files, the data track's ISO 9660 ----------------
struct Track {
    int number = 0;
    bool audio = false;
    int sector_size = 2352;     // 2352 raw, 2048 cooked
    std::string path;
    uint32_t file_start = 0;    // the disc sector of its file's first sector
    int32_t index0 = -1, index1 = 0;  // sectors inside its file
    uint32_t sectors = 0;       // INDEX 01 to the next track or the file's end
    uint32_t lba() const { return file_start + (uint32_t)index1; }
};

struct DiscFile { std::string name; uint32_t lba, size; bool dir; uint16_t time = 0, date = 0; };  // DOS's

class Disc {
public:
    bool open(const std::string& cue, std::string& err);
    const std::vector<Track>& tracks() const { return tracks_; }
    bool read_data(uint32_t sector, uint8_t* out);                 // one 2,048-byte sector
    bool read_bytes(uint32_t lba, uint32_t off, uint32_t n, uint8_t* out);
    bool find(const std::string& path, DiscFile& f);              // "\\DIR\\NAME.EXT", any case
    bool file(const std::string& path, std::vector<uint8_t>& out);
    // the entries of a directory ("" the root), named within it, in the disc's order; false if
    // there is no such directory
    bool list(const std::string& dir, std::vector<DiscFile>& out);
    // 16-bit stereo samples of the disc's audio sector (counted as lba() counts); false if none
    bool read_audio(uint32_t sector, uint8_t* out2352);
private:
    std::vector<Track> tracks_;
    const Track* data_ = nullptr;
    std::vector<DiscFile> files_;
    bool walked_ = false;
    void index();
    void walk(uint32_t lba, uint32_t size, const std::string& prefix, int depth);
    std::vector<std::pair<std::string, FILE*>> handles_;
    FILE* handle(const std::string& path);
};

// ---- the machine ---------------------------------------------------------------------------
struct Descriptor { uint32_t base = 0, limit = 0; bool valid = false; };

struct Pic {                     // an 8259A as the Towns programs it
    uint8_t irr = 0, isr = 0, imr = 0xFF, base = 0;
    uint8_t icw_step = 0, read_isr = 0, icw4 = 0;
};

struct Sprite { uint8_t reg[8] = {}; uint8_t sel = 0; bool busy = false; int page = 0;
                uint64_t done_at = 0; };

struct Video {
    uint16_t crtc[32] = {};
    uint8_t crtc_sel = 0;
    uint8_t vreg[2] = {}, vsel = 0;     // the video output registers (0x448/0x44A)
    uint8_t pal_sel = 0;                // 0xFD90
    uint8_t pal16[2][16][3] = {}, pal256[256][3] = {};   // r g b
    uint8_t pal_reg[256][3] = {};       // the colour registers 0xFD92-0xFD96 write (r g b), by entry
    uint8_t layer_ctrl = 0x0F;          // 0xFDA0
    uint8_t display_plane = 0, display_page = 0, vram_page = 0;  // 0xFF82, 0xFF83
    uint8_t wplane = 0, rplane = 0;     // 0xFF81
    uint8_t mask_sel = 0, mask[4] = {0xFF, 0xFF, 0xFF, 0xFF};   // 0x458/0x45A/0x45B
    uint8_t degipal[8] = {};
    bool dpmd = false;
};

// The sound's registers as the ports show them; the chips themselves are towns_sound.cpp's.
struct Sound {
    uint8_t pcm_ctrl = 0, pcm_onoff = 0xFF, pcm_mask = 0, pcm_flags = 0;   // 0x4F7, 0x4F8, 0x4EA, 0x4EB
    bool pcm_irq = false, fm_irq = false;   // the two halves of IRQ13 (0x4E9 bits 3 and 0)
    uint8_t volume[4] = {}, volume_sel = 0; // the output volumes (0x4E2/0x4E3)
    uint8_t in_volume[4] = {}, in_sel = 0;  // the input volumes (0x4E0/0x4E1): kept, not heard
    float cd_gain[2] = {1, 1};              // the CD's audio, left and right, as the volumes set it
    std::vector<uint8_t> wave = std::vector<uint8_t>(65536);  // the RF5C68's wave RAM
};

// The CD's audio: a play from a sector to a sector (both included, as MAME's CD plays them).
struct CdState { bool playing = false, ended = false; uint32_t from = 0, to = 0, lba = 0, left = 0;
                 uint8_t repeat = 0; };

constexpr int kAudioRate = 48000;   // the mix's rate (MAME's -samplerate default: the oracle's WAV)

struct SoundChips;                  // towns_sound.cpp
struct SoundChipsFree { void operator()(SoundChips*) const; };

struct IntVector { uint16_t sel = 0; uint32_t off = 0; bool set = false; };

enum : unsigned { kPadUp = 1, kPadDown = 2, kPadLeft = 4, kPadRight = 8, kPadA = 16, kPadB = 32,
                  kPadRun = 64, kPadSelect = 128 };

// A service the runtime does not have: logged once per kind, and counted.
void unknown(const char* fmt, ...);

class Machine {
public:
    Machine();
    const Profile* profile = &kTowns2M; // set before boot_run386
    ~Machine();
    void townsos();                     // the machine as TownsOS hands it to a program

    // memory: host pages for the linear ranges that are plain memory
    void map_ram(uint32_t linear, uint32_t size);           // zero-filled
    uint8_t* host(uint32_t linear);                         // the byte's host address, or null
    void write_block(uint32_t linear, const uint8_t* p, uint32_t n);
    void read_block(uint32_t linear, uint8_t* p, uint32_t n);
    uint32_t mem_read(uint32_t a, int n);
    void mem_write(uint32_t a, int n, uint32_t v);

    // descriptors (selector & ~3)
    Descriptor desc[0x2000 >> 2];
    void set_desc(uint16_t sel, uint32_t base, uint32_t limit);
    uint16_t alloc_ldt(uint32_t base, uint32_t limit);       // int 21h AH=48h

    // ports
    uint32_t in(uint32_t port, int n);
    void out(uint32_t port, int n, uint32_t v);
    uint8_t in8(uint32_t port);
    void out8(uint32_t port, uint8_t v);

    // time and interrupts
    uint64_t now = 0;                   // the machine's clocks run (kClock a second)
    uint64_t next_vsync = kFrameClocks;
    // The CPU's rate against the machine's clock, in percent: 100 runs the 386 at kClock, as
    // MAME's i386 (the datasheet's clocks, no wait states); less gives each of its clocks more
    // of the machine's time -- the devices, the timers and the sound keep theirs. The CPU's
    // count (c.clocks) and the machine's (`now`) are the same at 100.
    int cpu_rate = 100;
    uint64_t rate_cpu0 = 0, rate_now0 = 0;      // where the rate last changed, on both counts
    uint64_t machine_time(uint64_t cpu_clocks) const { return rate_now0 + (cpu_clocks - rate_cpu0) * 100 / (uint64_t)cpu_rate; }
    uint64_t cpu_time(uint64_t t) const { return rate_cpu0 + ((t - rate_now0) * (uint64_t)cpu_rate + 99) / 100; }
    void set_cpu_rate(int percent);     // from `now` on (the CPU's count there is cpu_time(now))
    uint64_t frame = 0;                 // VSYNCs so far
    bool vblank = false;
    uint64_t vblank_end = 0;
    Pic pic[2];
    IntVector pm_vector[256];           // Phar Lap 2504h, 2506h, 2507h: taken in protected mode
    uint32_t rm_vector[256] = {};       // Phar Lap 2505h, 2507h: kept, never taken
    void raise_irq(int irq, bool level);
    int pending_irq();                  // the vector to take, or -1
    void advance(uint64_t to);          // run the machine's clocks up to `to`
    uint64_t next_event() const;
    void beam(int& hpos, int& vpos) const;

    // devices
    Video video;
    Sprite sprite;
    Sound sound;
    CdState cd;
    // the sound (towns_sound.cpp): the RF5C68, the YM3438 (ymfm's), the CD's audio, their mix
    std::unique_ptr<SoundChips, SoundChipsFree> chips;
    void sound_start();
    uint8_t sound_in8(uint32_t port);
    void sound_out8(uint32_t port, uint8_t d);
    uint64_t sound_next_event() const;
    void sound_run(uint64_t to);        // the chips' events due by `to`
    void sound_irq();                   // IRQ13 from the FM's and the PCM's flags
    void cd_play(uint32_t from, uint32_t to);
    void cd_stop();
    uint32_t cd_sector();               // the sector playing (or paused at), up to `now`
    // the mix since the last take: kAudioRate stereo frames, up to about `now`
    void audio_take(std::vector<int16_t>& out);
    uint8_t audio_mute = 0;             // sources left out of the mix: 1 the FM, 2 the PCM, 4 the CD
    std::vector<uint8_t> vram = std::vector<uint8_t>(0x80000);
    std::vector<uint8_t> sprram = std::vector<uint8_t>(0x20000);
    uint8_t pad_mask = 0;
    uint8_t pad[2] = {0x7F, 0x7F};      // active low: bits 0-3 up down left right, 4 A, 5 B
    // the Towns pad as the player holds it (kPad* bits): Run and Select are wired as left with
    // right and up with down, as MAME's townspad has them
    void set_pad(int port, unsigned held);
    unsigned pad_held[2] = {};          // what set_pad was last given (a host's recording)
    // the mouse in the second port, as MAME's fmtowns has one there (an MSX mouse: each change
    // of its pin 8 -- 0x4D6 bit 5 -- gives the next nibble of the X and Y movement since the
    // last read, a pause of over 3 ms starting again from the first); `x`, `y` where the host
    // has moved it, `buttons` active low in bits 4 and 5
    struct Mouse { bool present = true; uint16_t data = 0; uint8_t stat = 3, pin8 = 0;
                   uint64_t last = 0; int16_t read_x = 0, read_y = 0, x = 0, y = 0;
                   uint8_t buttons = 0xFF; } mouse;
    void vram_pages();                  // map VRAM's pages as host memory, or not (write mask)
    // a diagnostic of where the CPU's time goes: every access to VRAM and sprite RAM (their
    // pages then left unmapped, so each goes through mem_read/mem_write) and to the ports, counted
    // (among them the pads' reads, 0x4D0-0x4D3, and the 1 us waits written to 0x6C)
    struct AccessCount { uint64_t vram_r = 0, vram_w = 0, spr_r = 0, spr_w = 0, io_r = 0, io_w = 0, pad_r = 0, wait_w = 0; } access;
    bool count_access = false;
    void set_count_access(bool on);
    void draw_sprites();
    // the frame as MAME's screen_update draws it: width x height of 0xRRGGBB
    void render(std::vector<uint32_t>& rgb, int& width, int& height);
    // where the picture is in that frame: the two layers' windows as the CRTC sets them, joined
    // (the rest is the border MAME's snapshot keeps); the window shows only this
    void display_area(int& x, int& y, int& w, int& h) const;

    // DOS, Phar Lap, the CD BIOS
    Disc disc;
    std::string save_dir = "save";
    char drive = 'Q';
    std::string cd_dir;                 // the CD's current directory, "" its root (ABEL's START: "ABEL\OP")
    uint32_t tbios_size = 0;            // TBIOS's image at 0x100000 (selectors 0x110, 0x118)
    uint32_t next_block = 0;          // the next int 21h AH=48h block's linear address
    uint16_t next_ldt = 0x44;
    uint32_t rm_buffer = 0x408C0;       // Phar Lap's real-mode call buffer (linear; 250Dh)
    struct Handle { bool used = false; bool host = false; FILE* f = nullptr;
                    std::vector<uint8_t> data; uint32_t pos = 0; };
    std::vector<Handle> handles = std::vector<Handle>(32);
    uint32_t dta = 0, dta_off = 0;      // the disk transfer area (1Ah), linear, and as DS:EDX gave it
    uint16_t dta_sel = 0;
    std::vector<std::string> search_dirs;   // the directories 4Eh searched (the DTA keeps an index)
    bool exited = false;
    int exit_code = 0;
    // DOS's date and time (2Ah, 2Ch): the host's clock, as MAME's RTC is, or with `clock_start`
    // set (seconds since 1970, the host's local time) that moment plus the machine's time run:
    // a run that can be played again the same
    int64_t clock_start = 0;
    void int21(X86Cpu& c);
    // the CD BIOS: `buf` the linear address of the call's buffer (DS:DI in real mode through
    // Phar Lap's 2511h; DS:EDI when the program raises int 93h itself)
    void int93(X86Cpu& c, uint32_t eax, uint32_t& ret_eax, uint32_t buf);
    bool cd_paused = false;
    void int90(X86Cpu& c);              // the keyboard BIOS (KYB_*), as FORRBIOS.NSD answers it
    // PATCH, this repository: the keyboard the kit did not have. Its recordings were taken with no key ever
    // pressed, so KYB_read (AH=09h) answered "none" unconditionally and a program waiting for a key waited
    // for ever. The pair is (scan code, ASCII), and the measured contract is in towns_dos.cpp's int90.
    void press_key(uint8_t scan, uint8_t ascii);
    struct Key { uint8_t scan, ascii; };
    std::vector<Key> keys;              // pending presses, served in order by KYB_read
    unsigned long keys_served = 0;      // how many KYB_read calls took one, for reports
    void int8e(X86Cpu& c);              // KimG (the kanji input manager), as FORRBIOS.NSD answers it
    void intfd(X86Cpu& c);
    // the timer BIOS, int 97h (the ROM's, which FORRBIOS.NSD hands protected-mode calls to; read
    // on the oracle with ABEL's OPNG, towns_dos.cpp): timers counting 10 ms ticks, each with a
    // limit and a far callback, called at the first safe point with IF set once the limit is met
    struct Timer97 { bool on = false, due = false; uint8_t mode = 0; uint16_t sel = 0; uint32_t off = 0, limit = 0, count = 0; };
    Timer97 t97[8];
    uint64_t next_t97 = 0;              // the next 10 ms tick, while a timer is on
    void int97(X86Cpu& c);
    int t97_due();                      // a timer whose callback is due (cleared), or -1

    // the host's turn at each VSYNC (headless runs save frames here, and may stop the run by
    // throwing Stop)
    std::function<void(Machine&)> on_vsync;

    // tracing
    bool trace_io = false, trace_int = false;
    std::vector<uint8_t> font;          // FMT_FNT.ROM if given (not needed so far)
};

extern Machine* g_towns;

struct Stop { int code; };          // thrown to end a run: the program's exit, or the host's

// RUN386's load of a P3 program: the image at linear `base` under code `cs` and data `cs + 8`,
// the stack, the registers as RUN386 hands them over (read on the Towns at the first
// instruction: every general register 0, EFLAGS 0x202, DS ES FS GS SS the data selector).
struct P3 { uint32_t image_offset, image_size, base_offset, eip, esp, min_extra, max_extra;
            std::vector<uint8_t> image; };
bool parse_p3(const std::vector<uint8_t>& file, P3& p, std::string& err);   // bound behind an MZ stub too
// TBIOS composed from its modules as TBIOSLD.SYS does, from the disc's TBIOS.INI (e.g. "TBIOS/TBIOS.INI")
bool compose_tbios(Disc& disc, const std::string& ini, std::vector<uint8_t>& image, std::string& err);
void boot_run386(Machine& m, X86Cpu& c, const P3& p, const std::vector<uint8_t>& tbios);   // the two below
void boot_towns(Machine& m, const std::vector<uint8_t>& tbios);    // low RAM, TBIOS at 0x100000
void run386_load(Machine& m, X86Cpu& c, const P3& p);             // a program, as RUN386 starts it
void run386_end(Machine& m);                                      // what its end undoes, for the next

// PNG, 8-bit RGB, no compression
bool write_png(const std::string& path, const std::vector<uint32_t>& rgb, int w, int h);

// A 16-bit stereo WAV at kAudioRate, written as it grows (its header patched at close).
class WavWriter {
public:
    bool open(const std::string& path);
    void write(const std::vector<int16_t>& lr);
    void close();
    ~WavWriter() { close(); }
private:
    FILE* f_ = nullptr;
    uint32_t bytes_ = 0;
};

// The oracle's sound without the game: a sound log (a port's oracle writes it: every write to
// ports 0x4C0-0x4FF as "MICROSECONDS OFFSET DATA MASK", every write to the wave RAM as
// "MICROSECONDS m ADDRESS DATA MASK", timed from power-on) played into a fresh machine's chips at
// its times, the CD controller's play (0x04, the MSFs in its parameters, 1 ms later) and stop
// (0x84, 0x85) as MAME's driver takes them, the mix written from power-on for `seconds`.
bool replay_sound_log(Machine& m, const std::string& log, double seconds, WavWriter& wav, std::string& err);

}  // namespace towns
