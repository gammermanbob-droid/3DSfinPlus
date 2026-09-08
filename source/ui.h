#pragma once
#include <citro2d.h>
#include <string>
#include <vector>
#include "jellyfin.h"

class UI {
public:
    static constexpr int VISIBLE_ROWS  = 9;   // rows that fit on top screen
    static constexpr int ROW_HEIGHT    = 24;
    static constexpr int TOP_W         = 400;
    static constexpr int TOP_H         = 240;
    static constexpr int BOT_W         = 320;
    static constexpr int BOT_H         = 240;

    // Item cover grid (movies / series / episodes inside a library). Column
    // count intentionally matches BGRID_COLS (below): this grid and its
    // bottom-screen touchable mirror share one selection index, so they must
    // agree on columns-per-row or moving the cursor looks diagonal on one of
    // the two screens.
    static constexpr int ITEM_GRID_COLS         = 3;
    static constexpr int ITEM_GRID_ROWS_VISIBLE = 2;

    // Bottom-screen touchable mirror grid, shared by the library, item, and
    // Continue Watching menus so a tap's hit-test always matches what's drawn
    // (both sides read these same constants; there is no separate copy to drift).
    static constexpr float BGRID_MX      = 8.0f;
    static constexpr float BGRID_MY      = 28.0f;
    static constexpr float BGRID_GAP     = 8.0f;
    static constexpr int   BGRID_COLS    = 3;
    static constexpr int   BGRID_ROWS_V  = 2;
    static constexpr float BGRID_CARD_W  = (BOT_W - 2*BGRID_MX - (BGRID_COLS-1)*BGRID_GAP) / BGRID_COLS;
    static constexpr float BGRID_CARD_H  = 76.0f;
    static constexpr float BGRID_PITCH_Y = BGRID_CARD_H + 10.0f;

    // Live TV guide list (bottom screen): one row per channel.
    static constexpr float LIVE_ROW_Y0  = 26.0f;
    static constexpr float LIVE_ROW_H   = 46.0f;
    static constexpr int   LIVE_ROWS_V  = 4;

    // Home-menu L/R tab strip.
    enum HomeMenuTab { TAB_LIBRARY = 0, TAB_CONTINUE = 1, TAB_LIVETV = 2 };

    UI(C3D_RenderTarget* top, C3D_RenderTarget* bot);
    ~UI();

    void beginFrame();
    void endFrame();

    void drawSetupScreen(const std::string& currentUrl);
    void drawLoginScreen(const std::string& serverUrl, const std::string& username);
    void drawLoadingScreen(const std::string& msg);
    void drawErrorScreen(const std::string& msg);
    void drawServerRefreshScreen(const std::vector<JellyfinScanProgress>& scans, bool complete);

    // Library home menu (Menu 1). The library grid itself lives only on the
    // touchable bottom screen (the top screen has no digitizer, and drawing
    // the same grid a second time at a different column count is what caused
    // the diagonal-selection bug); the top screen shows the Jellyfin logo
    // instead. covers is parallel to libs; a card whose covers[i].tex is null
    // falls back to a colored placeholder.
    void drawLibraryGrid(const std::vector<JellyfinLibrary>& libs,
                         const std::vector<C2D_Image>& covers,
                         int selected);

    // Continue Watching menu (Menu 2): top screen shows a detail panel for the
    // highlighted item; the touchable poster grid lives on the bottom screen.
    void drawContinueWatchingMenu(const std::vector<JellyfinItem>& resume,
                                  const std::vector<C2D_Image>& covers,
                                  int selected);

    // Live TV guide (Menu 3): top screen shows the highlighted channel's icon,
    // name, and current programme; the bottom screen lists every channel with
    // its icon and current programme, one row per channel, touchable. Only the
    // programme airing right now is shown — no schedule look-ahead.
    void drawLiveTvGuide(const std::vector<JellyfinItem>& channels,
                         const std::vector<C2D_Image>& icons,
                         int selected);

    // Tab strip shared by all three home menus ("L  Library · Continue · Live TV  R").
    void drawHomeMenuTabs(HomeMenuTab active);

    // Touch hit-testing for the bottom-screen mirror grid shared by the library,
    // item, and Continue Watching menus (see BGRID_* above). The visible page is
    // derived from `selected` (same rule drawBottomMirrorGrid uses), so this
    // always matches what's currently drawn. Returns the absolute item index
    // under (touchX, touchY), or -1 if the touch isn't over a card.
    static int hitTestBottomGrid(int touchX, int touchY, int count, int selected);

    // Touch hit-testing for the Live TV guide's channel list (see LIVE_ROW_* above).
    static int hitTestLiveList(int touchX, int touchY, int count, int selected);

    // Cover grid of the items inside a library level (movies, series, or
    // episodes). covers is parallel to items; a null tex falls back to a colored
    // placeholder. title is shown in the top bar (library or series name).
    void drawItemGrid(const std::vector<JellyfinItem>& items,
                      const std::vector<C2D_Image>& covers,
                      int selected, int offset,
                      const std::string& title);

    // Audio-track picker shown before playback (SELECT on an item). rows are the
    // track labels; the item's title goes in the top bar.
    void drawTrackScreen(const std::string& title,
                         const std::vector<std::string>& rows,
                         int selected, int offset);
    void drawSubtitleScreen(const std::string& title,
                            const std::vector<std::string>& rows,
                            int selected, int offset);

    void drawPlayerScreen(const JellyfinItem& item, const std::string& streamUrl);

private:
    C3D_RenderTarget* top_;
    C3D_RenderTarget* bot_;
    C2D_Font          font_;
    C2D_TextBuf       textBuf_;
    C2D_Image         logoImg_;   // Jellyfin logo, shown on the Library menu's top screen

    void drawText(const std::string& str, float x, float y, float scale, u32 color);
    void drawTextBuf(const std::string& str, float x, float y, float scale, u32 color,
                     float maxWidth);
    void drawRect(float x, float y, float w, float h, u32 color);
    void drawTopBar(const std::string& title);
    void drawBottomHints(const std::string& hints);
    void drawScrollList(const std::vector<std::string>& rows,
                        int selected, int offset);
    // Bottom-screen touchable card grid shared by the library, item, and
    // Continue Watching menus (geometry: BGRID_* above).
    void drawBottomMirrorGrid(const std::vector<C2D_Image>& covers,
                              const std::vector<std::string>& labels,
                              int count, int selected,
                              bool showProgress = false,
                              const std::vector<float>* progressFrac = nullptr);
    // Bottom-screen touchable channel list for the Live TV guide (LIVE_ROW_* above).
    void drawLiveTvRows(const std::vector<JellyfinItem>& channels,
                        const std::vector<C2D_Image>& icons,
                        int selected);

    static std::string formatDuration(long long ticks);
    static std::string truncate(const std::string& s, size_t maxLen);
};
