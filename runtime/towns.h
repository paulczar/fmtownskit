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

struct DiscFile { std::string name; uint32_t lba, size; bool dir; };

class Disc {
public:
    bool open(const std::string& cue, std::string& err);
    const std::vector<Track>& tracks() const { return tracks_; }
    bool read_data(uint32_t sector, uint8_t* out);                 // one 2,048-byte sector
    bool read_bytes(uint32_t lba, uint32_t off, uint32_t n, uint8_t* out);
    bool find(const std::string& path, DiscFile& f);              // "\\DIR\\NAME.EXT", any case
    bool file(const std::string& path, std::vector<uint8_t>& out);
    // 16-bit stereo samples of the disc's audio sector (counted as lba() counts); false if none
    bool read_audio(uint32_t sector, uint8_t* out2352);
private:
    std::vector<Track> tracks_;
    const Track* data_ = nullptr;
    std::vector<DiscFile> files_;
    bool walked_ = false;
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
    uint8_t layer_ctrl = 0x0F;          // 0xFDA0
    uint8_t display_plane = 0, display_page = 0, vram_page = 0;  // 0xFF82, 0xFF83
    uint8_t wplane = 0, rplane = 0;     // 0xFF81
    uint8_t mask_sel = 0, mask[4] = {0xFF, 0xFF, 0xFF, 0xFF};   // 0x458/0x45A/0x45B
    uint8_t degipal[8] = {};
    bool dpmd = false;
};

struct Sound {                    // registers only, so far
    uint8_t opn_addr[2] = {}, opn[2][256] = {};
    uint8_t pcm_reg[16] = {}, pcm_ctrl = 0, pcm_onoff = 0xFF, pcm_mask = 0, pcm_flags = 0;
    uint8_t volume[4] = {}, volume_sel = 0;
    std::vector<uint8_t> wave = std::vector<uint8_t>(65536);
};

struct CdState { bool playing = false; uint32_t from = 0, to = 0; uint8_t repeat = 0; };

struct IntVector { uint16_t sel = 0; uint32_t off = 0; bool set = false; };

// A service the runtime does not have: logged once per kind, and counted.
void unknown(const char* fmt, ...);

class Machine {
public:
    Machine();
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
    uint64_t now = 0;                   // guest clocks run (the CPU's c.clocks)
    uint64_t next_vsync = kFrameClocks;
    uint64_t frame = 0;                 // VSYNCs so far
    bool vblank = false;
    uint64_t vblank_end = 0;
    Pic pic[2];
    IntVector pm_vector[256];           // Phar Lap 2506h: always in protected mode
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
    std::vector<uint8_t> vram = std::vector<uint8_t>(0x80000);
    std::vector<uint8_t> sprram = std::vector<uint8_t>(0x20000);
    uint8_t pad_mask = 0;
    uint8_t pad[2] = {0x7F, 0x7F};      // active low: bits 0-3 up down left right, 4 A, 5 B
    void vram_pages();                  // map VRAM's pages as host memory, or not (write mask)
    void draw_sprites();
    // the frame as MAME's screen_update draws it: width x height of 0xRRGGBB
    void render(std::vector<uint32_t>& rgb, int& width, int& height);

    // DOS, Phar Lap, the CD BIOS
    Disc disc;
    std::string save_dir = "save";
    char drive = 'Q';
    uint32_t next_block = 0;            // the next int 21h AH=48h block's linear address
    uint16_t next_ldt = 0x44;
    uint32_t rm_buffer = 0x408C0;       // Phar Lap's real-mode call buffer (linear; 250Dh)
    struct Handle { bool used = false; bool host = false; FILE* f = nullptr;
                    std::vector<uint8_t> data; uint32_t pos = 0; };
    std::vector<Handle> handles = std::vector<Handle>(32);
    bool exited = false;
    int exit_code = 0;
    void int21(X86Cpu& c);
    void int93(X86Cpu& c, uint32_t eax, uint32_t& ret_eax, uint32_t rm_ds, uint32_t rm_es);
    void intfd(X86Cpu& c);

    // the host's turn at each VSYNC (headless runs save frames here, and may stop the run by
    // throwing Stop)
    std::function<void(Machine&)> on_vsync;

    // tracing
    bool trace_io = false, trace_int = false;
    std::vector<uint8_t> font;          // FMT_FNT.ROM if given (not needed so far)
};

extern Machine* g_towns;

struct Stop { int code; };          // thrown to end a run: the program's exit, or the host's
void pcm_take(std::vector<int16_t>& out);   // the RF5C68's stereo samples since the last take

// RUN386's load of a P3 program: the image at linear `base` under code `cs` and data `cs + 8`,
// the stack, the registers as RUN386 hands them over (read on the Towns at the first
// instruction: every general register 0, EFLAGS 0x202, DS ES FS GS SS the data selector).
struct P3 { uint32_t image_offset, image_size, base_offset, eip, esp, min_extra, max_extra;
            std::vector<uint8_t> image; };
bool parse_p3(const std::vector<uint8_t>& file, P3& p, std::string& err);
void boot_run386(Machine& m, X86Cpu& c, const P3& p, const std::vector<uint8_t>& tbios);

// PNG, 8-bit RGB, no compression
bool write_png(const std::string& path, const std::vector<uint32_t>& rgb, int w, int h);

}  // namespace towns
