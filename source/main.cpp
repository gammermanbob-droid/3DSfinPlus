#include <3ds.h>
#include <citro2d.h>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include "jellyfin.h"
#include "ui.h"
#include "player.h"
#include "image.h"

// ---- App state -------------------------------------------------------------

enum AppState {
    STATE_SERVER_SETUP,
    STATE_LOGIN,
    STATE_LOADING,
    STATE_LIBRARIES,
    STATE_SERVER_REFRESH,
    STATE_ITEMS,
    STATE_TRACKS,   // audio-track picker for the item SELECT was pressed on
    STATE_SUBTITLES,
    STATE_PLAYER,
    STATE_ERROR,
};

enum PendingLoad {
    LOAD_NONE,
    LOAD_CONNECT_AND_AUTH,
    LOAD_LIBRARIES,
    LOAD_SERVER_REFRESH,
    LOAD_ITEMS,   // open a library: list its movies/series
    LOAD_DRILL,   // open a series/season: list its seasons/episodes (see drillKind)
    LOAD_TRACKS,  // fetch an item's audio tracks for the picker
    LOAD_SUBTITLES,
    LOAD_SHUFFLE, // recursively collect playable items beneath the current level
    LOAD_LIVETV,  // fetch Live TV channels + current program + icons for Menu 3
    LOAD_SEARCH,  // series search (Menu 1's search bar)
};

// The three home-level menus, cycled with L/R. Replaces the old resumeFocus
// toggle: Continue Watching is now a full menu instead of a strip you drop
// into from the library grid.
enum HomeMenu { HOME_LIBRARIES, HOME_CONTINUE, HOME_LIVETV };

// ---- Globals ---------------------------------------------------------------

static JellyfinClient client;
static u64 nextScanPoll = 0;
static AppState  state       = STATE_SERVER_SETUP;
static PendingLoad pending   = LOAD_NONE;
static std::string loadMsg;
static std::string errorMsg;

static std::vector<JellyfinLibrary> libraries;
static std::vector<C2D_Image>       libCovers;     // GPU textures, parallel to libraries
static std::vector<std::string>     libCoverData;  // raw JPEG bytes cache (survives C3D_Fini)
static int selLib  = 0, libOffset  = 0;

// One level of the library → series → episodes browse hierarchy. coverData (raw
// JPEG bytes) is cached so textures can be rebuilt after playback frees VRAM and
// when popping back to a level; only the top level keeps live GPU textures.
struct BrowseLevel {
    std::string              parentId;
    std::string              title;        // top-bar label (library/series/season name)
    ChildKind                kind;         // what this level's items are
    std::vector<JellyfinItem> items;
    std::vector<std::string>  coverData;
    std::vector<C2D_Image>    covers;
    int sel = 0, offset = 0;
};
static std::vector<BrowseLevel> browseStack;

// Continue Watching (Menu 2). A full menu now rather than a strip you drop
// into from the library grid; the visible page is derived from selResume.
static std::vector<JellyfinItem> resumeItems;
static std::vector<C2D_Image>    resumeCovers;
static std::vector<std::string>  resumeCoverData;
static int  selResume = 0;

// Home-level state: which of the three menus is showing, and shared
// edge-detect state for touch taps on the library/continue-watching/live-tv
// menus (only one of those cases runs per frame, so one flag suffices).
static HomeMenu homeMenu = HOME_LIBRARIES;
static bool     homeTouchWasHeld = false;
static bool     itemsTouchWasHeld = false;

// Live TV guide (Menu 3). Fetched once per session (or whenever the guide is
// rescanned) rather than every time the user switches into the menu.
static std::vector<JellyfinItem> liveChannels;
static std::vector<C2D_Image>    liveChannelIcons;
static std::vector<std::string>  liveChannelIconData;
static int  selLive = 0;
static bool liveLoaded = false;

// Series search (Menu 1's search bar). Results are pushed onto the normal
// browse stack so drilling into a result works exactly like any other series.
static std::string searchQuery;

static void freeLibCovers() {
    for (auto& im : libCovers) Image_free(&im);
    libCovers.clear();
}

static void freeResumeCovers() {
    for (auto& im : resumeCovers) Image_free(&im);
    resumeCovers.clear();
}

// (Re)build cover textures from the cached JPEG bytes — no network. Called after
// playback tears down and re-creates the GPU context.
static void buildCoverTextures() {
    freeLibCovers();
    libCovers.assign(libraries.size(), C2D_Image{});
    for (size_t i = 0; i < libCoverData.size() && i < libCovers.size(); i++)
        if (!libCoverData[i].empty())
            Image_loadFromMemory(
                reinterpret_cast<const unsigned char*>(libCoverData[i].data()),
                libCoverData[i].size(), &libCovers[i]);
}

// Fast/no-art mode: browsing is metadata-only. Album artwork is still fetched
// on demand by preparePlayback() for Audio items.
static void fetchLibCovers() {
    libCoverData.assign(libraries.size(), std::string());
    buildCoverTextures();
}

// (Re)build resume poster textures from cached JPEG bytes — no network.
static void buildResumeTextures() {
    freeResumeCovers();
    resumeCovers.assign(resumeItems.size(), C2D_Image{});
    for (size_t i = 0; i < resumeCoverData.size() && i < resumeCovers.size(); i++)
        if (!resumeCoverData[i].empty())
            Image_loadFromMemory(
                reinterpret_cast<const unsigned char*>(resumeCoverData[i].data()),
                resumeCoverData[i].size(), &resumeCovers[i]);
}

static void freeLiveIcons() {
    for (auto& im : liveChannelIcons) Image_free(&im);
    liveChannelIcons.clear();
}

// (Re)build Live TV channel icon textures from cached JPEG bytes — no network.
static void buildLiveIconTextures() {
    freeLiveIcons();
    liveChannelIcons.assign(liveChannelIconData.size(), C2D_Image{});
    for (size_t i = 0; i < liveChannelIconData.size(); i++)
        if (!liveChannelIconData[i].empty())
            Image_loadFromMemory(
                reinterpret_cast<const unsigned char*>(liveChannelIconData[i].data()),
                liveChannelIconData[i].size(), &liveChannelIcons[i]);
}

// Live TV channel list is small and bounded (unlike a whole library), so
// unlike fast/no-art browsing it's worth fetching every channel's icon.
static void fetchLiveIcons() {
    liveChannelIconData.assign(liveChannels.size(), std::string());
    for (size_t i = 0; i < liveChannels.size(); i++)
        liveChannelIconData[i] = client.getPrimaryImage(liveChannels[i].id, 100);
    buildLiveIconTextures();
}

// Continue Watching is now its own full menu (Menu 2, touchable poster grid),
// so unlike the rest of fast/no-art browsing it's worth fetching real posters:
// the list is capped at a dozen items by getResumeItems(), so this stays one
// bounded batch of requests rather than the N+1 a whole library would cost.
static void fetchResume() {
    resumeItems = client.getResumeItems();
    selResume   = 0;
    resumeCoverData.assign(resumeItems.size(), std::string());
    for (size_t i = 0; i < resumeItems.size(); i++)
        resumeCoverData[i] = client.getPrimaryImage(resumeItems[i].id, 150);
    buildResumeTextures();
}

// ---- Browse stack helpers --------------------------------------------------
// Invariant: only the top level (browseStack.back()) holds live GPU textures.

static void freeLevelCovers(BrowseLevel& lv) {
    for (auto& im : lv.covers) Image_free(&im);
    lv.covers.clear();
}

static void buildLevelCovers(BrowseLevel& lv) {
    freeLevelCovers(lv);
    lv.covers.assign(lv.items.size(), C2D_Image{});
    for (size_t i = 0; i < lv.coverData.size() && i < lv.covers.size(); i++)
        if (!lv.coverData[i].empty())
            Image_loadFromMemory(
                reinterpret_cast<const unsigned char*>(lv.coverData[i].data()),
                lv.coverData[i].size(), &lv.covers[i]);
}

// Network: list only the parent's children. Covers remain empty by design so a
// large season/channel list appears after one request rather than N+1 requests.
static void pushLevel(const std::string& parentId, const std::string& title,
                      ChildKind kind) {
    if (!browseStack.empty()) freeLevelCovers(browseStack.back());

    BrowseLevel lv;
    lv.parentId    = parentId;
    lv.title       = title;
    lv.kind        = kind;
    lv.items       = kind == ChildKind::LiveTvChannels
                   ? client.getLiveTvChannels()
                   : client.getChildren(parentId, kind);
    // A series with no season grouping: fall back to a flat episode list so the
    // user lands on episodes instead of an empty season grid.
    if (kind == ChildKind::Seasons && lv.items.empty()) {
        lv.kind  = ChildKind::EpisodesRecursive;
        lv.items = client.getChildren(parentId, ChildKind::EpisodesRecursive);
    }
    lv.coverData.assign(lv.items.size(), std::string());
    browseStack.push_back(std::move(lv));
    buildLevelCovers(browseStack.back());
}

// Push a level whose items are already in hand (search results) rather than
// fetched by parentId — same bookkeeping as pushLevel, minus the network call.
static void pushSearchResults(const std::string& title, std::vector<JellyfinItem> items) {
    if (!browseStack.empty()) freeLevelCovers(browseStack.back());

    BrowseLevel lv;
    lv.title = title;
    lv.kind  = ChildKind::Direct;
    lv.items = std::move(items);
    lv.coverData.assign(lv.items.size(), std::string());
    browseStack.push_back(std::move(lv));
    buildLevelCovers(browseStack.back());
}

// Pop one level; rebuild the now-top level's textures from its cached bytes.
static void popLevel() {
    if (browseStack.empty()) return;
    freeLevelCovers(browseStack.back());
    browseStack.pop_back();
    if (!browseStack.empty()) buildLevelCovers(browseStack.back());
}

static void clearBrowse() {
    if (!browseStack.empty()) freeLevelCovers(browseStack.back());
    browseStack.clear();
}

static std::string playerUrl;
// Set just before entering STATE_PLAYER. playItem can come from the item grid or
// the Continue Watching strip, so the player path is source-agnostic.
static JellyfinItem playItem;
static double       playStartSec = 0.0;
static AppState     playReturn   = STATE_ITEMS;
static AppState     settingsReturn = STATE_ITEMS;
// Audio track chosen in STATE_TRACKS, -1 = let the server pick its default. Kept
// across the seek loop in STATE_PLAYER so restarting the transcode keeps the track.
static int          playAudioIndex = -1;
static int          playSubtitleIndex = -1; // -1 = subtitles off
static std::string  playSubtitleVtt;
static std::string  playArtworkData;
static std::string  settingsItemId;
static int          settingsAudioIndex = -1;     // server default
static int          settingsSubtitleIndex = -1;  // off
static std::string  settingsSubtitleSourceId;
// Audio-track picker state (SELECT on a playable item).
static std::vector<JellyfinAudioTrack> audioTracks;
static JellyfinItem trackItem;                  // item the picker is for
static int          selTrack = 0, trackOffset = 0;
static std::vector<JellyfinSubtitleTrack> subtitleTracks;
static int          selSubtitle = 0, subtitleOffset = 0; // row 0 is Off
// Target queued for a LOAD_DRILL (set when A is pressed on a series or season).
static std::string  drillId, drillTitle;
static ChildKind    drillKind = ChildKind::Seasons;
static std::string pendingUsername;
static std::string pendingPassword;
static std::string shuffleParentId;
static std::vector<JellyfinItem> shuffleQueue;
static size_t shuffleIndex = 0;
static bool shuffleActive = false;

static void selectSettingsItem(const JellyfinItem& item) {
    if (settingsItemId == item.id) return;
    settingsItemId        = item.id;
    settingsAudioIndex    = -1;
    settingsSubtitleIndex = -1;
    settingsSubtitleSourceId.clear();
}

static bool isContainer(const JellyfinItem& item) {
    return item.type == "Series" || item.type == "Season" ||
           item.type == "MusicArtist" || item.type == "MusicAlbum" ||
           item.type == "Artist" || item.type == "Album" ||
           item.type == "Folder" || item.type == "Playlist" ||
           item.type == "BoxSet";
}

static bool isLiveChannel(const JellyfinItem& item) {
    // Jellyfin's public DTO normally says TvChannel; accept the internal model
    // name too for compatibility with other server versions.
    return item.type == "TvChannel" || item.type == "LiveTvChannel";
}

static void preparePlayback(const JellyfinItem& item, AppState returnState) {
    playItem = item;
    selectSettingsItem(playItem);
    playStartSec = item.resumeTicks / 10000000.0;
    playAudioIndex = settingsAudioIndex;
    playSubtitleIndex = item.type == "Audio" ? -1 : settingsSubtitleIndex;
    playSubtitleVtt = item.type == "Audio"
                    ? client.getLyricsVtt(item.id, item.runTimeTicks)
                    : playSubtitleIndex >= 0
                        ? client.getSubtitleVtt(playItem.id,
                                                settingsSubtitleSourceId,
                                                playSubtitleIndex)
                        : std::string();
    playArtworkData = item.type == "Audio"
                    ? client.getPrimaryImage(item.id, 400)
                    : std::string();
    playerUrl = item.type == "Audio"
              ? client.getAudioStreamUrl(item.id, item.resumeTicks)
              : isLiveChannel(item)
                  ? client.getLiveTvStreamUrl(item.id)
                  : client.getStreamUrl(item.id, item.resumeTicks,
                                        playAudioIndex, -1);
    playReturn = returnState;
    state = STATE_PLAYER;
}

static void shuffleItems(std::vector<JellyfinItem>& items) {
    if (items.size() < 2) return;
    for (size_t i = items.size() - 1; i > 0; --i) {
        size_t j = (size_t)(rand() % (i + 1));
        JellyfinItem tmp = items[i];
        items[i] = items[j];
        items[j] = tmp;
    }
}

// ---- Config ----------------------------------------------------------------

static const char* CFG_PATH = "/3ds/3dsfin/config.ini";
static std::string cfgServer;
static std::string cfgUsername;
static std::string cfgPassword;

static void saveConfig() {
    FILE* f = fopen(CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "server=%s\n",   cfgServer.c_str());
    fprintf(f, "username=%s\n", cfgUsername.c_str());
    fprintf(f, "password=%s\n", cfgPassword.c_str());
    fclose(f);
}

static void loadConfig() {
    FILE* f = fopen(CFG_PATH, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.substr(0, 7)  == "server=")   cfgServer   = s.substr(7);
        if (s.substr(0, 9)  == "username=") cfgUsername = s.substr(9);
        if (s.substr(0, 9)  == "password=") cfgPassword = s.substr(9);
    }
    fclose(f);
}

// ---- Software keyboard helpers ---------------------------------------------

static std::string swkbdRead(const char* hint, const char* initial = nullptr,
                              bool hidden = false) {
    SwkbdState swkbd;
    char buf[512] = {};
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetHintText(&swkbd, hint);
    if (initial && *initial) swkbdSetInitialText(&swkbd, initial);
    if (hidden) swkbdSetPasswordMode(&swkbd, SWKBD_PASSWORD_HIDE_DELAY);
    swkbdInputText(&swkbd, buf, sizeof(buf));
    return std::string(buf);
}

// ---- Main ------------------------------------------------------------------

int main() {
    gfxInitDefault();
    httpcInit(0x1000);
    acInit();

    // Run at the New 3DS clock (804 MHz) + L2 cache when available. Without this a
    // New 3DS runs at the Old 3DS 268 MHz, throttling the CPU blit, demux, and the
    // per-call HTTP copy — i.e. both decode smoothness and download throughput.
    // No-op on an Old 3DS, so it's always safe to call.
    osSetSpeedupEnable(true);

    // Ensure config directory exists
    mkdir("/3ds",       0777);
    mkdir("/3ds/3dsfin", 0777);
    loadConfig();
    srand((unsigned)osGetTime());

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    C3D_RenderTarget* topScreen =
        C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* botScreen =
        C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    UI ui(topScreen, botScreen);

    // Automatically authenticate with the existing saved configuration. If the
    // saved data is incomplete or no longer valid, the normal login flow remains
    // available as a fallback.
    if (!cfgServer.empty() && !cfgUsername.empty() && !cfgPassword.empty()) {
        pendingUsername = cfgUsername;
        pendingPassword = cfgPassword;
        loadMsg = "Signing in as " + cfgUsername + "...";
        pending = LOAD_CONNECT_AND_AUTH;
        state   = STATE_LOADING;
        ui.beginFrame();
        ui.drawLoadingScreen(loadMsg);
        ui.endFrame();
    } else if (!cfgServer.empty()) {
        state = STATE_LOGIN;
    }

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown & KEY_START) break;

        // ------------------------------------------------------------------
        // Phase 1: execute any pending blocking network operation.
        // The previous frame already rendered the loading screen.
        // ------------------------------------------------------------------
        if (pending != LOAD_NONE) {
            switch (pending) {
                case LOAD_CONNECT_AND_AUTH:
                    if (!client.connect(cfgServer)) {
                        errorMsg = "Cannot reach server:\n" + cfgServer +
                                   "\n\nCheck the URL and your WiFi.";
                        state = STATE_ERROR;
                    } else if (!client.authenticate(pendingUsername, pendingPassword)) {
                        char statusBuf[64];
                        int s = client.lastStatus();
                        if (s == -3)
                            snprintf(statusBuf, sizeof(statusBuf), "Response parse error");
                        else if (s < 0)
                            snprintf(statusBuf, sizeof(statusBuf), "Network error (%d)", s);
                        else
                            snprintf(statusBuf, sizeof(statusBuf), "HTTP %d", s);
                        errorMsg = std::string("Login failed: ") + statusBuf +
                                   "\n\nIf HTTP 401: wrong credentials\n"
                                   "If HTTP 400: server rejected request\n"
                                   "If -1/-2: network/SSL error";
                        state = STATE_ERROR;
                    } else {
                        cfgUsername = pendingUsername;
                        cfgPassword = pendingPassword;
                        saveConfig();
                        loadMsg = "Loading libraries...";
                        pending = LOAD_LIBRARIES;
                        // Render one loading frame before the next blocking call
                        ui.beginFrame();
                        ui.drawLoadingScreen(loadMsg);
                        ui.endFrame();
                        continue;
                    }
                    break;

                case LOAD_SERVER_REFRESH:
                    client.refreshLibrariesAndGuide();
                    nextScanPoll = osGetTime() + 2000;
                    state = STATE_SERVER_REFRESH;
                    break;

                case LOAD_LIBRARIES:
                    libraries  = client.getLibraries();
                    // Favorites is a cross-library Jellyfin user filter, exposed
                    // as a synthetic tile. Live TV now has its own dedicated
                    // menu (Menu 3) with a richer guide, so it's no longer
                    // duplicated as a library tile here.
                    libraries.push_back({"__favorites__", "Favorites", "favorites"});
                    selLib     = 0;
                    libOffset  = 0;
                    homeMenu   = HOME_LIBRARIES;
                    liveLoaded = false;   // re-fetch the guide next time Menu 3 opens
                    fetchLibCovers();   // network: cache JPEGs + build textures
                    fetchResume();      // Continue Watching list + poster art
                    state      = STATE_LIBRARIES;
                    break;

                case LOAD_LIVETV:
                    // Capped well below getLiveTvChannels()'s own 500 default:
                    // unlike the old plain-text channel list, the guide fetches
                    // one icon request per channel, so this bounds how long
                    // "Loading Live TV guide..." takes on a large lineup.
                    liveChannels = client.getLiveTvChannels(60);
                    selLive      = 0;
                    liveLoaded   = true;
                    fetchLiveIcons();   // network: cache icon JPEGs + build textures
                    state        = STATE_LIBRARIES;
                    break;

                case LOAD_SEARCH:
                    pushSearchResults("Search: " + searchQuery,
                                      client.searchSeries(searchQuery));
                    state = STATE_ITEMS;
                    break;

                case LOAD_ITEMS:
                    clearBrowse();
                    {
                    // Live TV is no longer reached this way (it's Menu 3 now),
                    // so the only synthetic tile left here is Favorites.
                    ChildKind rootKind = libraries[selLib].collectionType == "favorites"
                                       ? ChildKind::Favorites : ChildKind::Direct;
                    pushLevel(libraries[selLib].id, libraries[selLib].name,
                              rootKind);
                    }
                    state = STATE_ITEMS;
                    break;

                case LOAD_DRILL:
                    pushLevel(drillId, drillTitle, drillKind);
                    state = STATE_ITEMS;
                    break;

                case LOAD_TRACKS: {
                    audioTracks = client.getAudioTracks(trackItem.id);
                    selTrack    = 0;
                    trackOffset = 0;
                    // Row zero is "Server default"; explicit tracks start at 1.
                    if (settingsAudioIndex >= 0)
                        for (int i = 0; i < (int)audioTracks.size(); i++)
                            if (audioTracks[i].index == settingsAudioIndex) {
                                selTrack = i + 1; break;
                            }
                    state = STATE_TRACKS;
                    break;
                }

                case LOAD_SUBTITLES: {
                    subtitleTracks = client.getSubtitleTracks(trackItem.id);
                    selSubtitle    = 0;
                    subtitleOffset = 0;
                    if (settingsSubtitleIndex >= 0)
                        for (int i = 0; i < (int)subtitleTracks.size(); i++)
                            if (subtitleTracks[i].index == settingsSubtitleIndex) {
                                selSubtitle = i + 1; break;
                            }
                    state = STATE_SUBTITLES;
                    break;
                }

                case LOAD_SHUFFLE:
                    shuffleQueue = client.getChildren(shuffleParentId,
                                                       ChildKind::PlayableRecursive,
                                                       500);
                    shuffleItems(shuffleQueue);
                    shuffleIndex = 0;
                    shuffleActive = !shuffleQueue.empty();
                    if (shuffleActive)
                        preparePlayback(shuffleQueue[0], STATE_ITEMS);
                    else {
                        errorMsg = "No playable movies, episodes, or songs found.";
                        state = STATE_ERROR;
                    }
                    break;

                default: break;
            }
            pending = LOAD_NONE;
        }

        // ------------------------------------------------------------------
        // Phase 2: handle input for the current state.
        // ------------------------------------------------------------------
        switch (state) {
            case STATE_SERVER_SETUP:
                if (kDown & KEY_A) {
                    std::string url = swkbdRead(
                        "Jellyfin server URL",
                        cfgServer.empty() ? "http://192.168.1.x:8096" : cfgServer.c_str());
                    if (!url.empty()) {
                        cfgServer = url;
                        state = STATE_LOGIN;
                    }
                }
                break;

            case STATE_LOGIN:
                if (kDown & KEY_B) { state = STATE_SERVER_SETUP; break; }
                if (kDown & KEY_A) {
                    pendingUsername = swkbdRead("Username",
                        cfgUsername.empty() ? nullptr : cfgUsername.c_str());
                    if (pendingUsername.empty()) break;
                    pendingPassword = swkbdRead("Password",
                        cfgPassword.empty() ? nullptr : cfgPassword.c_str(), true);
                    if (pendingPassword.empty()) break;
                    loadMsg = "Connecting to " + cfgServer + "...";
                    pending = LOAD_CONNECT_AND_AUTH;
                    state   = STATE_LOADING;
                }
                break;

            case STATE_SERVER_REFRESH:
                if (client.scansActive() && osGetTime() >= nextScanPoll) {
                    client.pollScanProgress();
                    nextScanPoll = osGetTime() + 3000;
                }
                if (kDown & KEY_B) state = STATE_LIBRARIES;
                else if (kDown & KEY_A) {
                    loadMsg = "Reloading libraries...";
                    pending = LOAD_LIBRARIES;
                    state = STATE_LOADING;
                }
                break;

            case STATE_LIBRARIES: {
                // L/R cycle the three home menus. A menu switch is a single
                // discrete action (not something you'd want to auto-repeat),
                // so kDown (press edge) rather than kHeld.
                if (kDown & (KEY_L | KEY_R)) {
                    int dir = (kDown & KEY_R) ? 1 : 2;   // +1 or -1 (mod 3)
                    homeMenu = (HomeMenu)(((int)homeMenu + dir) % 3);
                    if (homeMenu == HOME_CONTINUE) {
                        fetchResume();   // pick up any progress made elsewhere
                    } else if (homeMenu == HOME_LIVETV && !liveLoaded) {
                        loadMsg = "Loading Live TV guide...";
                        pending = LOAD_LIVETV;
                        state   = STATE_LOADING;
                    }
                    break;
                }

                // Touch: edge-detected tap (KEY_TOUCH is a held state, so a
                // prev-frame flag turns it into a single "just tapped" event).
                // Tapping only moves the cursor, the same as a D-Pad/circle-pad
                // press would — A is still what opens/plays/confirms.
                bool touching = (hidKeysHeld() & KEY_TOUCH) != 0;
                int  touchHit = -1;
                bool touchSearch = false;
                if (touching && !homeTouchWasHeld) {
                    touchPosition touch; hidTouchRead(&touch);
                    if (homeMenu == HOME_LIBRARIES && touch.px >= UI::BOT_W - 72 && touch.py < 24) {
                        touchSearch = true;
                    } else if (homeMenu == HOME_LIBRARIES) {
                        touchHit = UI::hitTestBottomGrid(touch.px, touch.py,
                                                          (int)libraries.size(), selLib);
                    } else if (homeMenu == HOME_CONTINUE) {
                        touchHit = UI::hitTestBottomGrid(touch.px, touch.py,
                                                          (int)resumeItems.size(), selResume);
                    } else {
                        touchHit = UI::hitTestLiveList(touch.px, touch.py,
                                                        (int)liveChannels.size(), selLive);
                    }
                }
                homeTouchWasHeld = touching;

                if (homeMenu == HOME_LIBRARIES) {
                    if (kDown & KEY_X) {
                        if (client.scansActive()) {
                            state = STATE_SERVER_REFRESH;
                            nextScanPoll = 0;
                            break;
                        }
                        loadMsg = "Starting library and guide scans...";
                        pending = LOAD_SERVER_REFRESH;
                        state = STATE_LOADING;
                        break;
                    }
                    if (touchSearch || (kDown & KEY_Y)) {
                        std::string q = swkbdRead("Search series");
                        if (!q.empty()) {
                            searchQuery = q;
                            loadMsg = "Searching \"" + q + "\"...";
                            pending = LOAD_SEARCH;
                            state   = STATE_LOADING;
                        }
                        break;
                    }

                    int n    = (int)libraries.size();
                    int cols = UI::GRID_COLS;
                    if (touchHit >= 0) selLib = touchHit;
                    if (kDown & KEY_RIGHT && selLib < n - 1 && (selLib % cols) != cols - 1) selLib++;
                    if (kDown & KEY_LEFT  && (selLib % cols) != 0)                          selLib--;
                    if (kDown & KEY_DOWN  && selLib + cols < n)                             selLib += cols;
                    if (kDown & KEY_UP    && selLib - cols >= 0)                            selLib -= cols;
                    // Scroll so the selected row stays visible (libOffset is in rows).
                    int selRow = selLib / cols;
                    if (selRow < libOffset) libOffset = selRow;
                    if (selRow >= libOffset + UI::GRID_ROWS_VISIBLE)
                        libOffset = selRow - UI::GRID_ROWS_VISIBLE + 1;

                    if (kDown & KEY_A && !libraries.empty()) {
                        loadMsg = "Loading \"" + libraries[selLib].name + "\"...";
                        pending = LOAD_ITEMS;
                        state   = STATE_LOADING;
                    }
                } else if (homeMenu == HOME_CONTINUE) {
                    int rn   = (int)resumeItems.size();
                    int cols = UI::BGRID_COLS;
                    if (touchHit >= 0) selResume = touchHit;
                    if (kDown & KEY_RIGHT && selResume < rn - 1)   selResume++;
                    if (kDown & KEY_LEFT  && selResume > 0)        selResume--;
                    if (kDown & KEY_DOWN  && selResume + cols < rn) selResume += cols;
                    if (kDown & KEY_UP    && selResume - cols >= 0) selResume -= cols;
                    if (kDown & KEY_A && rn > 0) {
                        shuffleActive = false;
                        preparePlayback(resumeItems[selResume], STATE_LIBRARIES);
                    }
                    if (kDown & KEY_Y && rn > 0) {
                        trackItem = resumeItems[selResume];
                        selectSettingsItem(trackItem);
                        settingsReturn = STATE_LIBRARIES;
                        loadMsg   = "Loading subtitles...";
                        pending   = LOAD_SUBTITLES;
                        state     = STATE_LOADING;
                    }
                    if (kDown & KEY_SELECT && rn > 0) {
                        trackItem = resumeItems[selResume];
                        selectSettingsItem(trackItem);
                        settingsReturn = STATE_LIBRARIES;
                        loadMsg   = "Loading audio tracks...";
                        pending   = LOAD_TRACKS;
                        state     = STATE_LOADING;
                    }
                } else { // HOME_LIVETV
                    int n = (int)liveChannels.size();
                    if (touchHit >= 0) selLive = touchHit;
                    if (kDown & KEY_DOWN && selLive < n - 1) selLive++;
                    if (kDown & KEY_UP   && selLive > 0)     selLive--;
                    if (kDown & KEY_A && n > 0) {
                        shuffleActive = false;
                        preparePlayback(liveChannels[selLive], STATE_LIBRARIES);
                    }
                }
                break;
            }

            case STATE_ITEMS: {
                if (kDown & KEY_B) {
                    popLevel();
                    if (browseStack.empty()) state = STATE_LIBRARIES;
                    break;
                }

                BrowseLevel& lv = browseStack.back();
                int n    = (int)lv.items.size();
                int cols = UI::ITEM_GRID_COLS;

                // Touch: tap a card in the bottom-screen mirror grid to select it
                // (see STATE_LIBRARIES above for the same edge-detected pattern).
                bool touching = (hidKeysHeld() & KEY_TOUCH) != 0;
                if (touching && !itemsTouchWasHeld) {
                    touchPosition touch; hidTouchRead(&touch);
                    int hit = UI::hitTestBottomGrid(touch.px, touch.py, n, lv.sel);
                    if (hit >= 0) lv.sel = hit;
                }
                itemsTouchWasHeld = touching;

                if (kDown & KEY_RIGHT && lv.sel < n - 1 && (lv.sel % cols) != cols - 1) lv.sel++;
                if (kDown & KEY_LEFT  && (lv.sel % cols) != 0)                          lv.sel--;
                if (kDown & KEY_DOWN  && lv.sel + cols < n)                             lv.sel += cols;
                if (kDown & KEY_UP    && lv.sel - cols >= 0)                            lv.sel -= cols;
                // Keep the selected row within the visible window (offset in rows).
                int selRow = lv.sel / cols;
                if (selRow < lv.offset) lv.offset = selRow;
                if (selRow >= lv.offset + UI::ITEM_GRID_ROWS_VISIBLE)
                    lv.offset = selRow - UI::ITEM_GRID_ROWS_VISIBLE + 1;

                if (kDown & KEY_A && n > 0) {
                    JellyfinItem& it = lv.items[lv.sel];
                    if (it.type == "Series") {
                        // Drill into the series to list its seasons.
                        drillId    = it.id;
                        drillTitle = it.name;
                        drillKind  = ChildKind::Seasons;
                        loadMsg    = "Loading \"" + it.name + "\"...";
                        pending    = LOAD_DRILL;
                        state      = STATE_LOADING;
                    } else if (it.type == "Season") {
                        // Drill into the season to list its episodes.
                        drillId    = it.id;
                        drillTitle = lv.title + " - " + it.name;
                        drillKind  = ChildKind::Episodes;
                        loadMsg    = "Loading \"" + it.name + "\"...";
                        pending    = LOAD_DRILL;
                        state      = STATE_LOADING;
                    } else if (isContainer(it)) {
                        drillId    = it.id;
                        drillTitle = lv.title + " - " + it.name;
                        drillKind  = ChildKind::Direct;
                        loadMsg    = "Loading \"" + it.name + "\"...";
                        pending    = LOAD_DRILL;
                        state      = STATE_LOADING;
                    } else {
                        shuffleActive = false;
                        preparePlayback(it, STATE_ITEMS);
                    }
                }

                // X starts a shuffled queue containing every playable item under
                // this album, artist, season, series, folder, or library level.
                if (kDown & KEY_X && n > 0 && !isLiveChannel(lv.items[lv.sel])) {
                    shuffleParentId = lv.parentId;
                    loadMsg = "Building shuffle queue...";
                    pending = LOAD_SHUFFLE;
                    state = STATE_LOADING;
                }

                // SELECT on a playable item opens the audio-track picker. Series
                // and seasons have no streams of their own, so it does nothing there.
                if (kDown & KEY_SELECT && n > 0) {
                    JellyfinItem& it = lv.items[lv.sel];
                    if (!isContainer(it) && it.type != "Audio") {
                        trackItem = it;
                        selectSettingsItem(trackItem);
                        settingsReturn = STATE_ITEMS;
                        loadMsg   = "Loading audio tracks...";
                        pending   = LOAD_TRACKS;
                        state     = STATE_LOADING;
                    }
                }
                if (kDown & KEY_Y && n > 0) {
                    JellyfinItem& it = lv.items[lv.sel];
                    if (!isContainer(it) && it.type != "Audio") {
                        trackItem = it;
                        selectSettingsItem(trackItem);
                        settingsReturn = STATE_ITEMS;
                        loadMsg   = "Loading subtitles...";
                        pending   = LOAD_SUBTITLES;
                        state     = STATE_LOADING;
                    }
                }
                break;
            }

            case STATE_TRACKS: {
                if (kDown & KEY_B) { state = settingsReturn; break; }

                int n = (int)audioTracks.size() + 1;
                if (kDown & KEY_DOWN && selTrack < n - 1) selTrack++;
                if (kDown & KEY_UP   && selTrack > 0)     selTrack--;
                if (selTrack < trackOffset) trackOffset = selTrack;
                if (selTrack >= trackOffset + UI::VISIBLE_ROWS)
                    trackOffset = selTrack - UI::VISIBLE_ROWS + 1;

                if (kDown & KEY_A) {
                    settingsAudioIndex = selTrack == 0
                                       ? -1 : audioTracks[selTrack - 1].index;
                    state = settingsReturn;
                }
                break;
            }


            case STATE_SUBTITLES: {
                if (kDown & KEY_B) { state = settingsReturn; break; }
                int n = (int)subtitleTracks.size() + 1; // Off + streams
                if (kDown & KEY_DOWN && selSubtitle < n - 1) selSubtitle++;
                if (kDown & KEY_UP   && selSubtitle > 0)     selSubtitle--;
                if (selSubtitle < subtitleOffset) subtitleOffset = selSubtitle;
                if (selSubtitle >= subtitleOffset + UI::VISIBLE_ROWS)
                    subtitleOffset = selSubtitle - UI::VISIBLE_ROWS + 1;
                if (kDown & KEY_A) {
                    settingsSubtitleIndex = selSubtitle == 0
                                          ? -1 : subtitleTracks[selSubtitle - 1].index;
                    settingsSubtitleSourceId = selSubtitle == 0
                                             ? std::string()
                                             : subtitleTracks[selSubtitle - 1].mediaSourceId;
                    state = settingsReturn;
                }
                break;
            }

            case STATE_PLAYER: {
                freeLibCovers();      // textures live in VRAM that playback tears down
                freeResumeCovers();
                freeLiveIcons();
                if (!browseStack.empty()) freeLevelCovers(browseStack.back());
                // Azahar retains a render target's screen-output binding after
                // C3D_Fini. Detach explicitly so playback can own both screens.
                C3D_RenderTargetDetachOutput(topScreen);
                C3D_RenderTargetDetachOutput(botScreen);
                C2D_Fini();
                C3D_Fini();

                // Seeking restarts the transcode at the target time: playerPlay
                // reports the target via seekTo and we call straight back in with
                // a fresh stream URL (same mechanism as resume).
                bool playAnother;
                do {
                    playAnother = false;
                    double seekTo;
                    bool finished = false;
                    do {
                        seekTo = -1.0;
                        playerPlay(playerUrl, playItem.runTimeTicks,
                               playItem.seriesName,
                               playItem.name,
                               playItem.productionYear,
                                   playStartSec, &seekTo, playSubtitleVtt,
                                   &finished, playArtworkData,
                                   playItem.type == "Audio");
                        client.stopTranscode();
                        if (seekTo >= 0) {
                            playStartSec = seekTo;
                            long long ticks = (long long)(seekTo * 10000000.0);
                            playerUrl = playItem.type == "Audio"
                                      ? client.getAudioStreamUrl(playItem.id, ticks)
                                      : client.getStreamUrl(playItem.id, ticks,
                                                            playAudioIndex, -1);
                        }
                    } while (seekTo >= 0);

                    if (finished && shuffleActive &&
                        shuffleIndex + 1 < shuffleQueue.size()) {
                        ++shuffleIndex;
                        preparePlayback(shuffleQueue[shuffleIndex], playReturn);
                        playAnother = true;
                    } else {
                        shuffleActive = false;
                    }
                } while (playAnother);

                // playerPlay's consoleInit(GFX_BOTTOM) switched that screen to
                // RGB565 + single-buffered and libctru never restores it, so
                // citro2d would transfer RGB8 pixels into a framebuffer GSP
                // still reads as 16-bit — the garbled bottom screen after
                // playback. Put back what gfxInitDefault set before C2D takes
                // the screen again (this reallocates the framebuffers, so it
                // must happen before C2D_CreateScreenTarget grabs pointers).
                gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
                gfxSetDoubleBuffering(GFX_BOTTOM, true);

                C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
                C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
                C2D_Prepare();
                topScreen = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
                botScreen = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
                ui.~UI();
                new (&ui) UI(topScreen, botScreen);
                buildCoverTextures();    // rebuild from cached JPEG bytes (no re-fetch)
                buildResumeTextures();
                buildLiveIconTextures();
                if (!browseStack.empty()) buildLevelCovers(browseStack.back());
                state = playReturn;
                break;
            }

            case STATE_ERROR:
                if (kDown & KEY_A || kDown & KEY_B) {
                    errorMsg.clear();
                    state = STATE_LOGIN;
                }
                break;

            case STATE_LOADING:
                break;
        }

        // ------------------------------------------------------------------
        // Phase 3: render the current state.
        // ------------------------------------------------------------------
        ui.beginFrame();

        switch (state) {
            case STATE_SERVER_SETUP:
                ui.drawSetupScreen(cfgServer);
                break;
            case STATE_LOGIN:
                ui.drawLoginScreen(cfgServer, cfgUsername);
                break;
            case STATE_LOADING:
                ui.drawLoadingScreen(loadMsg);
                break;
            case STATE_SERVER_REFRESH:
                ui.drawServerRefreshScreen(client.scanProgress(), client.scansCompleted());
                break;
            case STATE_LIBRARIES:
                if (homeMenu == HOME_LIBRARIES)
                    ui.drawLibraryGrid(libraries, libCovers, selLib, libOffset);
                else if (homeMenu == HOME_CONTINUE)
                    ui.drawContinueWatchingMenu(resumeItems, resumeCovers, selResume);
                else
                    ui.drawLiveTvGuide(liveChannels, liveChannelIcons, selLive);
                break;
            case STATE_ITEMS: {
                BrowseLevel& lv = browseStack.back();
                ui.drawItemGrid(lv.items, lv.covers, lv.sel, lv.offset, lv.title);
                break;
            }
            case STATE_TRACKS: {
                std::vector<std::string> rows;
                rows.push_back("Server default");
                for (const auto& t : audioTracks) {
                    // Jellyfin's DisplayTitle usually ends in "- Default" already;
                    // only add our own marker when it doesn't say so.
                    bool marked = t.title.find("Default") != std::string::npos;
                    rows.push_back(t.isDefault && !marked ? t.title + "   (default)"
                                                          : t.title);
                }
                ui.drawTrackScreen(trackItem.name, rows, selTrack, trackOffset);
                break;
            }
            case STATE_SUBTITLES: {
                std::vector<std::string> rows;
                rows.push_back("Off");
                for (const auto& t : subtitleTracks) {
                    std::string row = t.title;
                    if (t.isForced) row += " [Forced]";
                    else if (t.isDefault) row += " [Default]";
                    rows.push_back(row);
                }
                ui.drawSubtitleScreen(trackItem.name, rows,
                                      selSubtitle, subtitleOffset);
                break;
            }
            case STATE_PLAYER:
                ui.drawPlayerScreen(playItem, playerUrl);
                break;
            case STATE_ERROR:
                ui.drawErrorScreen(errorMsg);
                break;
        }

        ui.endFrame();
    }

    freeLibCovers();
    freeResumeCovers();
    freeLiveIcons();
    clearBrowse();
    C2D_Fini();
    C3D_Fini();
    acExit();
    httpcExit();
    gfxExit();
    return 0;
}
