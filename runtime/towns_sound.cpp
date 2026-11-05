// fmtownskit runtime -- the sound: the RF5C68 (eight PCM channels over 64 KB of wave RAM), the
// YM3438 (six FM channels: ymfm's core, BSD-3, in ymfm/), the CD's audio from the disc image,
// and their mix.
//
// As MAME's fmtowns has them (read, not copied: fmtowns.cpp, rf5c68.cpp, cdda.cpp): both chips
// at 8 MHz (MAME's guess), so the RF5C68 at 8 MHz / 384 and the YM3438 at 8 MHz / 144, as
// whole rates the way MAME's streams take them (20,833 and 55,555 Hz: the true 20,833.3 would
// drift 16 ppm from the oracle, a sample at 48 kHz every 1.2 s); the CD at 44,100 Hz; into the speakers FM and PCM at 1.0, the CD at 0.30
// times its volume (ports 0x4E2/0x4E3, CD left and right). IRQ13 is the FM's timers and the
// PCM's block ends together (0x4E9 shows which, reading 0x4EB clears the PCM's).
//
// Every source runs on the guest clock: the RF5C68 steps as an event (its interrupt must come
// on time), the YM3438 and the CD are brought up to date when touched and when the mix is
// taken, the YM3438's timers are events. The mix resamples each source to kAudioRate as MAME's
// streams do (each sample held, averaged over an output frame's span: held against the oracle's
// WAV, linear interpolation loses its highs, a plain hold has too many) and sums them as MAME's
// speaker does; a take gives every frame whose sources are all there, so it trails `now` by a
// sample or two.
#include "towns.h"
#include "ymfm_opn.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <numeric>

namespace towns {

static constexpr uint32_t kPcmRate = 8000000 / 384;     // 20,833
static constexpr uint32_t kFmRate = 8000000 / 144;      // 55,555
static constexpr uint32_t kCdRate = 44100;
static constexpr uint64_t kNever = ~0ull;

// sample n of a source at `rate`, in guest clocks
static uint64_t at(uint64_t n, uint32_t rate) { return n * kClock / rate; }

// A source's samples at its own rate, waiting for the mix: sample n at at(n, rate).
struct Stream {
    uint64_t a = 1, b = 1;          // output frame k is at source position k * a / b
    uint64_t first = 0;             // the index of buf's first sample
    std::vector<float> buf;         // left, right
    explicit Stream(uint32_t rate) {
        a = rate; b = kAudioRate;
        uint64_t g = std::gcd(a, b); a /= g; b /= g;
    }
    uint64_t count() const { return first + buf.size() / 2; }
    void push(float l, float r) { buf.push_back(l); buf.push_back(r); }
    void drop_to(uint64_t i) {
        if (i <= first) return;
        size_t k = std::min<size_t>((size_t)(i - first) * 2, buf.size());
        buf.erase(buf.begin(), buf.begin() + (long)k);
        first += k / 2;
    }
};

struct PcmChannel {
    uint8_t env = 0, pan = 0, start = 0;
    uint16_t step = 0, loopst = 0;
    uint32_t addr = 0;
    bool on = false;
    // Diagnostics only: how many times this channel has run off the end and wrapped since the CPU last touched it. A
    // one-shot effect that keeps wrapping is the shape of "the fight sound repeats until something else plays", and the
    // question worth answering is whether the end flag reached the driver at all.
    unsigned wraps = 0;
};

// ymfm's view of the machine: its timers and busy flag on the guest clock (its durations are
// the chip's 8 MHz clocks, two of the CPU's each), its interrupt into IRQ13.
struct FmPort : ymfm::ymfm_interface {
    Machine* m = nullptr;
    uint64_t at = 0;                    // the guest time of the chip's current access
    uint64_t timer[2] = {kNever, kNever};
    uint64_t busy_until = 0;
    // Instrument only: the first few timer schedules and cancels, with their sign. It is what showed that ymfm does schedule
    // this driver's timers (positive durations) and also cancels them, and the distinction is invisible in a count of writes.
    void ymfm_set_timer(uint32_t t, int32_t clocks) override {
        static unsigned pos = 0, neg = 0;
        bool show = false;
        if (clocks >= 0) {
            if (pos++ < 5) show = true;
        } else if (neg++ < 3) {
            show = true;
        }
        if (show && m && m->trace_int)
            std::fprintf(stderr, "  ymfm_set_timer(%u, %d) at frame %llu [pos=%u neg=%u]\n", t, clocks,
                         (unsigned long long)m->frame, pos, neg);
        timer[t] = clocks < 0 ? kNever : at + (uint64_t)clocks * 2;
    }
    void ymfm_set_busy_end(uint32_t clocks) override { busy_until = at + (uint64_t)clocks * 2; }
    bool ymfm_is_busy() override { return at < busy_until; }
    void ymfm_update_irq(bool asserted) override {
        // The link between arming a timer and the handler running is where the music stops, so it gets a line of its own. If
        // these appear at frame 97 and no interrupt is ever taken, the request exists and delivery does not, which is a
        // different fault from the driver never asking.
        if (m && m->trace_int) {
            static unsigned n = 0;
            if (n++ < 12)
                std::fprintf(stderr, "  FM IRQ %s at frame %llu\n", asserted ? "asserted" : "released",
                             (unsigned long long)m->frame);
        }
        if (m) { m->sound.fm_irq = asserted; m->sound_irq(); }
    }
    void expire(uint32_t t) { m_engine->engine_timer_expired(t); }
};

struct SoundChips {
    // the RF5C68
    PcmChannel pcm[8];
    uint8_t cbank = 0;
    uint8_t fm_addr[2] = {};            // instrument only: the last address written to each FM chip
    uint64_t pcm_n = 0, pcm_next = 0;   // the next sample, and its time
    Stream pcm_out{kPcmRate};
    // the YM3438
    FmPort port;
    ymfm::ym3438 fm{port};
    uint64_t fm_n = 0, fm_next = 0;
    Stream fm_out{kFmRate};
    // the CD
    uint64_t cd_n = 0;                  // the CD's next sample
    uint8_t sector[2352] = {};
    int sector_pos = 588;               // the next sample in `sector` (588 a sector)
    Stream cd_out{kCdRate};
    // the mix
    uint64_t k = 0;                     // the next output frame
    // Diagnostics: how many NON-ZERO samples each source produced in the current second. The mixer can be silent while a
    // chip is busy, and the census of register writes cannot tell which source is the silent one. This can.
    uint64_t nz_second = ~0ULL;
    uint64_t nz[3] = {0, 0, 0};          // FM, PCM, CD
};

void SoundChipsFree::operator()(SoundChips* p) const { delete p; }

void Machine::sound_start() {
    chips.reset(new SoundChips);
    chips->port.m = this;
    // NO fm.reset() HERE. A fresh ym3438 needs none, and on one it silences the chip permanently: the same controlled patch
    // gives a peak of 5482 without it and 0 with it, measured both ways. That is why music, menu beeps and the footstep sound
    // were all missing while sample effects worked.
}

// Counts the diagnostic lines the sound instruments print, so one session cannot flood the log with them. File scope
// rather than a member: it is a property of the instrument, not of the machine, and it must not become machine state.
static unsigned pcm_notes = 0;

void Machine::sound_irq() {
    if (trace_int && sound.pcm_irq && pcm_notes++ < 60)
        std::fprintf(stderr, "  PCM irq asserted at frame %llu\n", (unsigned long long)frame);
    raise_irq(13, sound.fm_irq || sound.pcm_irq);
}

// ---- the RF5C68 ------------------------------------------------------------------------------
static void pcm_sample(Machine& m, SoundChips& s) {
    Sound& snd = m.sound;
    int left = 0, right = 0;
    for (int ch = 0; ch < 8 && (snd.pcm_ctrl & 0x80); ++ch) {
        PcmChannel& c = s.pcm[ch];
        if (!c.on) continue;
        int sample = snd.wave[(c.addr >> 11) & 0xFFFF];
        if (sample == 0xFF) {
            // END OF SAMPLE. The chip restarts at the loop address, and the driver is told through a per-channel flag, and
            // through the interrupt when the mask enables that channel. Evidence rather than inference: this game writes
            // PCM mask = 80 at frame 1084 while starting channel 7 (on/off = 7F clears bit 7), and channel 7 is the one
            // observed wrapping at frame 1197. So bit 7 is channel 7, the mask and the flag are per channel, and the event
            // is this marker. The previous version keyed the flag to a 4 KB bank boundary instead, which never arrives for
            // a sample shorter than 4 KB, and every one of these effects is: the driver was never told, so it never stopped
            // the channel and the chip looped the sound for ever. That was the fight sound that repeated until another
            // sound overwrote the channel.
            // THE MARKER ENDS THE SAMPLE. Looping here was measured in play on 2026-11-05 and loops for ever, because the driver does
            // not release a channel of its own accord: the ordered log shows start, marker, and then nothing until the NEXT sound's
            // stop write replaces it. So the stopping has to be ours. What is left open is why the marker arrives about twelve frames
            // into a sound that should last longer, which is the wave dump below.
            snd.pcm_flags |= (uint8_t)(1 << ch);
            if (snd.pcm_mask & (1 << ch)) {
                snd.pcm_irq = true;
                m.sound_irq();
            }
            if (m.trace_int && c.wraps++ < 1) {
                std::fprintf(stderr, "  PCM ch%d sample ended at frame %llu (start %u, loop %u, step %u)\n", ch,
                             (unsigned long long)m.frame, c.start, c.loopst, c.step);
                // The bytes the channel was actually reading, at the loop address, so a marker that arrives too early can be told from
                // a marker that is genuinely there. A wave RAM that is mostly FF says the mapping is wrong rather than the sound short.
                unsigned ff = 0, first = 0;
                bool found = false;
                for (unsigned i = 0; i < 0x10000; ++i) {
                    if (snd.wave[i] == 0xFF) {
                        ++ff;
                    } else if (!found) {
                        found = true;
                        first = i;
                    }
                }
                const unsigned at = (unsigned)c.loopst << 8;
                std::fprintf(stderr, "  WAVE read at %04X (start %u loop %u):", (unsigned)((c.addr >> 11) & 0xFFFF), c.start,
                             c.loopst);
                for (unsigned i = 0; i < 24; ++i)
                    std::fprintf(stderr, " %02X", snd.wave[(((unsigned)(c.addr >> 11)) + i) & 0xFFFF]);
                std::fprintf(stderr, "\n  WAVE ff %u of 65536, first non-FF at %04X, loop point %04X\n", ff, found ? first : 0xFFFFu,
                             at & 0xFFFF);
            }
            c.on = false;
            c.addr = (uint32_t)c.loopst << 11;
            continue;
        }
        c.addr += c.step;
        int lv = (c.pan & 0x0F) * c.env, rv = ((c.pan >> 4) & 0x0F) * c.env;
        if (sample & 0x80) { left += ((sample & 0x7F) * lv) >> 5; right += ((sample & 0x7F) * rv) >> 5; }
        else { left -= (sample * lv) >> 5; right -= (sample * rv) >> 5; }
    }
    // a 10-bit DAC
    left = std::clamp(left & ~63, -32768, 32767);
    right = std::clamp(right & ~63, -32768, 32767);
    s.pcm_out.push(left / 32768.0f, right / 32768.0f);
    if (left || right) ++s.nz[1];
}

// ---- the YM3438 ------------------------------------------------------------------------------
static void fm_catch_up(SoundChips& s, uint64_t to) {
    ymfm::ym3438::output_data o;
    while (s.fm_next <= to) {
        s.fm.generate(&o, 1);
        s.fm_out.push(o.data[0] / 32768.0f, o.data[1] / 32768.0f);
        if (o.data[0] || o.data[1]) ++s.nz[0];
        s.fm_next = at(++s.fm_n, kFmRate);
    }
}

// ---- the CD ----------------------------------------------------------------------------------
static void cd_catch_up(Machine& m, SoundChips& s, uint64_t to) {
    CdState& cd = m.cd;
    while (at(s.cd_n, kCdRate) <= to) {
        ++s.cd_n;
        if (!cd.playing) { s.cd_out.push(0, 0); continue; }
        if (s.sector_pos == 588) {
            if (!cd.left) { cd.playing = false; cd.ended = true; s.cd_out.push(0, 0); continue; }
            if (!m.disc.read_audio(cd.lba, s.sector)) std::memset(s.sector, 0, sizeof s.sector);
            ++cd.lba; --cd.left;
            s.sector_pos = 0;
        }
        const uint8_t* p = s.sector + s.sector_pos++ * 4;
        int16_t l = (int16_t)(p[0] | p[1] << 8), r = (int16_t)(p[2] | p[3] << 8);
        s.cd_out.push(l / 32768.0f * m.sound.cd_gain[0], r / 32768.0f * m.sound.cd_gain[1]);
        if (l || r) ++s.nz[2];
    }
}

void Machine::cd_play(uint32_t from, uint32_t to) {
    cd_catch_up(*this, *chips, now);
    cd.from = from; cd.to = to;
    cd.lba = from; cd.left = to >= from ? to - from + 1 : 0;
    cd.playing = true; cd.ended = false;
    chips->sector_pos = 588;
}

void Machine::cd_stop() {
    cd_catch_up(*this, *chips, now);
    cd.playing = false;
}

uint32_t Machine::cd_sector() {
    cd_catch_up(*this, *chips, now);
    return chips->sector_pos < 588 && cd.lba > cd.from ? cd.lba - 1 : cd.lba;
}

// ---- time ------------------------------------------------------------------------------------
uint64_t Machine::sound_next_event() const {
    const SoundChips& s = *chips;
    return std::min({s.pcm_next, s.port.timer[0], s.port.timer[1]});
}

// Counts the timer diagnostics so a session cannot flood the log.
static unsigned tmr_notes = 0;
static unsigned tmr_exp_notes = 0;
static unsigned status_notes = 0;   // separate: sharing one counter meant 40 status reads exhausted it before any expiry printed

void Machine::sound_run(uint64_t to) {
    SoundChips& s = *chips;
    for (;;) {
        uint64_t t = sound_next_event();
        if (t > to) return;
        if (t == s.pcm_next) {
            pcm_sample(*this, s);
            s.pcm_next = at(++s.pcm_n, kPcmRate);
            continue;
        }
        int n = t == s.port.timer[0] ? 0 : 1;
        fm_catch_up(s, t);
        s.port.timer[n] = kNever;
        s.port.at = t;
        if (trace_int && tmr_exp_notes++ < 20)
            std::fprintf(stderr, "  FM timer %d expired at frame %llu (now %llu)\n", n, (unsigned long long)frame,
                         (unsigned long long)now);
        s.port.expire(n);
    }
}

// ---- the ports -------------------------------------------------------------------------------
uint8_t Machine::sound_in8(uint32_t port) {
    SoundChips& s = *chips;
    switch (port) {
    case 0x4D8: case 0x4DA: case 0x4DC: case 0x4DE:
        fm_catch_up(s, now);
        s.port.at = now;
        // A read of the FM address port returns the chip's status: timer overflow flags and the busy bit. A driver that polls
        // the timers instead of taking their interrupt reads here, so counting these says which kind of driver this is.
        {
            const uint8_t st = s.fm.read((port - 0x4D8) >> 1);
            // DIAGNOSTIC, U2_FM_FLAGS=1: report both timer overflow flags as set on every status read.
            // ymfm sets a timer's overflow flag only when that timer's interrupt is enabled, while the hardware sets it on expiry
            // regardless and gates only the interrupt line. A driver that polls the flags rather than taking the interrupt sees a
            // chip that never finishes a timer under ymfm, which looks exactly like a driver that gives up. This switch tests that
            // without patching ymfm, and it is off unless the variable is set.
            static const bool force_fm_flags = std::getenv("U2_FM_FLAGS") != nullptr;
            const uint8_t st_out = force_fm_flags ? (uint8_t)(st | 0x03) : st;
            // A per-frame summary rather than the first N reads: what matters is whether the driver is still polling later,
            // and a cap on printed lines hides exactly that. Bits 0 and 1 are the timer overflow flags it would be waiting
            // for, so the count of reads that carried either is the answer.
            if (trace_int) {
                static uint64_t f = ~0ULL;
                static unsigned n = 0, with_flags = 0;
                if (frame != f) {
                    if (n) std::fprintf(stderr, "  FM status frame %llu: %u reads, %u with a timer flag\n",
                                        (unsigned long long)f, n, with_flags);
                    f = frame; n = 0; with_flags = 0;
                }
                ++n;
                if (st_out & 0x03) ++with_flags;
            }
            return st_out;
        }
    case 0x4E0: return sound.in_volume[sound.in_sel & 3];
    case 0x4E1: return sound.in_sel;
    case 0x4E2: return sound.volume[sound.volume_sel & 3];
    case 0x4E3: return sound.volume_sel;
    case 0x4E8: return 1;
    case 0x4E9:
        // The status register the interrupt handler reads to tell FM from PCM. Logging the reads answers whether a handler
        // ran at all after the end-of-sample flag, which is the difference between "the chip did not signal" and "the chip
        // signalled and the driver ignored it".
        if (trace_int && pcm_notes++ < 60)
            std::fprintf(stderr, "  PCM status read = %02X at frame %llu\n",
                         (int)((sound.fm_irq ? 0x01 : 0) | (sound.pcm_irq ? 0x08 : 0)), (unsigned long long)frame);
        return (uint8_t)((sound.fm_irq ? 0x01 : 0) | (sound.pcm_irq ? 0x08 : 0));
    case 0x4EA: return sound.pcm_mask;
    case 0x4EB: {
        uint8_t r = sound.pcm_flags;
        sound.pcm_flags = 0;
        sound.pcm_irq = false;
        sound_irq();
        if (trace_int && pcm_notes++ < 60)
            std::fprintf(stderr, "  PCM flags read = %02X at frame %llu\n", r, (unsigned long long)frame);
        return r;
    }
    default: return 0;
    }
}

// MAME's cdda_db_to_gain: the CD's left (0) or right (1) channel; a positive figure mutes it.
static void cd_db(Machine& m, float db) {
    int ch = m.sound.volume_sel & 3;
    if (ch > 1) return;
    cd_catch_up(m, *m.chips, m.now);
    m.sound.cd_gain[ch] = db > 0 ? 0.0f : std::pow(10.0f, db / 20.0f);
}

// Counts the FM register diagnostics below, so a session cannot flood the log. File scope: instrument state, not machine state.
static unsigned fm_reg_notes = 0;
static unsigned late_notes = 0;
static unsigned keyon_notes = 0;

void Machine::sound_out8(uint32_t port, uint8_t d) {
    SoundChips& s = *chips;
    switch (port) {
    case 0x4D8: case 0x4DA: case 0x4DC: case 0x4DE: {
        fm_catch_up(s, now);
        s.port.at = now;
        // The original's sound driver writes 0x27 (timer control) to the YM3438 at the splash. Seeing whether this port's copy
        // of that driver reaches the same register is the difference between "the driver runs" and "the driver runs and gives
        // up", and the two look identical from outside.
        if (trace_int && fm_reg_notes++ < 400) {
            if ((port - 0x4D8) >> 1 == 0) std::fprintf(stderr, "  FM reg=%02X at frame %llu\n", d, (unsigned long long)frame);
            else std::fprintf(stderr, "  FM data=%02X at frame %llu\n", d, (unsigned long long)frame);
        }
        // Instrument, per CHIP. FM Towns has two YM3438s: 0x4D8/0x4DA are the first one's address and data, 0x4DC/0x4DE the
        // second's. Tracking a single address register missed every key-on aimed at the second chip, which is why the count
        // read zero while the sequencer was clearly running.
        {
            const int off = (port - 0x4D8) >> 1;    // 0 addr chip0, 1 data chip0, 2 addr chip1, 3 data chip1
            const int chip = off >> 1;
            if ((off & 1) == 0) {
                s.fm_addr[chip] = d;
            } else {
                if (s.fm_addr[chip] == 0x28 && (d & 0xF0) && keyon_notes++ < 20)
                    std::fprintf(stderr, "  FM key-on chip%d %02X at frame %llu\n", chip, d, (unsigned long long)frame);
                if (trace_int && frame > 200 && late_notes++ < 40)
                    std::fprintf(stderr, "  LATE chip%d %02X = %02X at frame %llu\n", chip, s.fm_addr[chip], d,
                                 (unsigned long long)frame);
            }
        }
        s.fm.write((port - 0x4D8) >> 1, d);
        return;
    }
    case 0x4E0: sound.in_volume[sound.in_sel & 3] = d; return;
    case 0x4E1: sound.in_sel = d; return;
    case 0x4E2:
        sound.volume[sound.volume_sel & 3] = d;
        if ((sound.volume_sel & 4) && !(sound.volume_sel & 0x18)) cd_db(*this, (~d & 0x3F) * -0.5f);
        return;
    case 0x4E3:
        sound.volume_sel = d;
        if (!(d & 4)) cd_db(*this, 1);
        else if (d & 8) cd_db(*this, 0);
        else if (d & 0x10) cd_db(*this, -32.0f);
        return;
    case 0x4EA:
        sound.pcm_mask = d;
        // The mask says which channels' end-of-bank flags interrupt the CPU. A driver that intends to stop a one-shot
        // sample sets it; one that polls the flags register never does. Printing the first few writes answers which kind
        // of driver this is, which decides what "the sample never stopped" actually means.
        if (trace_int && pcm_notes++ < 20)
            std::fprintf(stderr, "  PCM mask = %02X at frame %llu\n", d, (unsigned long long)frame);
        return;
    case 0x4EC: return;             // LED, mute
    case 0x4F0: case 0x4F1: case 0x4F2: case 0x4F3: case 0x4F4: case 0x4F5: case 0x4F6: {
        PcmChannel& c = s.pcm[s.cbank];
        switch (port - 0x4F0) {
        case 0: c.env = d; break;
        case 1: c.pan = d; break;
        case 2: c.step = (uint16_t)((c.step & 0xFF00) | d); break;
        case 3: c.step = (uint16_t)((c.step & 0x00FF) | (d << 8)); break;
        case 4: c.loopst = (uint16_t)((c.loopst & 0xFF00) | d); break;
        case 5: c.loopst = (uint16_t)((c.loopst & 0x00FF) | (d << 8)); break;
        case 6: c.start = d; c.wraps = 0; if (!c.on) c.addr = (uint32_t)c.start << (8 + 11); break;
        }
        return;
    }
    case 0x4F7:
        if (d & 0x40) s.cbank = d & 7;
        // the wave RAM window's bank: kept in pcm_ctrl's low bits only when MOD is 0
        sound.pcm_ctrl = (uint8_t)((d & 0x80) | (d & 0x40 ? (sound.pcm_ctrl & 0x0F) : (d & 0x0F)));
        return;
    case 0x4F8:
        sound.pcm_onoff = d;
        if (trace_int && pcm_notes++ < 40)
            std::fprintf(stderr, "  PCM on/off = %02X at frame %llu\n", d, (unsigned long long)frame);
        for (int i = 0; i < 8; ++i) {
            s.pcm[i].on = !((d >> i) & 1);
            s.pcm[i].wraps = 0;
            if (!s.pcm[i].on) s.pcm[i].addr = (uint32_t)s.pcm[i].start << (8 + 11);
        }
        return;
    default: return;
    }
}

// ---- the mix ---------------------------------------------------------------------------------
void Machine::audio_take(std::vector<int16_t>& out) {
    SoundChips& s = *chips;
    // Once a second, say which sources actually produced samples. A silent mixer with busy chips is the shape of "the music
    // never plays", and this names the source instead of leaving three candidates.
    if (trace_int) {
        const uint64_t sec = now / kClock;
        static uint64_t printed = ~0ULL;
        if (sec != s.nz_second) {
            // Printed when any source produced something, and otherwise once every thirty seconds, so a silent stretch is
            // visible without filling the log and the moment sound starts is never missed.
            if ((s.nz[0] || s.nz[1] || s.nz[2] || sec % 30 == 0) && printed != sec) {
                printed = sec;
                std::fprintf(stderr, "  AUDIO %llus fm=%llu pcm=%llu cd=%llu samples\n", (unsigned long long)s.nz_second,
                             (unsigned long long)s.nz[0], (unsigned long long)s.nz[1], (unsigned long long)s.nz[2]);
            }
            s.nz_second = sec;
            s.nz[0] = s.nz[1] = s.nz[2] = 0;
        }
    }
    out.clear();
    fm_catch_up(s, now);
    cd_catch_up(*this, s, now);
    Stream* src[3] = {&s.fm_out, &s.pcm_out, &s.cd_out};
    static const float gain[3] = {1.0f, 1.0f, 0.30f};       // MAME's routes into the speakers
    for (;; ++s.k) {
        float l = 0, r = 0;
        for (int n = 0; n < 3; ++n) {
            // the source's samples held, averaged over the output frame's span [p0, p1) (in
            // 1/b of a source sample): what MAME's resampler gives, highs and images alike
            Stream& st = *src[n];
            uint64_t p0 = s.k * st.a, p1 = p0 + st.a, i0 = p0 / st.b, i1 = (p1 - 1) / st.b;
            if (i1 >= st.count()) goto done;
            if (audio_mute & (1 << n)) continue;
            float sl = 0, sr = 0;
            for (uint64_t i = i0; i <= i1; ++i) {
                uint64_t w = std::min(p1, (i + 1) * st.b) - std::max(p0, i * st.b);
                const float* v = &st.buf[(size_t)(i - st.first) * 2];
                sl += v[0] * (float)w; sr += v[1] * (float)w;
            }
            l += gain[n] * sl / (float)st.a;
            r += gain[n] * sr / (float)st.a;
        }
        out.push_back((int16_t)std::clamp((int)(l * 32768.0f), -32768, 32767));
        out.push_back((int16_t)std::clamp((int)(r * 32768.0f), -32768, 32767));
    }
done:
    for (Stream* st : src) st->drop_to(s.k * st->a / st->b);
}

// ---- the oracle's sound, replayed ------------------------------------------------------------
static uint32_t bcd_msf(uint8_t m, uint8_t s, uint8_t f) {
    auto b = [](uint8_t v) { return (uint32_t)((v >> 4) * 10 + (v & 15)); };
    uint32_t n = (b(m) * 60 + b(s)) * 75 + b(f);
    return n >= 150 ? n - 150 : 0;
}

bool replay_sound_log(Machine& m, const std::string& path, double seconds, WavWriter& wav, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) { err = "cannot read " + path; return false; }
    uint8_t param[8] = {};                      // MAME's parameter[0] the newest
    uint64_t cd_at = kNever;                    // a play waiting for its 1 ms
    uint32_t cd_from = 0, cd_to = 0;
    std::vector<int16_t> audio;
    const uint64_t end = (uint64_t)(seconds * kClock);
    char line[128];
    auto run_to = [&](uint64_t t) {
        while (m.now < t) {
            uint64_t step = std::min({t, m.now + kFrameClocks, cd_at});
            if (step <= m.now) step = t;
            m.advance(step);
            if (m.now >= cd_at) { m.cd_play(cd_from, cd_to); cd_at = kNever; }
            m.audio_take(audio);
            wav.write(audio);
        }
    };
    while (std::fgets(line, sizeof line, f)) {
        unsigned long long us;
        char kind[4] = {};
        unsigned off, data, mask;
        bool mem = std::sscanf(line, "%llu %3s %x %x %x", &us, kind, &off, &data, &mask) == 5 && kind[0] == 'm';
        if (!mem && std::sscanf(line, "%llu %x %x %x", &us, &off, &data, &mask) != 4) continue;
        uint64_t t = us * (kClock / 1000000);
        if (t > end) break;
        run_to(t);
        for (int k = 0; k < 4; ++k) {
            if (!((mask >> (8 * k)) & 0xFF)) continue;
            uint8_t d = (uint8_t)(data >> (8 * k));
            uint32_t a = off + k;
            if (mem) { m.mem_write(0xC2200000u + (a & 0xFFF), 1, d); continue; }
            if (a == 0x4C4) { std::memmove(param + 1, param, 7); param[0] = d; continue; }
            if (a == 0x4C2) {
                if ((d & 0x9F) == 0x04) {        // play, from parameters 7..5 to 4..2
                    cd_from = bcd_msf(param[7], param[6], param[5]);
                    cd_to = bcd_msf(param[4], param[3], param[2]);
                    cd_at = m.now + kClock / 1000;
                } else if ((d & 0x9E) == 0x84) {
                    m.cd_stop();
                }
                continue;
            }
            if (a < 0x4D0) continue;            // the CD controller's other registers
            m.out8(a, d);
        }
    }
    std::fclose(f);
    run_to(end);
    return true;
}

// ---- WAV -------------------------------------------------------------------------------------
static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }

bool WavWriter::open(const std::string& path) {
    close();
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
                     1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    put32(h + 24, kAudioRate);
    put32(h + 28, kAudioRate * 4);
    std::fwrite(h, 1, sizeof h, f_);
    bytes_ = 0;
    return true;
}

void WavWriter::write(const std::vector<int16_t>& lr) {
    if (!f_ || lr.empty()) return;
    std::fwrite(lr.data(), 2, lr.size(), f_);      // little-endian, as the host is
    bytes_ += (uint32_t)lr.size() * 2;
}

void WavWriter::close() {
    if (!f_) return;
    uint8_t v[4];
    put32(v, 36 + bytes_);
    std::fseek(f_, 4, SEEK_SET); std::fwrite(v, 1, 4, f_);
    put32(v, bytes_);
    std::fseek(f_, 40, SEEK_SET); std::fwrite(v, 1, 4, f_);
    std::fclose(f_);
    f_ = nullptr;
}

}  // namespace towns
