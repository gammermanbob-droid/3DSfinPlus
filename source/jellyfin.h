#pragma once
#include "http.h"
#include <string>
#include <vector>

struct JellyfinScanProgress {
    std::string id, previousEnd, message;
    float percent = -1;
    bool active = false, completed = false;
};

struct JellyfinLibrary {
    std::string id;
    std::string name;
    std::string collectionType; // "movies", "tvshows", "music", etc.
};

struct JellyfinItem {
    std::string id;
    std::string name;           // movie title / episode name / season name
    std::string type;           // "Movie", "Episode", "Series", "Season"
    std::string seriesName;     // parent series (episodes only)
    long long   runTimeTicks;   // 10,000,000 ticks per second
    int         productionYear;
    long long   resumeTicks;    // saved playback position (0 = start), from UserData
    std::string currentProgram; // Live TV only: CurrentProgram.Name, empty otherwise
};

// One selectable audio track of an item (anime typically ships jpn + eng).
struct JellyfinAudioTrack {
    int         index;      // absolute stream index — the AudioStreamIndex to request
    std::string title;      // DisplayTitle, e.g. "Japanese - Opus 2.0 - Stereo"
    std::string language;   // 3-letter code ("jpn"), empty if untagged
    bool        isDefault;  // the track the server would pick on its own
};

// One selectable subtitle stream. Subtitles are burned into the server-side
// transcode so text, ASS/SSA styling, and image subtitles all work on the 3DS.
struct JellyfinSubtitleTrack {
    int         index;
    std::string mediaSourceId;
    std::string title;
    std::string language;
    bool        isDefault;
    bool        isForced;
};

// Which children to enumerate beneath a parent when browsing.
enum class ChildKind {
    Direct,             // a library's direct children: Movies or Series, by name
    Favorites,          // signed-in user's favorited media across all libraries
    LiveTvChannels,     // enabled Live TV channels (no programme guide)
    Seasons,            // a series' seasons, ordered by season number
    Episodes,           // a season's episodes, ordered by episode number
    EpisodesRecursive,  // every episode beneath a series, flattened (seasonless fallback)
    PlayableRecursive,  // every movie, episode, or song beneath a folder/library
};

class JellyfinClient {
public:
    JellyfinClient();

    // Returns false if the server is unreachable
    bool connect(const std::string& serverUrl);

    // Returns false on bad credentials
    bool authenticate(const std::string& username, const std::string& password);

    std::vector<JellyfinLibrary> getLibraries();

    // Start both server tasks with the signed-in user's permissions.
    // Reports accepted/running tasks separately from errors, not completion.
    std::string refreshLibrariesAndGuide();
    void pollScanProgress();
    const std::vector<JellyfinScanProgress>& scanProgress() const { return scans_; }
    bool scansActive() const;
    bool scansCompleted() const;

    // Lists a parent's children according to kind: a library's movies/series,
    // a series' seasons, a season's episodes, or (fallback) every episode beneath
    // a series flattened in season/episode order. limit caps the page size.
    std::vector<JellyfinItem> getChildren(const std::string& parentId,
                                          ChildKind kind  = ChildKind::Direct,
                                          int       limit = 200);

    // In-progress items across all libraries ("Continue Watching"), newest first.
    // Each item's resumeTicks holds the saved playback position.
    std::vector<JellyfinItem> getResumeItems(int limit = 12);

    // Lists channels from Jellyfin Live TV, including each channel's currently
    // airing programme name (item.currentProgram) for the Live TV guide menu.
    // No look-ahead: only the program airing right now is requested.
    std::vector<JellyfinItem> getLiveTvChannels(int limit = 500);

    // Series matching a free-text search term (title contains/fuzzy match, same
    // as Jellyfin's own search). Used by the home menu's series search bar.
    // Movies, episodes, and other item types are deliberately excluded.
    std::vector<JellyfinItem> searchSeries(const std::string& query, int limit = 50);

    // The item's audio tracks, in stream order. Empty if the item has no audio or
    // the lookup failed. Used to offer a track picker before starting the stream.
    std::vector<JellyfinAudioTrack> getAudioTracks(const std::string& itemId);

    // The item's embedded/external subtitle streams, in stream order.
    std::vector<JellyfinSubtitleTrack> getSubtitleTracks(const std::string& itemId);

    // Requests a subtitle as WebVTT for the experimental bottom-screen renderer.
    std::string getSubtitleVtt(const std::string& itemId,
                               const std::string& mediaSourceId,
                               int streamIndex);

    // Returns a direct-stream URL pre-configured for 3DS capabilities.
    // startTicks seeks the transcode to a resume position (0 = from the start).
    // audioStreamIndex picks a specific audio track (-1 = let the server choose).
    // Each call embeds a fresh PlaySessionId: without one, Jellyfin matches the
    // request to the still-running transcode of the previous stream and ignores
    // the new StartTimeTicks — which made seeking a no-op.
    std::string getStreamUrl(const std::string& itemId,
                             long long startTicks = 0,
                             int       audioStreamIndex = -1,
                             int       subtitleStreamIndex = -1);

    // Live channels must be opened through PlaybackInfo before requesting HLS;
    // unlike files, they have no finite runtime from which main.m3u8 can be made.
    std::string getLiveTvStreamUrl(const std::string& channelId);

    // Audio-only HLS/AAC stream for Jellyfin music items. It uses the same TS
    // demux and Helix AAC decoder as video playback, without requiring H.264.
    std::string getAudioStreamUrl(const std::string& itemId,
                                  long long startTicks = 0);

    // Fetch Jellyfin's synced/unsynced lyrics and convert them to WebVTT so the
    // bottom-screen subtitle renderer can display them during music playback.
    std::string getLyricsVtt(const std::string& itemId,
                             long long runTimeTicks = 0);

    // Tells the server to kill the transcode job of the last getStreamUrl()
    // stream. Call between seeks (and after playback) so orphaned ffmpeg jobs
    // don't pile up server-side. Safe to call when nothing is active.
    void stopTranscode();

    // Fetches the raw Primary-image bytes (JPEG) for an item/library, scaled to
    // fillWidth px. Returns an empty string if the item has no image or on error.
    std::string getPrimaryImage(const std::string& itemId, int fillWidth);

    bool        isAuthenticated() const { return !accessToken_.empty(); }
    std::string serverUrl()       const { return serverUrl_; }
    int         lastStatus()      const { return lastStatus_; }

private:
    std::vector<JellyfinScanProgress> scans_;
    int         lastStatus_ = 0;
    std::string serverUrl_;
    std::string userId_;
    std::string accessToken_;
    std::string deviceId_;
    std::string lastPlaySessionId_;   // session of the last getStreamUrl() call
    HttpClient  http_;

    void applyAuthHeader();
};
