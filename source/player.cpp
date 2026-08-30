#include "player.h"
#include "audio.h"
#include "stb_image.h"
#include "http.h"
#include "aacdec.h"
#include <3ds.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

// ─── Dimensions ──────────────────────────────────────────────────────────────
// VID_W/VID_H are the MAX (output buffer is allocated for these). The actual
// coded resolution is read from the H.264 SPS at runtime — Jellyfin preserves
// aspect ratio under the MaxWidth/MaxHeight caps, so the real frame is often
// shorter than 240 (e.g. 400x224 for 16:9). MVD must be configured with the
// real coded dims or render() silently writes nothing.
static constexpr u32 VID_W = 400, VID_H = 240;
static constexpr u32 FB_W  = 240, FB_H  = 400;

// Actual coded dimensions (updated from SPS; default to the max until parsed).
static u32 g_decW = VID_W, g_decH = VID_H;

// On-screen debug overlay (bottom-screen console + green test paint). Hidden by
// default; toggled during playback by holding X + D-Pad Up. File logging to
// player_debug.txt is independent of this flag. DBG() prints only when enabled.
static bool g_dbg = false;
#define DBG(...) do { if (g_dbg) printf(__VA_ARGS__); } while (0)

// ─── Buffer sizes ─────────────────────────────────────────────────────────────
static constexpr u32 TS_SZ  = 188;
static constexpr u32 RD_SZ  = TS_SZ * 128;
static constexpr u32 PES_SZ = 512 * 1024;
static constexpr u32 NAL_SZ = 512 * 1024;

// ─── MPEG-TS helpers ──────────────────────────────────────────────────────────
static inline int  tsPid (const u8* p) { return ((p[1]&0x1F)<<8)|p[2]; }
static inline bool tsPUSI(const u8* p) { return (p[1]&0x40)!=0; }

static const u8* tsPayload(const u8* p, int* outSz) {
    u8 afc = (p[3]>>4)&3;
    if (afc == 2) { *outSz=0; return nullptr; }
    int off = 4;
    if (afc == 3) off += 1 + p[4];
    if (off >= (int)TS_SZ) { *outSz=0; return nullptr; }
    *outSz = TS_SZ - off;
    return p + off;
}

// ─── PAT parser ───────────────────────────────────────────────────────────────
static int parsePAT(const u8* pl, int sz) {
    if (sz < 9) return -1;
    int ptr = pl[0];
    const u8* t = pl + 1 + ptr;
    int rem = sz - 1 - ptr;
    if (rem < 8 || t[0] != 0x00) return -1;
    int secLen     = ((t[1]&0x0F)<<8)|t[2];
    int entryBytes = secLen - 9;
    const u8* prg  = t + 8;
    for (int i = 0; i+4 <= entryBytes && i+4 <= rem-8; i += 4) {
        int prog = (prg[i]<<8)|prg[i+1];
        int pid  = ((prg[i+2]&0x1F)<<8)|prg[i+3];
        if (prog != 0) return pid;
    }
    return -1;
}

// ─── PMT parser ───────────────────────────────────────────────────────────────
// Returns the H.264 video PID (-1 if none). If audPid is non-null, also reports
// the first AAC audio PID (0x0F = ADTS, 0x11 = LATM) via *audPid, or -1.
static int parsePMT(const u8* pl, int sz, int* audPid = nullptr) {
    if (audPid) *audPid = -1;
    if (sz < 13) return -1;
    int ptr = pl[0];
    const u8* t = pl + 1 + ptr;
    int rem = sz - 1 - ptr;
    if (rem < 12 || t[0] != 0x02) return -1;
    int secLen  = ((t[1]&0x0F)<<8)|t[2];
    int piLen   = ((t[10]&0x0F)<<8)|t[11];
    int esBytes = secLen - 13 - piLen;
    const u8* es = t + 12 + piLen;
    int remEs    = rem - 12 - piLen;
    int vid = -1;
    for (int o = 0; o+5 <= esBytes && o+5 <= remEs; ) {
        int type = es[o];
        int pid  = ((es[o+1]&0x1F)<<8)|es[o+2];
        int esil = ((es[o+3]&0x0F)<<8)|es[o+4];
        if (type == 0x1B && vid < 0) vid = pid;                       // H.264 video
        else if ((type == 0x0F || type == 0x11) && audPid && *audPid < 0)
            *audPid = pid;                                            // AAC audio
        o += 5 + esil;
    }
    return vid;
}

// ─── PES header skip ──────────────────────────────────────────────────────────
static int pesHeaderLen(const u8* pay, int sz) {
    if (sz < 9 || pay[0]!=0 || pay[1]!=0 || pay[2]!=1) return 0;
    return 9 + pay[8];
}

// Extract the 90 kHz presentation timestamp from a PES header, or -1 if absent.
static long long pesPTS(const u8* pay, int sz) {
    if (sz < 14 || pay[0]!=0 || pay[1]!=0 || pay[2]!=1) return -1;
    if (!(pay[7] & 0x80)) return -1;             // PTS_DTS_flags: no PTS present
    return ((long long)((pay[9]  >> 1) & 0x07) << 30)
         | ((long long) pay[10]               << 22)
         | ((long long)((pay[11] >> 1) & 0x7F) << 15)
         | ((long long) pay[12]               <<  7)
         | ((long long)((pay[13] >> 1) & 0x7F));
}

// ─── Bottom-screen info block (text console: 30 rows x 40 cols) ──────────────
// The block sits at the top of the screen. Metadata grows downward from
// ROW_META and is at most 6 rows (series + blank + 2 title lines + blank +
// year), so everything below it keeps a fixed row — the bar doesn't shift when
// a title wraps or a movie has no series name. Text spans cols 5..38.
static constexpr int ROW_SUB    = 1;    // bottom-screen subtitles, rows 1..7
static constexpr int ROW_STATUS = 9;    // "Buffering..." / "Seeking..."
static constexpr int ROW_META   = 11;   // series / title / year, rows 11..16
static constexpr int ROW_TIME   = 19;   // "MM:SS / MM:SS"    + "B EXIT"
static constexpr int ROW_BAR    = 21;   // "[####--------]"
static constexpr int ROW_HINTS  = 23;   // "<< -10s  D-PAD  +30s >>"

struct SubtitleCue {
    double start = 0, end = 0;
    std::string text;
};

static double parseVttTime(const std::string& s) {
    int h = 0, m = 0;
    double sec = 0;
    if (sscanf(s.c_str(), "%d:%d:%lf", &h, &m, &sec) == 3)
        return h * 3600.0 + m * 60.0 + sec;
    if (sscanf(s.c_str(), "%d:%lf", &m, &sec) == 2)
        return m * 60.0 + sec;
    return -1;
}

static std::string cleanVttText(const std::string& in) {
    std::string out;
    bool tag = false;
    for (char c : in) {
        if (c == '<') { tag = true; continue; }
        if (c == '>') { tag = false; continue; }
        if (!tag) out += c;
    }
    struct Entity { const char* from; const char* to; } entities[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&nbsp;", " "}
    };
    for (auto& e : entities) {
        size_t p = 0;
        while ((p = out.find(e.from, p)) != std::string::npos)
            out.replace(p, strlen(e.from), e.to);
    }
    return out;
}

static std::vector<SubtitleCue> parseVtt(const std::string& data) {
    std::vector<SubtitleCue> cues;
    std::vector<std::string> lines;
    size_t p = 0;
    while (p <= data.size()) {
        size_t e = data.find('\n', p);
        if (e == std::string::npos) e = data.size();
        std::string line = data.substr(p, e - p);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        if (e == data.size()) break;
        p = e + 1;
    }
    for (size_t i = 0; i < lines.size(); i++) {
        size_t arrow = lines[i].find("-->");
        if (arrow == std::string::npos) continue;
        std::string a = lines[i].substr(0, arrow);
        std::string b = lines[i].substr(arrow + 3);
        while (!a.empty() && a.back() == ' ') a.pop_back();
        while (!b.empty() && b.front() == ' ') b.erase(0, 1);
        size_t setting = b.find(' ');
        if (setting != std::string::npos) b.resize(setting);
        SubtitleCue cue;
        cue.start = parseVttTime(a);
        cue.end   = parseVttTime(b);
        for (++i; i < lines.size() && !lines[i].empty(); i++) {
            if (!cue.text.empty()) cue.text += '\n';
            cue.text += cleanVttText(lines[i]);
        }
        if (cue.start >= 0 && cue.end > cue.start && !cue.text.empty())
            cues.push_back(cue);
    }
    return cues;
}

static void drawSubtitle(const std::vector<SubtitleCue>& cues, double posSec) {
    for (int row = ROW_SUB; row < ROW_SUB + 7; row++)
        printf("\x1b[%d;1H                                        ", row);
    const SubtitleCue* active = nullptr;
    for (const auto& cue : cues) {
        if (posSec >= cue.start && posSec < cue.end) { active = &cue; break; }
        if (cue.start > posSec) break;
    }
    if (!active) return;

    std::vector<std::string> rows;
    size_t p = 0;
    while (p < active->text.size() && rows.size() < 3) {
        size_t hard = active->text.find('\n', p);
        size_t end = hard == std::string::npos ? active->text.size() : hard;
        while (p < end && rows.size() < 3) {
            size_t take = end - p;
            if (take > 38) {
                take = 38;
                size_t sp = active->text.rfind(' ', p + take);
                if (sp != std::string::npos && sp > p) take = sp - p;
            }
            rows.push_back(active->text.substr(p, take));
            p += take;
            while (p < end && active->text[p] == ' ') p++;
        }
        p = hard == std::string::npos ? active->text.size() : hard + 1;
    }
    int row = ROW_SUB + (6 - (int)rows.size()) / 2;
    for (const auto& s : rows) {
        int col = 1 + (40 - (int)s.size()) / 2;
        printf("\x1b[%d;%dH%s", row++, col, s.c_str());
    }
}

static void fmtTime(char* buf, size_t n, double sec) {
    if (sec < 0) sec = 0;
    int t = (int)(sec + 0.5);
    int h = t / 3600, m = (t % 3600) / 60, s = t % 60;
    if (h > 0) snprintf(buf, n, "%d:%02d:%02d", h, m, s);
    else       snprintf(buf, n, "%02d:%02d", m, s);
}

// Draws "MM:SS / MM:SS" and a [####----] bar under the metadata.
// Uses ANSI cursor positioning so it updates in place (no scrolling).
static void drawSeekBar(double posSec, double durSec) {
    const int barW = 28;
    char bar[barW + 1];
    int filled = 0;
    if (durSec > 0) {
        double frac = posSec / durSec;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        filled = (int)(frac * barW + 0.5);
    }
    for (int i = 0; i < barW; i++) bar[i] = (i < filled) ? '#' : '-';
    bar[barW] = '\0';

    char cur[16];
    fmtTime(cur, sizeof(cur), posSec);
    if (durSec > 0) {
        char tot[16];
        fmtTime(tot, sizeof(tot), durSec);
        printf("\x1b[%d;5H%s / %s    ", ROW_TIME, cur, tot);  // spaces clear leftovers
    } else {
        printf("\x1b[%d;5H%s    ", ROW_TIME, cur);            // unknown duration
    }
    printf("\x1b[%d;5H[%s]", ROW_BAR, bar);
}

// Button hints. Skips go directly under the seek bar (which spans cols 5..34)
// so each label sits on the side it seeks toward; exit goes opposite the
// timecode, past the space drawSeekBar rewrites.
// Static: drawn once alongside drawMeta, redrawn when the debug overlay closes.
static void drawControls() {
    printf("\x1b[%d;22HA PAUSE", ROW_TIME);
    printf("\x1b[%d;31HB EXIT",  ROW_TIME);
    printf("\x1b[%d;5H<< -10s",  ROW_HINTS);
    printf("\x1b[%d;17HD-PAD",   ROW_HINTS);
    printf("\x1b[%d;28H+30s >>", ROW_HINTS);
}

// Series name / episode title / year at the top of the screen, one blank line
// between each item. A long title wraps to a second line instead of being
// truncated. Grows downward from ROW_META; at most 6 rows.
static void drawMeta(const std::string& series, const std::string& title, int year) {
    const size_t W = 34;   // console text width (col 5 .. 38)

    // Wrap the title into up to two lines.
    std::string t1, t2;
    if (title.size() <= W) {
        t1 = title;
    } else {
        size_t len = W;
        size_t sp  = title.rfind(' ', W);
        if (sp != std::string::npos && sp > 0) len = sp;   // break on a space
        t1 = title.substr(0, len);
        size_t j = len;
        while (j < title.size() && title[j] == ' ') j++;
        t2 = title.substr(j);
        if (t2.size() > W) t2 = t2.substr(0, W - 3) + "...";
    }

    int row = ROW_META;

    if (!series.empty()) { printf("\x1b[%d;5H%.34s", row, series.c_str()); row += 2; }
    if (!title.empty()) {
        printf("\x1b[%d;5H%s", row, t1.c_str()); row++;
        if (!t2.empty()) { printf("\x1b[%d;5H%s", row, t2.c_str()); row++; }
        row++;   // blank line after the title
    }
    if (year > 0) printf("\x1b[%d;5H%d", row, year);
}

// ─── Blit 400x240 BGR565 -> 240x400 BGR8 and swap ────────────────────────────
// 3DS top-screen layout: column-major, 240 rows per column.
// Logical (x,y) -> physical offset = (x*240 + (239-y)) * 3
// C3D_Fini is called in main before playerPlay, so gspWaitForVBlank is safe.
// Only swap the top screen to avoid disturbing the console on the bottom screen.
static void blitFrame(const u8* mvdOut, u32 frameCount) {
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
    const u16* src = (const u16*)mvdOut;

    if (frameCount < 3) {
        DBG("fb=%p px[0]=%04X dim=%lux%lu\n",
            (void*)fb, (unsigned)src[0],
            (unsigned long)g_decW, (unsigned long)g_decH);
    }

    // Black out the screen first so a smaller-than-screen frame has no leftover
    // (e.g. the green test paint) in the letterbox margin.
    memset(fb, 0, FB_W * FB_H * 3);

    // Source stride is the coded width (g_decW); clamp to screen bounds.
    u32 maxY = (g_decH < FB_W) ? g_decH : FB_W;   // y maps to screen X (240 wide)
    u32 maxX = (g_decW < FB_H) ? g_decW : FB_H;   // x maps to screen Y (400 tall)

    // Centre the picture in the screen so a frame smaller than 400x240 gets equal
    // black bars on both sides instead of hugging the top-left corner. A 4:3
    // source transcodes to 320x240 and otherwise puts all 80px of bar on the right.
    u32 padX = (FB_H - maxX) / 2;                 // horizontal margin (screen 400 across)
    u32 padY = (FB_W - maxY) / 2;                 // vertical margin (screen 240 down)

    for (u32 y = 0; y < maxY; y++) {
        for (u32 x = 0; x < maxX; x++) {
            u16 px  = src[y * g_decW + x];
            u8  r5  = (px >> 11) & 0x1F;
            u8  g6  = (px >> 5)  & 0x3F;
            u8  b5  =  px        & 0x1F;
            u32 off = ((x + padX) * FB_W + (FB_W - 1 - (y + padY))) * 3;
            fb[off]     = (b5 << 3) | (b5 >> 2);
            fb[off + 1] = (g6 << 2) | (g6 >> 4);
            fb[off + 2] = (r5 << 3) | (r5 >> 2);
        }
    }

    GSPGPU_FlushDataCache(fb, FB_W * FB_H * 3);
    gspWaitForVBlank();
    gfxScreenSwapBuffers(GFX_TOP, false);
}

// Mirrors to the on-screen console (only when debug is enabled) and always to file.
#define DLOG(dbg, ...) do { if (g_dbg) printf(__VA_ARGS__); if(dbg){fprintf(dbg,__VA_ARGS__);fflush(dbg);} } while(0)

// ─── Decoded-frame FIFO + display scheduler ──────────────────────────────────
// Decode and display are decoupled: MVD writes each frame into a slot of this
// FIFO and the demux keeps running ahead (bounded by the slot count), while
// displayPump() blits each frame only when its presentation time arrives.
//
// The previous design paced inside the decoder, so display, demux and the audio
// feed all stalled together on every pacing sleep. That made A/V sync impossible
// to close: holding video to let audio catch up also halted the demux that
// delivers said audio, so the ndsp queue starved, underrunning in a ~0.5s
// snap/underrun/snap stutter cycle (confirmed on hardware via the sync log).
// With the FIFO, the demux runs up to ~0.7s ahead of the screen, which keeps the
// ndsp queue deep and absorbs the muxer's A/V interleave skew (audio PES for a
// given moment arrive later in the stream than the video PES for that moment).
//
// Frame timing comes from a smooth wall-clock anchor, *disciplined* against
// audio::audioClock() — the program-time PTS of the sample the DSP is playing
// right now, on the same 90 kHz axis as video PTS: small error → anchor slew of
// at most ±2 ms/frame (drift-proof, immune to DSP clock jitter); large error
// (resume start, stall recovery) → one anchor snap, then locked. Frame timing is
// never taken directly from the DSP clock — that was tried and stuttered. All
// anchor math is signed (s64): audio behind video makes intermediate values
// negative, and u64 arithmetic underflowed there.
static constexpr int FIFO_MAX = 16;
struct VidFrame { u8* buf; long long pts; };
static VidFrame  g_fifo[FIFO_MAX];
static int       g_fifoN     = 0;      // slots successfully allocated
static int       g_fifoHead  = 0;      // next frame to display
static int       g_fifoLen   = 0;      // decoded frames waiting
static u32       g_dispCount = 0;      // frames blitted so far
static long long g_lastBlitPts = -1;   // PTS on screen (stale-audio reference)
static long long g_vidFirstPts = -1;   // first video PTS demuxed (stale-audio ref at start)

static long long g_pacePts0  = -1;     // 90 kHz PTS of the anchor frame
static s64       g_paceWall0 = 0;      // osGetTime() (ms) scheduled for the anchor PTS
static FILE*     g_paceLog   = nullptr;   // sync-state log (player_debug.txt), for tuning
static u32       g_paceFrames = 0;

static void freeFifoSlots() {
    for (int i = 0; i < g_fifoN; i++) { linearFree(g_fifo[i].buf); g_fifo[i].buf = nullptr; }
    g_fifoN = 0;
}

static void blitHead(FILE* dbg) {
    VidFrame& f = g_fifo[g_fifoHead];
    blitFrame(f.buf, g_dispCount);
    g_dispCount++;
    if (f.pts >= 0) g_lastBlitPts = f.pts;
    g_fifoHead = (g_fifoHead + 1) % g_fifoN;
    g_fifoLen--;
    if (g_dispCount <= 5) DLOG(dbg, "frame %u\n", (unsigned)g_dispCount);
}

// Blit every frame whose presentation time has arrived. waitFree additionally
// blocks (napping in 20ms slices) until a FIFO slot is free for the decoder;
// drain blocks until the FIFO is empty (end of stream). The naps hand the core
// to the download thread, so waiting here never starves the network side.
static void displayPump(bool waitFree, bool drain, bool* stop, FILE* dbg) {
    while (g_fifoLen > 0 && !*stop) {
        long long pts = g_fifo[g_fifoHead].pts;
        if (pts < 0) { blitHead(dbg); continue; }      // unstamped AU: show immediately
        s64 now = (s64)osGetTime();
        if (g_pacePts0 < 0) {                          // first frame anchors the clock
            g_pacePts0 = pts; g_paceWall0 = now;
            blitHead(dbg);
            continue;
        }
        long long dpts = pts - g_pacePts0;
        if (dpts < 0) dpts += (1LL << 33);             // 33-bit PTS wraparound
        s64 target = g_paceWall0 + dpts / 90;          // 90 kHz ticks → ms
        if (now < target) {                            // head frame not due yet
            if (!drain && !(waitFree && g_fifoLen >= g_fifoN)) return;
            s64 nap = target - now;
            if (nap > 20) nap = 20;
            svcSleepThread(nap * 1000000LL);
            hidScanInput();
            if (hidKeysDown() & KEY_B) { *stop = true; return; }
            continue;
        }

        // Frame is due. Audio-clock servo: err > 0 = video presenting early
        // (audio behind), err < 0 = video late.
        double ac = audio::audioClock();
        s64 errMs = 0;
        if (ac >= 0) {
            errMs = (s64)((pts / 90000.0 - ac) * 1000.0);
            if (errMs > 300 || errMs < -300) {
                if (errMs > 1500) errMs = 1500;        // bound the hold on wild PTS gaps
                g_paceWall0 = now + errMs - dpts / 90;
                if (g_paceLog) { fprintf(g_paceLog, "sync snap err=%lldms\n", (long long)errMs); fflush(g_paceLog); }
                if (errMs > 0) continue;               // target moved ahead → wait again
            } else {
                s64 slew = errMs / 8;                  // heavy damping: absorbs clock jitter
                if (slew >  2) slew =  2;              // imperceptible per frame
                if (slew < -2) slew = -2;
                g_paceWall0 += slew;
            }
        } else if (now - target > 200) {
            // No audio clock (no track, missing dsp_firm, underrun) and we fell
            // well behind: re-anchor instead of racing through the backlog.
            g_pacePts0 = pts; g_paceWall0 = now;
            DBG("pace resync\n");
        }
        if (g_paceLog && (++g_paceFrames % 240) == 0) {
            if (ac >= 0)
                fprintf(g_paceLog, "sync err=%+lldms q=%d drops=%u fifo=%d\n",
                        (long long)errMs, audio::queuedBufs(), audio::droppedBlocks(), g_fifoLen);
            else
                fprintf(g_paceLog, "sync noclock q=%d drops=%u fifo=%d\n",
                        audio::queuedBufs(), audio::droppedBlocks(), g_fifoLen);
            fflush(g_paceLog);
        }
        blitHead(dbg);
    }
}

// ─── Read-ahead download ring buffer ──────────────────────────────────────────
// A background thread continuously pulls the HTTP TS stream into this ring while
// the main thread demuxes/decodes/paces out of it. This decouples network I/O
// from display timing: when paceToPts() sleeps to hold a frame to its real
// presentation time, the download thread keeps the buffer full instead of the
// socket starving — which is what previously caused the stutter (a single thread
// can't both pace display AND keep reading). RING_SZ is a power of two so the
// free-running u32 head/tail counters wrap cleanly; index = pos & RING_MASK.
static constexpr u32 RING_SZ   = 8u * 1024 * 1024;   // ~1.8 min at 0.6 Mbps
static constexpr u32 RING_MASK = RING_SZ - 1;
static constexpr u32 PREBUF_SZ = 512u * 1024;        // fill this much before playing

struct DlRing {
    u8*           data;
    std::string   playlistUrl;
    FILE*         dbg;
    volatile u32  head;          // producer: total bytes written
    volatile u32  tail;          // consumer: total bytes consumed
    volatile bool producerDone;  // stream ended/errored — no more data coming
    volatile bool consumerStop;  // consumer asked the producer to stop
    LightLock     lock;          // guards the head/tail snapshot
};
static DlRing g_ring;

static inline u32 ringUsed() {
    LightLock_Lock(&g_ring.lock);
    u32 u = g_ring.head - g_ring.tail;   // wrap-safe: both are free-running u32
    LightLock_Unlock(&g_ring.lock);
    return u;
}

static std::string hlsResolve(const std::string& base, const std::string& ref) {
    if (ref.compare(0, 7, "http://") == 0 || ref.compare(0, 8, "https://") == 0)
        return ref;
    size_t scheme = base.find("://");
    if (scheme == std::string::npos) return ref;
    size_t authorityEnd = base.find('/', scheme + 3);
    std::string origin = authorityEnd == std::string::npos
                       ? base : base.substr(0, authorityEnd);
    if (!ref.empty() && ref[0] == '/') return origin + ref;
    size_t slash = base.rfind('/');
    return (slash == std::string::npos ? origin + "/" : base.substr(0, slash + 1)) + ref;
}

static void flushBottomConsole() {
    // Use libctru's framebuffer-aware flush rather than guessing the allocation
    // size/stride. This covers both emulator and hardware framebuffer layouts.
    gfxFlushBuffers();
}

static void hudRect(u8* fb, int x, int y, int w, int h,
                    u8 r, u8 g, u8 b) {
    if (!fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 320) w = 320 - x;
    if (y + h > 240) h = 240 - y;
    for (int py = y; py < y + h; py++)
        for (int px = x; px < x + w; px++) {
            // Azahar's bottom-screen presentation path is most reliable in the
            // native console RGB565 layout. Store the rotated framebuffer pixel
            // directly instead of relying on its 24-bit BGR conversion path.
            u32 off = px * 240 + (239 - py);
            ((u16*)fb)[off] = (u16)(((r >> 3) << 11) |
                                    ((g >> 2) << 5) | (b >> 3));
        }
}

static void drawPlaybackHud(u8* fb, double posSec, double durSec, bool paused) {
    if (!fb) return;

    // High-contrast control deck. Keep it tall so emulator scaling cannot wash
    // the controls into the background at small window sizes.
    hudRect(fb, 0, 118, 320, 122, 18, 42, 68);
    hudRect(fb, 12, 130, 296, 14, 225, 232, 238);
    double frac = durSec > 0 ? posSec / durSec : 0;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    hudRect(fb, 12, 130, (int)(296 * frac), 14, 0, 235, 205);

    // Rewind / forward chevrons.
    for (int i = 0; i < 12; i++) {
        hudRect(fb, 39 + i, 172 - i, 4, 2 * i + 4, 255, 255, 255);
        hudRect(fb, 67 + i, 172 - i, 4, 2 * i + 4, 255, 255, 255);
        hudRect(fb, 277 - i, 172 - i, 4, 2 * i + 4, 255, 255, 255);
        hudRect(fb, 249 - i, 172 - i, 4, 2 * i + 4, 255, 255, 255);
    }

    // Centre play/pause button.
    hudRect(fb, 126, 154, 68, 68, 0, 190, 175);
    if (paused) {
        for (int i = 0; i < 14; i++)
            hudRect(fb, 145 + i, 174 - i / 2, 3, i + 2, 255, 255, 255);
    } else {
        hudRect(fb, 145, 171, 10, 34, 255, 255, 255);
        hudRect(fb, 165, 171, 10, 34, 255, 255, 255);
    }

    // Red B/exit indicator at the far right.
    hudRect(fb, 286, 198, 30, 34, 210, 35, 55);
    hudRect(fb, 293, 205, 15, 5, 255, 255, 255);
    hudRect(fb, 293, 218, 15, 5, 255, 255, 255);
    hudRect(fb, 293, 205, 5, 18, 255, 255, 255);
    hudRect(fb, 304, 209, 5, 10, 255, 255, 255);

    GSPGPU_FlushDataCache(fb, 320 * 240 * 2);
}

static void presentPlaybackBottom(double posSec, double durSec, bool paused) {
    u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr);
    // Clear and redraw the entire back buffer ourselves. PrintConsole caches a
    // framebuffer pointer and was clearing the just-presented HUD after swaps.
    hudRect(fb, 0, 0, 320, 240, 6, 12, 20);
    // Use the same pointer for the background and every control. Some emulator
    // backends advance their writable buffer when queried more than once.
    drawPlaybackHud(fb, posSec, durSec, paused);
    flushBottomConsole();
    // The HUD is a persistent single-buffer surface. Azahar can present the
    // clear from one alternating buffer while dropping the controls written to
    // the other, so never swap the bottom screen during playback.
    gspWaitForVBlank();
}

static int hlsSegmentNumber(const std::string& uri) {
    size_t end = uri.find(".ts");
    if (end == std::string::npos) return -1;
    size_t slash = uri.rfind('/', end);
    size_t begin = slash == std::string::npos ? 0 : slash + 1;
    if (begin >= end) return -1;
    int value = 0;
    for (size_t i = begin; i < end; i++) {
        if (uri[i] < '0' || uri[i] > '9') return -1;
        value = value * 10 + (uri[i] - '0');
    }
    return value;
}

static bool ringPut(DlRing* r, const u8* src, u32 size) {
    u32 pos = 0;
    while (pos < size && !r->consumerStop) {
        u32 freeb = RING_SZ - ringUsed();
        if (freeb == 0) { svcSleepThread(2000000LL); continue; }
        u32 hi = r->head & RING_MASK;
        u32 n = size - pos;
        if (n > freeb) n = freeb;
        u32 contig = RING_SZ - hi;
        if (n > contig) n = contig;
        memcpy(r->data + hi, src + pos, n);
        LightLock_Lock(&r->lock);
        r->head += n;
        LightLock_Unlock(&r->lock);
        pos += n;
    }
    return pos == size;
}

// Azahar buffers each HTTP response before exposing it to the emulated http:C
// service. Fetch finite HLS TS segments and concatenate them into the ring.
static void dlThread(void* arg) {
    DlRing* r = (DlRing*)arg;
    HttpClient http;
    int lastSegment = -1;
    unsigned failures = 0;
    while (!r->consumerStop) {
        HttpResponse playlist = http.get(r->playlistUrl);
        if (!playlist.ok()) {
            if (r->dbg) {
                fprintf(r->dbg, "HLS playlist fail status=%d result=%08lX stage=%s\n",
                        playlist.status, (unsigned long)playlist.result,
                        httpFailureStageName(playlist.failureStage));
                fflush(r->dbg);
            }
            if (++failures >= 6) break;
            svcSleepThread(2000000000LL);
            continue;
        }
        failures = 0;

        bool endList = playlist.body.find("#EXT-X-ENDLIST") != std::string::npos;
        bool foundNew = false;
        bool switchedPlaylist = false;
        size_t pos = 0;
        while (pos < playlist.body.size() && !r->consumerStop) {
            size_t eol = playlist.body.find('\n', pos);
            if (eol == std::string::npos) eol = playlist.body.size();
            std::string line = playlist.body.substr(pos, eol - pos);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            pos = eol + 1;
            if (line.empty() || line[0] == '#') continue;
            if (line.find(".m3u8") != std::string::npos) {
                r->playlistUrl = hlsResolve(r->playlistUrl, line);
                switchedPlaylist = true;
                break;
            }

            int number = hlsSegmentNumber(line);
            if (number < 0 || number <= lastSegment) continue;
            HttpResponse segment = http.get(hlsResolve(r->playlistUrl, line));
            if (!segment.ok() || segment.body.empty()) {
                if (r->dbg) {
                    fprintf(r->dbg, "HLS segment %d fail status=%d result=%08lX\n",
                            number, segment.status, (unsigned long)segment.result);
                    fflush(r->dbg);
                }
                failures++;
                break;
            }
            if (r->dbg) {
                fprintf(r->dbg, "HLS segment %d bytes=%lu\n", number,
                        (unsigned long)segment.body.size());
                fflush(r->dbg);
            }
            if (!ringPut(r, reinterpret_cast<const u8*>(segment.body.data()),
                         (u32)segment.body.size())) break;
            lastSegment = number;
            foundNew = true;
            failures = 0;
        }
        if (switchedPlaylist) continue;
        if (failures >= 6) break;
        if (endList && !foundNew) break;
        if (!foundNew) svcSleepThread(500000000LL);
    }
    r->producerDone = true;
}

// Consumer side: copy exactly n bytes out of the ring (handling wrap). The caller
// must have already confirmed ringUsed() >= n.
static void ringTake(u8* out, u32 n) {
    u32 ti     = g_ring.tail & RING_MASK;
    u32 contig = RING_SZ - ti;
    if (contig >= n) {
        memcpy(out, g_ring.data + ti, n);
    } else {
        memcpy(out, g_ring.data + ti, contig);
        memcpy(out + contig, g_ring.data, n - contig);
    }
    LightLock_Lock(&g_ring.lock);
    g_ring.tail += n;
    LightLock_Unlock(&g_ring.lock);
}

// Sentinel marker positions: 4 corners of the BGR565 output buffer, using the
// CURRENT coded dimensions (g_decW/g_decH) so they match where MVD writes.
// MVD overwrites these when it actually decodes a frame; if they survive a
// process/render call, no frame was produced. (0x11 per reference impl.)
static inline void mvdSetSentinels(u8* buf) {
    u32 sz = g_decW * g_decH * 2;
    buf[0]              = 0x11;
    buf[g_decW*2 - 1]   = 0x11;
    buf[sz - g_decW*2]  = 0x11;
    buf[sz - 1]         = 0x11;
}
static inline bool mvdSentinelsChanged(const u8* buf) {
    u32 sz = g_decW * g_decH * 2;
    return buf[0]              != 0x11 ||
           buf[g_decW*2 - 1]   != 0x11 ||
           buf[sz - g_decW*2]  != 0x11 ||
           buf[sz - 1]         != 0x11;
}

// ─── Minimal H.264 SPS parser (coded resolution only) ────────────────────────
struct BitReader { const u8* d; u32 nbits; u32 pos; };
static u32 brU1(BitReader* b) {
    if (b->pos >= b->nbits) return 0;
    u32 v = (b->d[b->pos >> 3] >> (7 - (b->pos & 7))) & 1; b->pos++; return v;
}
static u32 brUn(BitReader* b, int n) { u32 v = 0; while (n-- > 0) v = (v << 1) | brU1(b); return v; }
static u32 brUE(BitReader* b) {
    int z = 0; while (b->pos < b->nbits && brU1(b) == 0 && z < 31) z++;
    return ((1u << z) - 1) + brUn(b, z);
}
static int brSE(BitReader* b) { u32 k = brUE(b); return (k & 1) ? (int)((k + 1) >> 1) : -(int)(k >> 1); }

// Parse coded width/height from an SPS NAL (payload starts at the NAL header byte).
static bool parseSPS(const u8* nal, u32 len, u32* outW, u32* outH) {
    u8 rbsp[96]; u32 r = 0;
    for (u32 i = 1; i < len && r < sizeof(rbsp); i++) {     // skip NAL header byte
        if (i >= 2 && nal[i] == 3 && nal[i-1] == 0 && nal[i-2] == 0) continue; // emu prevention
        rbsp[r++] = nal[i];
    }
    BitReader b = { rbsp, r * 8, 0 };
    u32 profile = brUn(&b, 8); brUn(&b, 8); brUn(&b, 8);    // profile / constraints / level
    brUE(&b);                                                // seq_parameter_set_id
    if (profile==100||profile==110||profile==122||profile==244||profile==44||
        profile==83||profile==86||profile==118||profile==128||profile==138||
        profile==139||profile==134||profile==135) {
        u32 chroma = brUE(&b);
        if (chroma == 3) brU1(&b);
        brUE(&b); brUE(&b); brU1(&b);                        // bit depths + qpprime
        if (brU1(&b)) {                                      // scaling matrix present
            int cnt = (chroma != 3) ? 8 : 12;
            for (int i = 0; i < cnt; i++)
                if (brU1(&b)) {                              // list present → skip it
                    int sz = (i < 6) ? 16 : 64, last = 8, next = 8;
                    for (int j = 0; j < sz; j++) {
                        if (next != 0) { int d = brSE(&b); next = (last + d + 256) & 255; }
                        last = (next == 0) ? last : next;
                    }
                }
        }
    }
    brUE(&b);                                                // log2_max_frame_num
    u32 poc = brUE(&b);
    if (poc == 0) brUE(&b);
    else if (poc == 1) {
        brU1(&b); brSE(&b); brSE(&b);
        u32 n = brUE(&b); for (u32 i = 0; i < n; i++) brSE(&b);
    }
    brUE(&b); brU1(&b);                                      // max_num_ref_frames + gaps flag
    u32 wMbs = brUE(&b), hMaps = brUE(&b);
    u32 frameMbsOnly = brU1(&b);
    u32 w = (wMbs + 1) * 16;
    u32 h = (hMaps + 1) * 16 * (2 - frameMbsOnly);
    if (w == 0 || h == 0 || w > 1024 || h > 1024) return false;
    *outW = w; *outH = h; return true;
}

// ─── Access-unit H.264 decoder ───────────────────────────────────────────────
// Faithful adaptation of Core-2-Extreme/Video_player_for_3DS Util_decoder_mvd_decode,
// for an Annex-B MPEG-TS source instead of FFmpeg/AVCC:
//   1. Rebuild the access unit dropping AUD(9)/filler(12) NALs that MVD chokes on.
//   2. Re-arm the output buffer with MVDSTD_SetConfig() *before every frame*.
//   3. Feed the whole access unit in one mvdstdProcessVideoFrame() call.
//      The first AU is fed twice: pass 1 registers SPS/PPS, pass 2 decodes the IDR.
//   4. The frame is usually written during process(); check sentinels first and
//      only fall back to polling mvdstdRenderVideoFrame() if nothing appeared.
static void processH264(u8* pes, u32 pesLen,
                        u8* feedBuf,
                        MVDSTD_Config* cfg, bool* first,
                        bool* stop, u32* frameCount, FILE* dbg, long long auPts) {
    (void)first;
    static u32 auCount = 0;
    auCount++;
    bool log = (auCount <= 25 || *frameCount < 5);
    u32 bufSz = g_decW * g_decH * 2;

    // Feed ONE NAL per mvdstdProcessVideoFrame call (the reference never lumps
    // SPS/PPS/slice together): SPS(7)/PPS(8)→PARAMSET, VCL slice(1..5)→FRAMEREADY→render.
    u32 pos = 0;
    while (pos + 3 < pesLen && !*stop) {
        bool sc3 = pes[pos]==0 && pes[pos+1]==0 && pes[pos+2]==1;
        bool sc4 = !sc3 && pes[pos]==0 && pes[pos+1]==0 &&
                   pes[pos+2]==0 && pes[pos+3]==1;
        if (!sc3 && !sc4) { pos++; continue; }

        u32 scLen   = sc4 ? 4 : 3;
        u32 payload = pos + scLen;           // first byte after start code
        u32 nalEnd  = pesLen;
        for (u32 s = payload + 1; s + 2 < pesLen; s++) {
            if (pes[s]==0 && pes[s+1]==0 &&
                (pes[s+2]==1 || (s+3<pesLen && pes[s+2]==0 && pes[s+3]==1))) {
                nalEnd = s; break;
            }
        }
        u8  nalType  = pes[payload] & 0x1F;
        u32 nalBytes = nalEnd - payload;     // NAL payload (no start code)
        pos = nalEnd;

        if (nalBytes == 0 || nalType == 9 || nalType == 12) continue;  // skip AUD/filler
        if (3 + nalBytes > NAL_SZ) continue;

        // SPS: parse the real coded resolution and reconfigure MVD if it changed.
        // MVD render() writes nothing unless the config dims match the decoded frame.
        if (nalType == 7) {
            u32 w, h;
            if (parseSPS(pes + payload, nalBytes, &w, &h) &&
                (w != g_decW || h != g_decH)) {
                g_decW = w; g_decH = h;
                bufSz  = g_decW * g_decH * 2;
                // Output physaddr is re-pointed at a FIFO slot before every VCL.
                mvdstdGenerateDefaultConfig(cfg, g_decW, g_decH, g_decW, g_decH,
                                            nullptr, nullptr, nullptr);
                DLOG(dbg, "SPS dims=%lux%lu (reconfigured MVD)\n",
                     (unsigned long)g_decW, (unsigned long)g_decH);
            }
        }

        // Build a single Annex-B NAL (00 00 01 + payload) in linear feedBuf
        feedBuf[0]=0; feedBuf[1]=0; feedBuf[2]=1;
        memcpy(feedBuf + 3, pes + payload, nalBytes);
        u32 feedLen = 3 + nalBytes;
        GSPGPU_FlushDataCache(feedBuf, feedLen);

        bool isVCL = (nalType >= 1 && nalType <= 5);

        u8* slot = nullptr;
        if (isVCL) {                         // arm a FIFO slot just before a frame NAL
            // If the FIFO is full, display due frames until a slot frees up —
            // this is where playback speed is regulated now (backpressure).
            if (g_fifoLen >= g_fifoN) displayPump(true, false, stop, dbg);
            if (*stop) return;
            slot = g_fifo[(g_fifoHead + g_fifoLen) % g_fifoN].buf;
            cfg->physaddr_outdata0 = osConvertVirtToPhys(slot);
            cfg->physaddr_outdata1 = osConvertVirtToPhys(slot);
            mvdSetSentinels(slot);
            GSPGPU_FlushDataCache(slot, bufSz);
            MVDSTD_SetConfig(cfg);
        }

        Result rp = mvdstdProcessVideoFrame(feedBuf, feedLen, 0, nullptr);
        if (rp == (Result)MVD_STATUS_INCOMPLETEPROCESSING)   // retry once
            rp = mvdstdProcessVideoFrame(feedBuf, feedLen, 0, nullptr);

        if (log) DLOG(dbg, "NAL t=%u sz=%u proc=%08lX\n",
                     (unsigned)nalType, (unsigned)nalBytes, (unsigned long)rp);

        if (!isVCL) continue;                // parameter sets / SEI: no frame to render

        // Render the decoded frame into the slot (NULL = no re-SetConfig, patched mvd.c)
        bool got = false;
        Result rr = (Result)MVD_STATUS_BUSY;
        for (int i = 0; i < 256; i++) {
            rr = mvdstdRenderVideoFrame(nullptr, false);
            GSPGPU_InvalidateDataCache(slot, bufSz);
            if (mvdSentinelsChanged(slot)) { got = true; break; }
            if (rr != (Result)MVD_STATUS_BUSY) break;
        }
        if (log) DLOG(dbg, " rend=%08lX got=%d px=%02X%02X\n",
                     (unsigned long)rr, (int)got, slot[0], slot[1]);

        if (got) {
            g_fifo[(g_fifoHead + g_fifoLen) % g_fifoN].pts = auPts;
            g_fifoLen++;
            (*frameCount)++;
            if (*frameCount <= 5) DLOG(dbg, "dec %u\n", (unsigned)*frameCount);
            displayPump(false, false, stop, dbg);   // show whatever is due
        }
        hidScanInput();
        if (hidKeysDown() & KEY_B) { *stop = true; return; }
    }
}

// ─── AAC audio decode (Helix) ─────────────────────────────────────────────────
// The audio elementary stream is AAC-LC in ADTS framing (Jellyfin's AudioCodec=aac
// inside an MPEG-TS). Each completed audio PES holds one or more ADTS frames; we
// locate each syncword, decode it to interleaved PCM16 with Helix, and hand the
// PCM to ndsp tagged with its program-time PTS. The PES header carries the PTS of
// the first ADTS frame in the PES; g_audNextPts walks it forward by each frame's
// duration so every pushed block is tagged, which is what lets audio::audioClock()
// report true playback position for the video pacer's servo.
static HAACDecoder g_aac = nullptr;
static double      g_audNextPts = -1.0;   // PTS (sec) of the next ADTS frame decoded
static bool        g_audioOnly = false;
// One ADTS frame decodes to at most 2048 samples/channel (1024 LC, doubled by SBR)
// × 2 channels = 4096 interleaved shorts.
static short       g_pcm[2048 * 2];

static void processAAC(unsigned char* buf, int len, FILE* dbg) {
    if (!g_aac || len <= 0) return;
    static u32 frames = 0;
    unsigned char* p = buf;
    int bytesLeft = len;
    while (bytesLeft > 0) {
        int off = AACFindSyncWord(p, bytesLeft);
        if (off < 0) break;                          // no (more) ADTS frames here
        p += off; bytesLeft -= off;

        int err = AACDecode(g_aac, &p, &bytesLeft, g_pcm);
        if (err == ERR_AAC_INDATA_UNDERFLOW) break;  // frame split across PES → drop tail
        if (err) {                                   // corrupt frame → skip a byte, resync
            if (bytesLeft > 0) { p++; bytesLeft--; }
            continue;
        }

        AACFrameInfo fi;
        AACGetLastFrameInfo(g_aac, &fi);
        if (fi.outputSamps <= 0 || fi.nChans <= 0) continue;
        if (!audio::ready()) {
            audio::configure(fi.sampRateOut, fi.nChans);
            if (dbg) { fprintf(dbg, "AAC cfg: %d Hz x%d ch\n", fi.sampRateOut, fi.nChans); fflush(dbg); }
        }
        int spc = fi.outputSamps / fi.nChans;
        // Resumed transcodes prime the mux with audio from before the video's
        // start point (seen ~2.8s of it on hardware); queueing that puts audio
        // seconds behind for the whole session. Skip blocks that predate what is
        // (or will first be) on screen. Reference is the displayed frame once one
        // exists, else the first demuxed video PTS.
        long long vref = (g_lastBlitPts >= 0) ? g_lastBlitPts : g_vidFirstPts;
        bool stale = (g_audNextPts >= 0.0 && vref >= 0 &&
                      g_audNextPts < vref / 90000.0 - 0.5);
        if (!stale)
            audio::push(g_pcm, spc, fi.nChans, g_audNextPts);
        if (g_audNextPts >= 0.0 && fi.sampRateOut > 0)
            g_audNextPts += (double)spc / (double)fi.sampRateOut;
        if (++frames <= 3 && dbg) { fprintf(dbg, "AAC frame %u samps=%d\n", (unsigned)frames, fi.outputSamps); fflush(dbg); }
    }
}

static void blitArtwork(const std::string& data) {
    if (data.empty()) return;
    int aw = 0, ah = 0, comp = 0;
    unsigned char* rgba = stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(data.data()), (int)data.size(),
        &aw, &ah, &comp, 4);
    if (!rgba || aw <= 0 || ah <= 0) {
        if (rgba) stbi_image_free(rgba);
        return;
    }

    const int side = 224;
    const int left = (400 - side) / 2;
    const int top  = (240 - side) / 2;
    for (int pass = 0; pass < 2; pass++) {
        u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
        memset(fb, 0, FB_W * FB_H * 3);
        for (int y = 0; y < side; y++) {
            int sy = y * ah / side;
            for (int x = 0; x < side; x++) {
                int sx = x * aw / side;
                const u8* p = rgba + (sy * aw + sx) * 4;
                int dx = left + x, dy = top + y;
                u32 off = (dx * FB_W + (FB_W - 1 - dy)) * 3;
                fb[off] = p[2]; fb[off + 1] = p[1]; fb[off + 2] = p[0];
            }
        }
        GSPGPU_FlushDataCache(fb, FB_W * FB_H * 3);
        gspWaitForVBlank();
        gfxScreenSwapBuffers(GFX_TOP, false);
    }
    stbi_image_free(rgba);
}

// ─── Player entry point ───────────────────────────────────────────────────────
bool playerPlay(const std::string& url, long long runTimeTicks,
                const std::string& series, const std::string& title, int year,
                double startSec, double* seekOut,
                const std::string& subtitleVtt, bool* finishedOut,
                const std::string& artworkData, bool audioOnly) {
    if (seekOut) *seekOut = -1.0;
    if (finishedOut) *finishedOut = false;
    g_audioOnly = audioOnly;
    // C2D_CreateScreenTarget replaced gfx's framebuffer pointers with its own VRAM
    // allocation. After C3D_Fini that VRAM is freed but the pointers stay stale.
    // gfxSetScreenFormat is a no-op when the format hasn't changed, so it doesn't
    // fix the pointers. Full gfxExit+gfxInitDefault gives us fresh linear-memory
    // framebuffers that gfxGetFramebuffer and gfxSwapBuffers can safely use.
    gfxExit();
    gfxInitDefault();

    // Clear the top screen before playback (both buffers). When debug is on it
    // paints solid green as a framebuffer-path test; otherwise plain black.
    {
        u8 gch = g_dbg ? 255 : 0;  // green channel
        for (int pass = 0; pass < 2; pass++) {
            u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
            for (u32 i = 0; i < FB_W * FB_H * 3; i += 3) {
                fb[i]   = 0; fb[i+1] = gch; fb[i+2] = 0;
            }
            GSPGPU_FlushDataCache(fb, FB_W * FB_H * 3);
            gspWaitForVBlank();
            gfxScreenSwapBuffers(GFX_TOP, false);
        }
    }

    // The HUD is fully redrawn into both alternating buffers below. Explicit
    // double buffering matches what Azahar presents after each VBlank.
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    // Do not call consoleInit here: the HUD owns and redraws both bottom buffers.
    if (audioOnly) blitArtwork(artworkData);
    DBG("playerPlay\n");

    FILE* dbg = fopen("/3ds/3dsfin/player_debug.txt", "w");
    if (dbg) {
        u16 bottomW = 0, bottomH = 0;
        gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &bottomW, &bottomH);
        fprintf(dbg, "BUILD=single-buffer-hud-5 vttBytes=%lu bottomFormat=%d dims=%ux%u\n",
                (unsigned long)subtitleVtt.size(),
                (int)gfxGetScreenFormat(GFX_BOTTOM), bottomW, bottomH);
        size_t query = url.find('?');
        fprintf(dbg, "URL: %.*s%s\n\n",
                (int)(query == std::string::npos ? url.size() : query),
                url.c_str(), query == std::string::npos ? "" : "?<redacted>");
        fflush(dbg);
    }

    // Linear memory buffers (g_ring.data is the background download ring).
    // Frame FIFO slots are allocated best-effort — each holds one full-size
    // BGR565 frame; fewer slots just means less decode-ahead.
    g_ring.data = (u8*)linearAlloc(RING_SZ);
    u8* pesBuf  = (u8*)linearAlloc(PES_SZ);
    u8* nalBuf  = (u8*)linearAlloc(NAL_SZ);
    u8* audBuf  = (u8*)linearAlloc(PES_SZ);   // audio PES accumulator (ADTS frames)
    g_fifoN = 0;
    for (int i = 0; i < FIFO_MAX; i++) {
        g_fifo[i].buf = (u8*)linearAlloc(VID_W * VID_H * 2);
        if (!g_fifo[i].buf) break;
        g_fifo[i].pts = -1;
        g_fifoN++;
    }

    if (!g_ring.data || !pesBuf || !nalBuf || !audBuf || g_fifoN < 4) {
        printf("alloc failed\n");
        if (dbg) { fprintf(dbg, "alloc failed (fifo=%d)\n", g_fifoN); fclose(dbg); }
        linearFree(g_ring.data); linearFree(pesBuf);
        linearFree(nalBuf); linearFree(audBuf);
        freeFifoSlots();
        svcSleepThread(3000000000LL);
        return false;
    }
    DBG("Buffers OK (fifo=%d)\n", g_fifoN);

    // Reset coded dims to the max so the first SPS always triggers reconfigure.
    g_decW = VID_W; g_decH = VID_H;

    // Reset the frame-pacing anchor (set on the first displayed frame below),
    // the audio PTS walker (set from the first audio PES header), and FIFO state.
    g_pacePts0 = -1; g_paceWall0 = 0;
    g_audNextPts = -1.0;
    g_paceLog = dbg; g_paceFrames = 0;
    g_fifoHead = 0; g_fifoLen = 0; g_dispCount = 0;
    g_lastBlitPts = -1; g_vidFirstPts = -1;

    // Init MVD hardware decoder
    Result mvdRet = mvdstdInit(MVDMODE_VIDEOPROCESSING,
                               MVD_INPUT_H264, MVD_OUTPUT_BGR565,
                               MVD_DEFAULT_WORKBUF_SIZE, nullptr);
    if (R_FAILED(mvdRet)) {
        printf("mvdstdInit fail: 0x%08X\n", (unsigned)mvdRet);
        if (dbg) { fprintf(dbg, "mvdstdInit fail: 0x%08X\n", (unsigned)mvdRet); fclose(dbg); }
        linearFree(g_ring.data); linearFree(pesBuf);
        linearFree(nalBuf); linearFree(audBuf);
        freeFifoSlots();
        svcSleepThread(3000000000LL);
        return false;
    }
    DBG("MVD OK\n");

    MVDSTD_Config mvdCfg;
    mvdstdGenerateDefaultConfig(&mvdCfg, VID_W, VID_H, VID_W, VID_H,
                                nullptr, nullptr, nullptr);
    mvdCfg.physaddr_outdata0 = osConvertVirtToPhys(g_fifo[0].buf);
    mvdCfg.physaddr_outdata1 = osConvertVirtToPhys(g_fifo[0].buf);
    MVDSTD_SetConfig(&mvdCfg);
    DLOG(dbg, "fifo[0] virt=%08lX phys=%08lX slots=%d\n",
         (unsigned long)g_fifo[0].buf,
         (unsigned long)mvdCfg.physaddr_outdata0, g_fifoN);

    // Override probe: stock libctru returns -1 (FFFFFFFF) for a NULL config;
    // our vendored/patched mvd.c allows NULL. This single line proves which
    // mvd.c is actually linked into the running binary.
    Result nullProbe = mvdstdRenderVideoFrame(nullptr, false);
    DLOG(dbg, "NULLrender probe=%08lX (FFFFFFFF=stock libctru, else=patched mvd.c)\n",
         (unsigned long)nullProbe);

    // ─── Audio: ndsp output + Helix AAC decoder ───────────────────────────────
    // ndsp init fails (and audio stays silent) if dsp_firm wasn't dumped to the
    // SD card; that's non-fatal — video still plays. The decoder is configured
    // lazily from the first decoded frame's real sample rate / channel count.
    bool audioOn = audio::init();
    g_aac = AACInitDecoder();
    DLOG(dbg, "audio: ndsp=%d aacDec=%p\n", (int)audioOn, (void*)g_aac);

    DBG("HLS streaming... B=stop\n");

    // ─── Decode loop ─────────────────────────────────────────────────────────
    int  pmtPid    = -1;
    int  vidPid    = -1;
    int  audPid    = -1;
    u32  pesLen    = 0;
    bool pesActive = false;
    u32  audLen    = 0;
    bool audActive = false;
    bool stop       = false;
    bool mvdFirst   = true;   // first access unit is fed twice (params, then decode)
    u32  frameCount = 0;
    u32  pktCount   = 0;

    bool dbgComboPrev = false;   // edge-detect for the X + D-Pad Up debug toggle
    bool prodDoneLogged = false; // one-time log when the download stream ends
    double seekReq = -1.0;       // seek target (sec) requested via D-Pad, -1 = none
    bool seekPrevL = false, seekPrevR = false;   // edge-detect (held-based: inner
                                 // hidScanInput calls would eat hidKeysDown edges)
    bool paused    = false;      // A toggles: demux, decode, blit and DSP all halt
    bool pausePrevA = false;     // edge-detect for A (held-based, as above)
    s64  pausedAt  = 0;          // osGetTime() when the pause began

    // Seek-bar state: position from PES PTS, total from the Jellyfin item.
    double    durSec      = runTimeTicks > 0 ? runTimeTicks / 10000000.0 : 0.0;
    double    posSec      = startSec;      // resume offset; PTS delta is added below
    std::vector<SubtitleCue> subtitleCues = parseVtt(subtitleVtt);
    if (dbg) {
        fprintf(dbg, "subtitle cues=%lu\n", (unsigned long)subtitleCues.size());
        fflush(dbg);
    }
    long long firstPts    = -1;          // PTS of the first frame (position origin)
    long long curPesPts   = -1;          // PTS of the access unit now being accumulated
    s64       lastBottomRender = -1000;

    if (!g_dbg) {
        presentPlaybackBottom(startSec, durSec, false);
    }

    // Start the background download thread filling the ring. Same priority as the
    // main thread so the two round-robin on the core; the main thread yields often
    // (pacing sleeps, vblank waits), letting the producer keep the ring topped up.
    g_ring.playlistUrl  = url;
    g_ring.dbg          = dbg;
    g_ring.head         = 0;
    g_ring.tail         = 0;
    g_ring.producerDone = false;
    g_ring.consumerStop = false;
    LightLock_Init(&g_ring.lock);
    s32 mainPrio = 0x30;
    svcGetThreadPriority(&mainPrio, CUR_THREAD_HANDLE);
    // core 0 (same as main): equal-priority round-robin, and a shared L1 so the
    // ring memory is trivially coherent between producer and consumer.
    Thread dlThr = threadCreate(dlThread, &g_ring, 32 * 1024, mainPrio, 0, false);
    if (dbg) { fprintf(dbg, "dlThread=%p prio=%ld\n", (void*)dlThr, (long)mainPrio); fflush(dbg); }

    // "Buffering…" hint at the top of the console so a stall reads as buffering
    // rather than a crash. rebuf tracks whether the hint is currently shown.
    bool rebuf = true;
    if (!g_dbg) printf("\x1b[%d;5HBuffering...   ", ROW_STATUS);

    // Prebuffer: wait until the ring holds PREBUF_SZ (or the stream ended) so a
    // brief network dip after playback starts doesn't immediately underrun.
    while (!stop && ringUsed() < PREBUF_SZ && !g_ring.producerDone) {
        hidScanInput();
        if (hidKeysDown() & KEY_B) { stop = true; break; }
        svcSleepThread(10000000LL);   // 10ms
    }

    // A single reusable TS packet scratch (the ring hands out whole packets).
    u8 pkt[TS_SZ];

    while (!stop) {
        hidScanInput();

        // Toggle the on-screen debug overlay when X + D-Pad Up are held together.
        bool dbgCombo = (hidKeysHeld() & KEY_X) && (hidKeysHeld() & KEY_DUP);
        if (dbgCombo && !dbgComboPrev) {
            g_dbg = !g_dbg;
            if (g_dbg) printf("[debug ON]\n");
            else {                                     // back to clean view: redraw all
                consoleClear();
                drawMeta(series, title, year);
                drawControls();
                lastBottomRender = -1000;              // force a full HUD redraw
            }
        }
        dbgComboPrev = dbgCombo;

        // Seek: D-Pad Left/Right jump -10s/+30s. The live transcode can't be
        // seeked in-stream, so hand the target back to the caller, which starts
        // a fresh stream there (same path as resume).
        bool skL = (hidKeysHeld() & KEY_DLEFT)  != 0;
        bool skR = (hidKeysHeld() & KEY_DRIGHT) != 0;
        if ((skL && !seekPrevL) || (skR && !seekPrevR)) {
            double t = posSec + ((skR && !seekPrevR) ? 30.0 : -10.0);
            if (durSec > 0 && t > durSec - 10.0) t = durSec - 10.0;
            if (t < 0) t = 0;
            seekReq = t;
        }
        seekPrevL = skL; seekPrevR = skR;
        if (seekReq >= 0) {
            // Same slot as the buffering hint. It stays up through teardown and
            // the caller's restart — the next playerPlay's consoleInit clears it
            // — so the gap between the press and the new stream isn't dead air.
            if (!g_dbg) printf("\x1b[%d;5HSeeking...     ", ROW_STATUS);
            if (dbg) { fprintf(dbg, "seek to %.1fs (from %.1fs)\n", seekReq, posSec); fflush(dbg); }
            break;
        }

        if (hidKeysDown() & KEY_B) { stop = true; break; }

        // Pause/resume on A. Held-based edge detect for the same reason as seek.
        // Pausing simply stops the loop doing any work: no demux, no decode, no
        // blit (so the last frame stays on screen) and the DSP channel is halted.
        // The download thread keeps filling the ring and naps once it is full, so
        // a pause applies HTTP backpressure to the transcode rather than losing data.
        bool aHeld = (hidKeysHeld() & KEY_A) != 0;
        if (aHeld && !pausePrevA) {
            paused = !paused;
            audio::setPaused(paused);
            if (paused) {
                pausedAt = (s64)osGetTime();
                if (!g_dbg) printf("\x1b[%d;5HPaused         ", ROW_STATUS);
                if (dbg) { fprintf(dbg, "paused at %.1fs\n", posSec); fflush(dbg); }
            } else {
                // Wall time ran on while the stream stood still, so every queued
                // frame would now look late and the servo would snap. Push the
                // pacer's anchor forward by exactly the pause length instead.
                s64 held = (s64)osGetTime() - pausedAt;
                g_paceWall0 += held;
                if (!g_dbg) { printf("\x1b[%d;5H               ", ROW_STATUS); rebuf = false; }
                if (dbg) { fprintf(dbg, "resumed after %lldms\n", (long long)held); fflush(dbg); }
            }
        }
        pausePrevA = aHeld;

        if (paused) {
            s64 pausedNow = (s64)osGetTime();
            if (!g_dbg && pausedNow - lastBottomRender >= 66) {
                presentPlaybackBottom(posSec, durSec, true);
                lastBottomRender = pausedNow;
            }
            svcSleepThread(30000000LL);
            continue;
        }

        // Drain whole TS packets out of the ring. Decoding a frame paces+blits
        // inside processH264 (it may sleep); meanwhile dlThread keeps refilling the
        // ring. Cap the batch so the debug toggle and seek bar stay responsive.
        int processed = 0;
        int batchLimit = audioOnly ? 8 : 128;
        // Music has no video pacer. Leave the compressed stream in the ring while
        // the DSP queue is comfortably full; unlike the old wait inside
        // processAAC(), this returns to the outer loop every frame so controls,
        // lyrics, and the progress bar remain responsive.
        while (!stop && (!audioOnly || audio::queuedBufs() < 24) &&
               ringUsed() >= TS_SZ &&
               processed < batchLimit) {
            ringTake(pkt, TS_SZ);
            processed++;
            pktCount++;
            if (pktCount % 500 == 0) {
                DBG("pkts=%u frms=%u\n", (unsigned)pktCount, (unsigned)frameCount);
                if (dbg) {
                    fprintf(dbg,"pkts=%u pmtPid=%d vidPid=%d frames=%u used=%u\n",
                            (unsigned)pktCount, pmtPid, vidPid, (unsigned)frameCount,
                            (unsigned)ringUsed());
                    fflush(dbg);
                }
            }

            if (pkt[0] != 0x47) continue;
            int pid = tsPid(pkt);
            int psz = 0;
            const u8* pay = tsPayload(pkt, &psz);
            bool pusi = tsPUSI(pkt);

            if (pid==0 && pay && pmtPid==-1) {
                pmtPid = parsePAT(pay, psz);
                if (pmtPid!=-1) {
                    DBG("PAT->pmtPid=%d\n", pmtPid);
                    if (dbg) { fprintf(dbg,"PAT pmtPid=%d\n",pmtPid); fflush(dbg); }
                }
            } else if (pmtPid!=-1 && pid==pmtPid && pay &&
                       (vidPid==-1 || audPid==-1)) {
                int foundAudio = -1;
                int foundVideo = parsePMT(pay, psz, &foundAudio);
                if (foundVideo >= 0) vidPid = foundVideo;
                if (foundAudio >= 0) audPid = foundAudio;
                if (vidPid!=-1 || audPid!=-1) {
                    DBG("PMT->vidPid=%d audPid=%d\n", vidPid, audPid);
                    if (dbg) { fprintf(dbg,"PMT vidPid=%d audPid=%d\n",vidPid,audPid); fflush(dbg); }
                }
            } else if (audPid!=-1 && pid==audPid && pay) {
                // Audio elementary stream: accumulate a PES, then decode its ADTS
                // frames when the next PES starts (PUSI).
                if (pusi) {
                    if (audActive && audLen > 0)
                        processAAC(audBuf, (int)audLen, dbg);
                    // PES PTS = presentation time of the first ADTS frame starting
                    // here; re-syncs g_audNextPts each PES so per-frame accumulation
                    // error (or dropped/corrupt frames) can't build up.
                    long long apts = pesPTS(pay, psz);
                    if (apts >= 0) {
                        g_audNextPts = apts / 90000.0;
                        // Audio-only music has no video PTS to drive the seek bar.
                        // Anchor it to the first audio PES instead.
                        if (vidPid < 0) {
                            if (firstPts < 0) firstPts = apts;
                            long long d = apts - firstPts;
                            if (d < 0) d += (1LL << 33);
                            posSec = startSec + d / 90000.0;
                        }
                    }
                    int skip = pesHeaderLen(pay, psz);
                    audLen = 0; audActive = true;
                    int cp = psz - skip;
                    if (cp > 0 && (u32)cp <= PES_SZ) { memcpy(audBuf, pay+skip, cp); audLen = cp; }
                } else if (audActive && audLen+psz <= PES_SZ) {
                    memcpy(audBuf+audLen, pay, psz); audLen += psz;
                }
            } else if (vidPid!=-1 && pid==vidPid && pay) {
                if (pusi) {
                    long long pts = pesPTS(pay, psz);
                    if (pts >= 0) {
                        if (firstPts < 0) {
                            firstPts = pts;
                            g_vidFirstPts = pts;
                            // Audio demuxed before the first video PES is stale
                            // resume priming if it runs well behind the video
                            // start — purge it or playback begins seconds
                            // desynced. Harmless no-op on aligned streams.
                            if (g_audNextPts >= 0.0 &&
                                g_audNextPts < pts / 90000.0 - 0.5) {
                                audio::flushQueue();
                                if (dbg) { fprintf(dbg, "flushed stale audio (aud=%.2f vid=%.2f)\n",
                                                   g_audNextPts, pts / 90000.0); fflush(dbg); }
                            }
                        }
                        long long d = pts - firstPts;
                        if (d < 0) d += (1LL << 33);   // 33-bit PTS wraparound
                        posSec = startSec + d / 90000.0;
                    }
                    if (pesActive && pesLen > 0)
                        processH264(pesBuf,pesLen,nalBuf,&mvdCfg,&mvdFirst,&stop,&frameCount,dbg,curPesPts);
                    curPesPts = pts;   // PTS now belongs to the AU starting here
                    int skip = pesHeaderLen(pay,psz);
                    pesLen = 0; pesActive = true;
                    int cp = psz-skip;
                    if (cp>0 && (u32)cp<=PES_SZ) { memcpy(pesBuf,pay+skip,cp); pesLen=cp; }
                } else if (pesActive && pesLen+psz<=PES_SZ) {
                    memcpy(pesBuf+pesLen,pay,psz); pesLen+=psz;
                }
            }
        }

        // Display any frames that came due (also keeps video moving through
        // network stalls, when the batch loop above has nothing to decode).
        if (vidPid >= 0) displayPump(false, false, &stop, dbg);

        // One-time note when the download stream ends — distinguishes a normal
        // end-of-file from the server silently stopping mid-stream (throttling).
        if (g_ring.producerDone && !prodDoneLogged) {
            prodDoneLogged = true;
            if (dbg) { fprintf(dbg, "producer done: pkts=%u used=%u\n",
                               (unsigned)pktCount, (unsigned)ringUsed()); fflush(dbg); }
        }

        // Buffering indicator: clear once frames flow again, re-show on underrun.
        if (!g_dbg) {
            if (processed > 0) {
                if (rebuf) { printf("\x1b[%d;5H               ", ROW_STATUS); rebuf = false; }
            } else if (!g_ring.producerDone &&
                       !(audioOnly && audio::queuedBufs() >= 24) && !rebuf) {
                printf("\x1b[%d;5HBuffering...   ", ROW_STATUS); rebuf = true;
            }
        }

        // With no video PTS, the DSP clock is the authoritative music position.
        if (audioOnly && firstPts >= 0) {
            double clock = audio::audioClock();
            if (clock >= 0) posSec = startSec + clock - firstPts / 90000.0;
        }

        // Fully redraw and present the active bottom buffer at 15 Hz. Partial
        // console updates were correct but Azahar periodically presented the
        // untouched alternate buffer, making the HUD flash and vanish.
        s64 renderNow = (s64)osGetTime();
        if (!g_dbg && renderNow - lastBottomRender >= 66) {
            presentPlaybackBottom(posSec, durSec, paused);
            lastBottomRender = renderNow;
        }

        // Stream finished and fully drained → done.
        if (g_ring.producerDone && ringUsed() < TS_SZ) break;
        // Nothing to do this pass (waiting on the network) → yield briefly.
        if (processed == 0) svcSleepThread(5000000LL);   // 5ms
    }

    // Leaving while paused (B or a seek): un-halt the channel so the next stream
    // isn't silent, and drop what's queued rather than draining it at pace.
    if (paused) { audio::setPaused(false); paused = false; stop = true; }

    // Show whatever is still queued at its proper pace before tearing down —
    // unless the user is seeking away, in which case just drop it.
    if (!stop && seekReq < 0 && audActive && audLen > 0)
        processAAC(audBuf, (int)audLen, dbg);
    if (!stop && seekReq < 0 && vidPid >= 0)
        displayPump(false, true, &stop, dbg);

    if (finishedOut)
        *finishedOut = !stop && seekReq < 0 &&
                       g_ring.producerDone && ringUsed() < TS_SZ;

    DBG("End: pkts=%u frms=%u\n", (unsigned)pktCount, (unsigned)frameCount);
    g_paceLog = nullptr;
    if (dbg) {
        fprintf(dbg,"End: pkts=%u pmtPid=%d vidPid=%d dec=%u disp=%u\n",
                (unsigned)pktCount, pmtPid, vidPid, (unsigned)frameCount,
                (unsigned)g_dispCount);
        fflush(dbg);
    }

    if (!stop && seekReq < 0)
        svcSleepThread(2000000000LL); // show stats for 2s before returning

    // Stop the producer. HLS responses are finite, so an in-flight request
    // completes promptly without cancelling a continuous HTTP connection.
    g_ring.consumerStop = true;
    threadJoin(dlThr, 15000000000LL);
    threadFree(dlThr);
    if (dbg) fclose(dbg);
    AACFreeDecoder(g_aac); g_aac = nullptr;
    audio::exit();
    mvdstdExit();
    linearFree(g_ring.data); linearFree(pesBuf);
    linearFree(nalBuf); linearFree(audBuf);
    freeFifoSlots();
    if (seekOut) *seekOut = seekReq;
    return true;
}
