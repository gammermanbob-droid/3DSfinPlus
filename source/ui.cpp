#include "ui.h"
#include "image.h"
#include "jellyfin_logo.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

// ---- Palette ---------------------------------------------------------------
static constexpr u32 COL_BG        = C2D_Color32(0x1a, 0x1a, 0x2e, 0xFF);
static constexpr u32 COL_BG_BOT    = C2D_Color32(0x16, 0x16, 0x28, 0xFF);
static constexpr u32 COL_BAR       = C2D_Color32(0x0f, 0x3c, 0x78, 0xFF);
static constexpr u32 COL_SEL       = C2D_Color32(0x1a, 0x6b, 0xb5, 0xFF);
static constexpr u32 COL_ROW_ALT   = C2D_Color32(0x22, 0x22, 0x3a, 0xFF);
static constexpr u32 COL_WHITE     = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF);
static constexpr u32 COL_GREY      = C2D_Color32(0xaa, 0xaa, 0xaa, 0xFF);
static constexpr u32 COL_YELLOW    = C2D_Color32(0xFF, 0xd7, 0x00, 0xFF);
static constexpr u32 COL_RED       = C2D_Color32(0xFF, 0x44, 0x44, 0xFF);
static constexpr u32 COL_GREEN     = C2D_Color32(0x44, 0xFF, 0x88, 0xFF);

// ---- Helpers ---------------------------------------------------------------

std::string UI::formatDuration(long long ticks) {
    if (ticks <= 0) return "";
    long long secs  = ticks / 10000000LL;
    int h = (int)(secs / 3600);
    int m = (int)((secs % 3600) / 60);
    int s = (int)(secs % 60);
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else        snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return std::string(buf);
}

std::string UI::truncate(const std::string& str, size_t maxLen) {
    if (str.size() <= maxLen) return str;
    return str.substr(0, maxLen - 3) + "...";
}

// ---- Construction ----------------------------------------------------------

UI::UI(C3D_RenderTarget* top, C3D_RenderTarget* bot)
    : top_(top), bot_(bot), logoImg_{} {
    font_    = C2D_FontLoadSystem(CFG_REGION_USA);
    textBuf_ = C2D_TextBufNew(4096);
    Image_loadFromMemory(jellyfin_logo_png, jellyfin_logo_png_len, &logoImg_);
}

UI::~UI() {
    Image_free(&logoImg_);
    C2D_TextBufDelete(textBuf_);
    C2D_FontFree(font_);
}

// ---- Low-level draw helpers ------------------------------------------------

void UI::drawText(const std::string& str, float x, float y, float scale, u32 color) {
    C2D_Text t;
    C2D_TextFontParse(&t, font_, textBuf_, str.c_str());
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

void UI::drawTextBuf(const std::string& str, float x, float y, float scale, u32 color,
                     float maxWidth) {
    // Simple character-level truncation based on approximate char width
    float charW   = scale * 11.0f; // rough estimate for system font
    int   maxChars = static_cast<int>(maxWidth / charW);
    drawText(truncate(str, maxChars < 4 ? 4 : (size_t)maxChars), x, y, scale, color);
}

void UI::drawRect(float x, float y, float w, float h, u32 color) {
    C2D_DrawRectSolid(x, y, 0.0f, w, h, color);
}

void UI::drawTopBar(const std::string& title) {
    drawRect(0, 0, TOP_W, 22, COL_BAR);
    drawText("3DSFin", 6, 4, 0.50f, COL_YELLOW);
    drawText(title,    80, 4, 0.50f, COL_WHITE);
}

void UI::drawBottomHints(const std::string& hints) {
    drawRect(0, BOT_H - 20, BOT_W, 20, COL_BAR);
    drawText(hints, 6, BOT_H - 17, 0.42f, COL_GREY);
}

void UI::drawScrollList(const std::vector<std::string>& rows, int selected, int offset) {
    int startY = 26;
    for (int i = 0; i < VISIBLE_ROWS; i++) {
        int idx = offset + i;
        if (idx >= (int)rows.size()) break;
        float ry = static_cast<float>(startY + i * ROW_HEIGHT);
        u32 bg   = (idx == selected) ? COL_SEL
                 : (i % 2 == 0)      ? COL_BG
                                      : COL_ROW_ALT;
        drawRect(0, ry, TOP_W, ROW_HEIGHT, bg);
        drawTextBuf(rows[idx], 8, ry + 5, 0.50f, COL_WHITE, TOP_W - 16);
    }

    // Scroll indicator
    if ((int)rows.size() > VISIBLE_ROWS) {
        float barH    = TOP_H - 26;
        float thumbH  = barH * VISIBLE_ROWS / rows.size();
        float thumbY  = 26 + barH * offset / rows.size();
        drawRect(TOP_W - 4, 26,     4, barH,   COL_ROW_ALT);
        drawRect(TOP_W - 4, thumbY, 4, thumbH, COL_GREY);
    }
}

// ---- Frame -----------------------------------------------------------------

void UI::beginFrame() {
    C2D_TextBufClear(textBuf_);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(top_, COL_BG);
    C2D_TargetClear(bot_, COL_BG_BOT);
}

void UI::endFrame() {
    C3D_FrameEnd(0);
}

// ---- Screens ---------------------------------------------------------------

void UI::drawSetupScreen(const std::string& currentUrl) {
    C2D_SceneBegin(top_);
    drawTopBar("Server Setup");
    drawText("Press A to enter your Jellyfin server URL", 8, 60, 0.50f, COL_WHITE);
    drawText("Example:  http://192.168.1.x:8096",         8, 90, 0.48f, COL_GREY);
    if (!currentUrl.empty()) {
        drawText("Current:  " + currentUrl, 8, 120, 0.48f, COL_GREEN);
    }

    C2D_SceneBegin(bot_);
    drawBottomHints("A: Enter URL   START: Quit");
}

void UI::drawLoginScreen(const std::string& serverUrl, const std::string& username) {
    C2D_SceneBegin(top_);
    drawTopBar("Login");
    drawText("Server:",   8, 50,  0.50f, COL_GREY);
    drawTextBuf(serverUrl, 80, 50, 0.48f, COL_WHITE, TOP_W - 88);
    drawText("Press A to log in", 8, 100, 0.52f, COL_WHITE);
    if (!username.empty())
        drawText("User: " + username, 8, 130, 0.48f, COL_GREEN);

    C2D_SceneBegin(bot_);
    drawBottomHints("A: Enter credentials   B: Change server");
}

void UI::drawLoadingScreen(const std::string& msg) {
    C2D_SceneBegin(top_);
    drawTopBar("Loading");
    drawText(msg, 8, 110, 0.55f, COL_WHITE);

    C2D_SceneBegin(bot_);
}

void UI::drawServerRefreshScreen(const std::vector<JellyfinScanProgress>& scans, bool complete) {
    C2D_SceneBegin(top_);
    drawTopBar("Library and guide scans");
    drawRect(0, 26, TOP_W, TOP_H - 26, COL_BG);
    const char* names[] = {"Libraries", "Guide"};
    for (size_t i = 0; i < scans.size() && i < 2; ++i) {
        const auto& scan = scans[i];
        float y = 38 + i * 78;
        std::string label = names[i];
        if (scan.percent >= 0) {
            char value[24];
            snprintf(value, sizeof(value), "  %.0f%%", scan.percent);
            label += value;
        }
        drawText(label, 12, y, 0.52f, COL_WHITE);
        drawRect(12, y + 24, TOP_W - 24, 10, COL_ROW_ALT);
        if (scan.percent >= 0)
            drawRect(12, y + 24, (TOP_W - 24) * scan.percent / 100.f, 10,
                     scan.completed ? COL_GREEN : COL_SEL);
        drawText(scan.message, 12, y + 39, 0.45f, COL_GREY);
    }
    bool active = false;
    for (const auto& scan : scans) active = active || scan.active;
    drawText(complete ? "Scans complete - ready to reload!" :
             active ? "Scan status updates automatically." : "Scan ended - check results above.",
             12, 207, 0.48f, complete ? COL_GREEN : COL_GREY);
    C2D_SceneBegin(bot_);
    drawText("You can go back while scans run.", 8, 60, 0.46f, COL_GREY);
    drawBottomHints("A: Reload libraries   B: Back");
}

void UI::drawErrorScreen(const std::string& msg) {
    C2D_SceneBegin(top_);
    drawTopBar("Error");
    drawRect(0, 26, TOP_W, TOP_H - 26, COL_BG);

    // Multi-line: split on \n
    float y = 60;
    std::string line;
    for (char c : msg) {
        if (c == '\n') {
            drawText(line, 8, y, 0.52f, COL_RED);
            line.clear();
            y += 26;
        } else {
            line += c;
        }
    }
    if (!line.empty()) drawText(line, 8, y, 0.52f, COL_RED);

    C2D_SceneBegin(bot_);
    drawBottomHints("A/B: Back to login");
}

// Draw an image so it fills the whole x/y/w/h box without distortion ("cover"):
// scale uniformly to cover the box, then center-crop the overflow by trimming the
// subtexture's UV rect to the box's aspect ratio. (C2D_DrawImageAt with separate
// sx/sy would instead *stretch* the art, smearing portrait covers into the card.)
static void drawImageCover(const C2D_Image& im, float x, float y, float w, float h) {
    const float iw = (float)im.subtex->width;
    const float ih = (float)im.subtex->height;
    if (iw <= 0 || ih <= 0) return;

    // Work on a copy of the subtexture so we can shrink its UV window.
    Tex3DS_SubTexture st = *im.subtex;
    const float imgA  = iw / ih;
    const float boxA  = w / h;

    if (imgA > boxA) {
        // Source is wider than the box → keep full height, crop left/right.
        const float keep = boxA / imgA;                  // fraction of width shown
        const float trim = (st.right - st.left) * (1.0f - keep) * 0.5f;
        st.left  += trim;
        st.right -= trim;
        st.width  = (u16)(iw * keep);
    } else {
        // Source is taller than the box → keep full width, crop top/bottom.
        const float keep = imgA / boxA;                  // fraction of height shown
        const float trim = (st.top - st.bottom) * (1.0f - keep) * 0.5f;  // top > bottom
        st.top    -= trim;
        st.bottom += trim;
        st.height  = (u16)(ih * keep);
    }

    C2D_Image cropped = { im.tex, &st };
    C2D_DrawImageAt(cropped, x, y, 0.0f, nullptr, w / (float)st.width, h / (float)st.height);
}

// Bottom-screen touchable card grid shared by the library, item, and Continue
// Watching menus. The visible page is derived from `selected` rather than a
// separately-tracked offset, so drawing and hitTestBottomGrid() can never drift
// apart: whichever card is selected is always on-screen, on both.
void UI::drawBottomMirrorGrid(const std::vector<C2D_Image>& covers,
                              const std::vector<std::string>& labels,
                              int count, int selected,
                              bool showProgress,
                              const std::vector<float>* progressFrac) {
    if (count <= 0) {
        drawText("(nothing here)", 8, 100, 0.46f, COL_GREY);
        return;
    }

    const int perPage = BGRID_COLS * BGRID_ROWS_V;
    int page = (selected >= 0 ? selected : 0) / perPage;
    int base = page * perPage;

    for (int i = 0; i < perPage; i++) {
        int idx = base + i;
        if (idx >= count) break;

        int   c = i % BGRID_COLS, r = i / BGRID_COLS;
        float x = BGRID_MX + c * (BGRID_CARD_W + BGRID_GAP);
        float y = BGRID_MY + r * BGRID_PITCH_Y;
        bool  sel = (idx == selected);

        if (sel) drawRect(x - 2, y - 2, BGRID_CARD_W + 4, BGRID_CARD_H + 4, COL_SEL);

        bool hasImg = (idx < (int)covers.size() && covers[idx].tex != nullptr);
        if (hasImg) drawImageCover(covers[idx], x, y, BGRID_CARD_W, BGRID_CARD_H);
        else        drawRect(x, y, BGRID_CARD_W, BGRID_CARD_H, COL_ROW_ALT);

        if (showProgress && progressFrac && idx < (int)progressFrac->size()) {
            float frac = (*progressFrac)[idx];
            if (frac > 0.0f) {
                if (frac > 1.0f) frac = 1.0f;
                drawRect(x, y + BGRID_CARD_H - 4, BGRID_CARD_W,        4, C2D_Color32(0,0,0,0xC0));
                drawRect(x, y + BGRID_CARD_H - 4, BGRID_CARD_W * frac, 4, COL_YELLOW);
            }
        }

        drawRect(x, y + BGRID_CARD_H - 16, BGRID_CARD_W, 16, C2D_Color32(0, 0, 0, 0xB0));
        if (idx < (int)labels.size())
            drawTextBuf(labels[idx], x + 3, y + BGRID_CARD_H - 14, 0.34f, COL_WHITE, BGRID_CARD_W - 6);
    }

    int totalPages = (count + perPage - 1) / perPage;
    if (totalPages > 1) {
        float barH   = BOT_H - BGRID_MY - 6;
        float thumbH = barH / totalPages;
        float thumbY = BGRID_MY + thumbH * page;
        drawRect(BOT_W - 4, BGRID_MY, 4, barH,   COL_ROW_ALT);
        drawRect(BOT_W - 4, thumbY,   4, thumbH, COL_GREY);
    }
}

// Bottom-screen touchable channel list for the Live TV guide. Same
// selection-derives-the-page rule as drawBottomMirrorGrid.
void UI::drawLiveTvRows(const std::vector<JellyfinItem>& channels,
                        const std::vector<C2D_Image>& icons,
                        int selected) {
    if (channels.empty()) {
        drawText("(no channels found)", 8, 100, 0.46f, COL_GREY);
        return;
    }

    int page = (selected >= 0 ? selected : 0) / LIVE_ROWS_V;
    int base = page * LIVE_ROWS_V;

    for (int i = 0; i < LIVE_ROWS_V; i++) {
        int idx = base + i;
        if (idx >= (int)channels.size()) break;

        float y   = LIVE_ROW_Y0 + i * LIVE_ROW_H;
        bool  sel = (idx == selected);
        drawRect(0, y, BOT_W, LIVE_ROW_H - 2,
                 sel ? COL_SEL : (i % 2 == 0 ? COL_BG_BOT : COL_ROW_ALT));

        bool hasImg = (idx < (int)icons.size() && icons[idx].tex != nullptr);
        if (hasImg) drawImageCover(icons[idx], 6, y + 4, 38, 38);
        else        drawRect(6, y + 4, 38, 38, COL_ROW_ALT);

        drawTextBuf(channels[idx].name, 52, y + 3, 0.44f, COL_WHITE, BOT_W - 60);
        const std::string& prog = channels[idx].currentProgram;
        drawTextBuf(prog.empty() ? "No programme data" : prog,
                    52, y + 23, 0.36f, COL_GREY, BOT_W - 60);
    }

    int totalPages = ((int)channels.size() + LIVE_ROWS_V - 1) / LIVE_ROWS_V;
    if (totalPages > 1) {
        float barH   = LIVE_ROWS_V * LIVE_ROW_H;
        float thumbH = barH / totalPages;
        float thumbY = LIVE_ROW_Y0 + thumbH * page;
        drawRect(BOT_W - 4, LIVE_ROW_Y0, 4, barH,   COL_ROW_ALT);
        drawRect(BOT_W - 4, thumbY,      4, thumbH, COL_GREY);
    }
}

// Tab strip shown on the top screen of all three home menus.
void UI::drawHomeMenuTabs(HomeMenuTab active) {
    const char* names[3] = {"Library", "Continue Watching", "Live TV"};
    float y = 24.0f;
    drawRect(0, y, TOP_W, 20, COL_BG);
    float x = 8.0f;
    for (int i = 0; i < 3; i++) {
        u32 col = (i == (int)active) ? COL_YELLOW : COL_GREY;
        std::string label = std::string(i == (int)active ? "> " : "  ") + names[i];
        drawText(label, x, y + 2, 0.40f, col);
        x += 132.0f;
    }
    drawText("L/R", TOP_W - 34, y + 2, 0.38f, COL_GREY);
}

int UI::hitTestBottomGrid(int touchX, int touchY, int count, int selected) {
    if (count <= 0) return -1;
    const int perPage = BGRID_COLS * BGRID_ROWS_V;
    int page = (selected >= 0 ? selected : 0) / perPage;
    int base = page * perPage;

    if (touchX < BGRID_MX || touchY < BGRID_MY) return -1;
    float relX = touchX - BGRID_MX;
    float relY = touchY - BGRID_MY;
    int c = (int)(relX / (BGRID_CARD_W + BGRID_GAP));
    int r = (int)(relY / BGRID_PITCH_Y);
    if (c < 0 || c >= BGRID_COLS || r < 0 || r >= BGRID_ROWS_V) return -1;
    // Reject a tap landing in the gap between cards rather than on one.
    if (relX - c * (BGRID_CARD_W + BGRID_GAP) > BGRID_CARD_W) return -1;
    if (relY - r * BGRID_PITCH_Y > BGRID_CARD_H) return -1;

    int idx = base + r * BGRID_COLS + c;
    if (idx < 0 || idx >= count) return -1;
    return idx;
}

int UI::hitTestLiveList(int touchX, int touchY, int count, int selected) {
    if (count <= 0) return -1;
    if (touchX < 0 || touchX >= BOT_W || touchY < LIVE_ROW_Y0) return -1;
    int page = (selected >= 0 ? selected : 0) / LIVE_ROWS_V;
    int base = page * LIVE_ROWS_V;
    int row = (int)((touchY - LIVE_ROW_Y0) / LIVE_ROW_H);
    if (row < 0 || row >= LIVE_ROWS_V) return -1;
    int idx = base + row;
    if (idx < 0 || idx >= count) return -1;
    return idx;
}

void UI::drawLibraryGrid(const std::vector<JellyfinLibrary>& libs,
                         const std::vector<C2D_Image>& covers,
                         int selected) {
    // Top screen: branding only. The library grid used to be drawn here too,
    // at a different column count than its bottom-screen touchable mirror —
    // since both shared one selection index, D-Pad/circle-pad presses moved
    // the highlight to a different-looking position on each screen, which
    // read as the cursor drifting diagonally. There is now exactly one grid
    // (bottom screen only, see below), so that mismatch can't recur.
    C2D_SceneBegin(top_);
    drawTopBar("Libraries");
    drawHomeMenuTabs(TAB_LIBRARY);

    if (logoImg_.tex) {
        float lw = 150.0f, lh = 150.0f;
        C2D_DrawImageAt(logoImg_, (TOP_W - lw) / 2.0f, 56.0f, 0.3f,
                        nullptr, lw / (float)logoImg_.subtex->width,
                        lh / (float)logoImg_.subtex->height);
    }
    drawText("3DSFin", (TOP_W - 6 * 0.55f * 11.0f) / 2.0f, 212, 0.55f, COL_WHITE);

    // Bottom screen: the touchable library grid (the top screen has no
    // digitizer, so this is the only way to select a library by tapping it).
    C2D_SceneBegin(bot_);
    drawRect(0, 0, BOT_W, BOT_H, COL_BG_BOT);

    std::vector<std::string> labels;
    labels.reserve(libs.size());
    for (const auto& l : libs) labels.push_back(l.name);
    drawBottomMirrorGrid(covers, labels, (int)libs.size(), selected);

    // Search button (series only), top-right corner.
    drawRect(BOT_W - 72, 2, 68, 20, COL_BAR);
    drawText("Search", BOT_W - 64, 5, 0.38f, COL_WHITE);

    drawBottomHints("A: Open  Y: Search  X: Scan  L/R: Menu");
}

// Continue Watching menu (Menu 2). Top screen: a detail panel for the
// highlighted item. Bottom screen: the touchable poster grid.
void UI::drawContinueWatchingMenu(const std::vector<JellyfinItem>& resume,
                                  const std::vector<C2D_Image>& covers,
                                  int selected) {
    C2D_SceneBegin(top_);
    drawTopBar("Continue Watching");
    drawHomeMenuTabs(TAB_CONTINUE);

    if (resume.empty()) {
        drawText("Nothing in progress.", 12, 110, 0.52f, COL_GREY);
    } else if (selected >= 0 && selected < (int)resume.size()) {
        const auto& it = resume[selected];
        bool hasImg = selected < (int)covers.size() && covers[selected].tex != nullptr;
        float px = 12, py = 50, pw = 130, ph = 170;
        if (hasImg) drawImageCover(covers[selected], px, py, pw, ph);
        else        drawRect(px, py, pw, ph, COL_ROW_ALT);

        float tx = px + pw + 16;
        const std::string& title = !it.seriesName.empty() ? it.seriesName : it.name;
        drawTextBuf(title, tx, py + 4, 0.52f, COL_WHITE, TOP_W - tx - 10);
        if (!it.seriesName.empty())
            drawTextBuf(it.name, tx, py + 30, 0.42f, COL_GREY, TOP_W - tx - 10);

        if (it.productionYear > 0) {
            char yr[32];
            snprintf(yr, sizeof(yr), "Year: %d", it.productionYear);
            drawText(yr, tx, py + 58, 0.42f, COL_GREY);
        }
        std::string dur = formatDuration(it.runTimeTicks);
        if (!dur.empty()) drawText("Duration: " + dur, tx, py + 80, 0.42f, COL_GREY);

        if (it.runTimeTicks > 0) {
            float frac = (float)it.resumeTicks / (float)it.runTimeTicks;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            char pct[24];
            snprintf(pct, sizeof(pct), "%.0f%% watched", frac * 100.0f);
            drawText(pct, tx, py + 102, 0.42f, COL_YELLOW);
            drawRect(tx, py + 124, TOP_W - tx - 20, 8, COL_ROW_ALT);
            drawRect(tx, py + 124, (TOP_W - tx - 20) * frac, 8, COL_YELLOW);
        }
    }

    C2D_SceneBegin(bot_);
    drawRect(0, 0, BOT_W, BOT_H, COL_BG_BOT);

    std::vector<std::string> labels;
    std::vector<float>       frac;
    labels.reserve(resume.size());
    frac.reserve(resume.size());
    for (const auto& it : resume) {
        labels.push_back(!it.seriesName.empty() ? it.seriesName : it.name);
        frac.push_back(it.runTimeTicks > 0
                      ? (float)it.resumeTicks / (float)it.runTimeTicks : 0.0f);
    }
    drawBottomMirrorGrid(covers, labels, (int)resume.size(), selected, true, &frac);

    drawBottomHints(resume.empty() ? "L/R: Menu"
                                    : "A: Play  Y: Subtitles  SELECT: Audio  L/R: Menu");
}

// Live TV guide (Menu 3). Top screen: the highlighted channel's icon, name,
// and current programme. Bottom screen: the touchable channel list.
void UI::drawLiveTvGuide(const std::vector<JellyfinItem>& channels,
                         const std::vector<C2D_Image>& icons,
                         int selected) {
    C2D_SceneBegin(top_);
    drawTopBar("Live TV Guide");
    drawHomeMenuTabs(TAB_LIVETV);

    if (channels.empty()) {
        drawText("No Live TV channels found.", 12, 110, 0.50f, COL_GREY);
    } else if (selected >= 0 && selected < (int)channels.size()) {
        const auto& ch = channels[selected];
        bool hasImg = selected < (int)icons.size() && icons[selected].tex != nullptr;
        float ix = 12, iy = 50, iw = 120, ih = 120;
        if (hasImg) drawImageCover(icons[selected], ix, iy, iw, ih);
        else        drawRect(ix, iy, iw, ih, COL_ROW_ALT);

        float tx = ix + iw + 18;
        drawTextBuf(ch.name, tx, iy + 6, 0.56f, COL_WHITE, TOP_W - tx - 10);
        drawText("Now playing:", tx, iy + 44, 0.42f, COL_GREY);
        drawTextBuf(ch.currentProgram.empty() ? "(no programme data)" : ch.currentProgram,
                    tx, iy + 66, 0.46f, COL_YELLOW, TOP_W - tx - 10);
    }

    C2D_SceneBegin(bot_);
    drawRect(0, 0, BOT_W, BOT_H, COL_BG_BOT);
    drawLiveTvRows(channels, icons, selected);
    drawBottomHints(channels.empty() ? "L/R: Menu" : "A: Watch Live   L/R: Menu");
}

void UI::drawItemGrid(const std::vector<JellyfinItem>& items,
                      const std::vector<C2D_Image>& covers,
                      int selected,
                      const std::string& title) {
    // The interactive grid lives only on the touchable bottom screen (see
    // below); the top screen just shows the title bar and the Jellyfin
    // logo, same treatment as the Library home menu.
    C2D_SceneBegin(top_);
    drawTopBar(title);

    if (logoImg_.tex) {
        float lw = 150.0f, lh = 150.0f;
        C2D_DrawImageAt(logoImg_, (TOP_W - lw) / 2.0f, 56.0f, 0.3f,
                        nullptr, lw / (float)logoImg_.subtex->width,
                        lh / (float)logoImg_.subtex->height);
    }

    if (items.empty())
        drawText("(no items found)", (TOP_W - 17 * 0.50f * 11.0f) / 2.0f, 216, 0.50f, COL_GREY);

    // Bottom screen: the touchable grid, with a one-line metadata strip for
    // the selected item beneath it.
    C2D_SceneBegin(bot_);
    drawRect(0, 0, BOT_W, BOT_H, COL_BG_BOT);

    {
        std::vector<std::string> labels;
        labels.reserve(items.size());
        std::vector<float> frac;
        frac.reserve(items.size());
        for (const auto& it : items) {
            labels.push_back(it.name);
            frac.push_back(it.runTimeTicks > 0
                          ? (float)it.resumeTicks / (float)it.runTimeTicks : 0.0f);
        }
        drawBottomMirrorGrid(covers, labels, (int)items.size(), selected, true, &frac);
    }

    if (!items.empty() && selected < (int)items.size()) {
        auto& it = items[selected];
        std::string meta = it.type;
        if (it.productionYear > 0) {
            char yr[16];
            snprintf(yr, sizeof(yr), "  %d", it.productionYear);
            meta += yr;
        }
        std::string dur = formatDuration(it.runTimeTicks);
        if (!dur.empty()) meta += "  " + dur;
        drawTextBuf(truncate(it.name, 34) + "  -  " + meta, 8, 196, 0.36f, COL_GREY, BOT_W - 16);
    }

    // Series, seasons, artists, albums, and folders drill in; media plays.
    bool drillable = !items.empty() && selected < (int)items.size()
                  && (items[selected].type == "Series" ||
                      items[selected].type == "Season" ||
                      items[selected].type == "MusicArtist" ||
                      items[selected].type == "MusicAlbum" ||
                      items[selected].type == "Artist" ||
                      items[selected].type == "Album" ||
                      items[selected].type == "Folder" ||
                      items[selected].type == "Playlist" ||
                      items[selected].type == "BoxSet");
    bool music = !items.empty() && selected < (int)items.size()
              && items[selected].type == "Audio";
    bool live = !items.empty() && selected < (int)items.size()
             && (items[selected].type == "TvChannel" ||
                 items[selected].type == "LiveTvChannel");
    drawBottomHints(drillable ? "A: Open   X: Shuffle   B: Back"
                    : live ? "A: Watch Live   B: Back"
                    : music ? "A: Play   X: Shuffle   B: Back"
                            : "A: Play  X: Shuffle  SELECT/Y: Tracks");
}

void UI::drawTrackScreen(const std::string& title,
                         const std::vector<std::string>& rows,
                         int selected, int offset) {
    C2D_SceneBegin(top_);
    drawTopBar("Audio Track");
    drawScrollList(rows, selected, offset);

    C2D_SceneBegin(bot_);
    drawRect(0, 0, BOT_W, BOT_H, COL_BG_BOT);
    drawTextBuf(title, 8, 40, 0.50f, COL_WHITE, BOT_W - 16);
    drawText("Save, then press A on the episode to play.", 8, 96, 0.40f, COL_GREY);
    drawBottomHints("A: Save   B: Cancel   D-Pad: Move");
}

void UI::drawSubtitleScreen(const std::string& title,
                            const std::vector<std::string>& rows,
                            int selected, int offset) {
    C2D_SceneBegin(top_);
    drawTopBar("Subtitles");
    drawScrollList(rows, selected, offset);

    C2D_SceneBegin(bot_);
    drawRect(0, 0, BOT_W, BOT_H, COL_BG_BOT);
    drawTextBuf(title, 8, 40, 0.50f, COL_WHITE, BOT_W - 16);
    drawText("Jellyfin burns subtitles into the video.", 8, 96, 0.42f, COL_GREY);
    drawBottomHints("A: Save   B: Cancel   D-Pad: Move");
}

void UI::drawPlayerScreen(const JellyfinItem& item, const std::string& streamUrl) {
    C2D_SceneBegin(top_);
    drawTopBar("Now Playing");
    drawText(truncate(item.name, 40), 8, 40, 0.52f, COL_WHITE);

    std::string dur = formatDuration(item.runTimeTicks);
    if (!dur.empty()) drawText("Duration: " + dur, 8, 70, 0.48f, COL_GREY);
    drawText("Type: " + item.type, 8, 90, 0.48f, COL_GREY);

    drawText("Stream URL ready.", 8, 120, 0.48f, COL_GREEN);
    drawText("(Video playback: Phase 2)", 8, 142, 0.44f, COL_GREY);

    // Show beginning of URL for debug
    drawTextBuf(streamUrl, 8, 168, 0.38f, COL_GREY, TOP_W - 16);

    C2D_SceneBegin(bot_);
    drawRect(0, 0, BOT_W, BOT_H, COL_BG_BOT);
    drawText("Stream URL copied to log.", 8, 80, 0.48f, COL_GREY);
    drawBottomHints("B: Back to list");
}
