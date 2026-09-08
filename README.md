# 3DSFin
Video playback of your entire jellyfin catalog, on your New 3ds!

3DSfinPlus also browses Jellyfin music libraries (artist, album, and song),
plays server-transcoded AAC audio, and supports shuffled continuous playback.
Music playback uses a bottom-screen HUD with play/pause, seeking, progress, and
album artwork on the top screen.
### Scan libraries and guide data

On the library home screen, press **X** to start Jellyfin's library scan and
Live TV guide refresh. The signed-in Jellyfin account needs administrator
permission. The result screen reports each task as started, already running,
or failed; accepted scans continue on the server in the background.
The scan menu displays separate library and guide progress bars, updating every three seconds while open. A green completion message appears only after both scans succeed. Press A to reload libraries, or B to leave scans running; X returns to their progress while they are active. Missing percentages, connection loss, failures, and cancellations are shown without claiming completion.
This refreshes server guide data; it does not add programme-guide browsing.

### Home menus: Library, Continue Watching, Live TV Guide

The home screen is three menus, switched with the **L**/**R** shoulder
buttons: **Library** (your Jellyfin libraries, plus a Favorites tile),
**Continue Watching** (a full touchable poster grid, promoted out of the old
bottom-screen strip), and **Live TV Guide**. Circle Pad and the touch screen
both move the selection the same way D-Pad presses do; **A** opens, plays, or
watches whatever is highlighted. On the Library menu, press **Y** or tap
**Search** (bottom-right) to search series by title.

### Live TV guide

Live TV channels appear in their own **Live TV Guide** menu (Menu 3, via
L/R) rather than as a library tile: the bottom screen lists every channel
with its icon and the programme airing *right now*; the top screen shows the
highlighted channel larger. There is intentionally no schedule look-ahead —
only what's currently on. Live TV compatibility is experimental and is not
guaranteed: results can vary with the provider, tuner, proxy, codec, and
Jellyfin transcoding configuration.

For faster startup, browsing a library's movies/series/episodes stays a
metadata-only layout without cover art (music album art still loads when a
song enters the player). Continue Watching posters and Live TV channel icons
are the exception: both lists are small and bounded, so their artwork loads
up front.
3DSfin is a *work in progress* jellyfin client for New Nintendo 3DS.

[![GitHub Release](https://img.shields.io/github/v/release/arechawla/3dsfin)](https://github.com/arechawla/3DSfin/releases) ![Downloads](https://img.shields.io/github/downloads/arechawla/3dsfin/total)

The release includes both Homebrew Launcher (`.3dsx`) and installable
`3dsfinplus.cia` builds.

## Install 


 ### 1. Manual Install
 Donwload from releases tab and place onto 3ds sd card, launch from homebrew 

 ### 3. Build From Source:

 #### Prerequisites
  - [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `3ds-dev` group installed
  - [MakeRom](https://github.com/3DSGuy/Project_CTR/releases/tag/makerom-v0.18.3) installed in `~\devkitPro\tools\bin` for .cia build

  #### Build

  **Linux / macOS**
  ```bash
  export DEVKITPRO=/opt/devkitpro
  export DEVKITARM=/opt/devkitpro/devkitARM
  export PATH=$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH
  make
```

  **Windows (PowerShell)**
  ```
  $env:DEVKITPRO = "C:/devkitPro"
  $env:DEVKITARM = "C:/devkitPro/devkitARM"
  $env:PATH = "C:\devkitPro\msys2\usr\bin;C:\devkitPro\devkitARM\bin;C:\devkitPro\tools\bin;$env:PATH"
  C:\devkitPro\msys2\usr\bin\make.exe
  ```

Output: `3dsfin.3dsx, 3dsfin.cia`


## Setup
Login with server IP and user credentials:
![Screenshot](3dsfinlogindualscreen.jpg)
Connection will start:
![Screenshot](connecting.jpg)
Full Library Catalog can be browsed:
![Screenshot](mainscreennew.jpg)
Browsing Within Library:
![Screenshot](librarybrowse.jpg)
Playback:
![Screenshot](playback.jpg)

Press **X** in any item grid to shuffle all playable movies, episodes, or songs
under the current library, series, season, artist, album, or folder. Songs advance
automatically until the shuffled queue ends; press **B** to stop and return.

During playback the bottom-screen HUD is touchable: tap the seek bar to jump
to that point, or tap the play/pause, rewind (-10s), fast-forward (+30s), or
back icons — the same layout the on-screen labels already describe, just also
reachable by touch.

### Lyrics (WIP)

3DSfinPlus can request Jellyfin lyrics for music and display lyric text in the
bottom-screen HUD. This compatibility is **work in progress**: the initial lyric
line displays, but synchronized lines do not yet advance during playback. Video
subtitle playback is unaffected.



## Current Status
3DSfin is in very early (alpha) development. Check out release notes for versions in releases tab for the latest updates.

### Emulator HTTPS compatibility

This build supports HTTPS and reverse-proxy endpoints such as ngrok when 3DSfin
runs under an Azahar-derived emulator on Windows, Android, macOS, or Linux. It:

- configures HTTPS for emulator use;
- follows up to five HTTP redirects;
- sends `ngrok-skip-browser-warning` for API and video requests; and
- paces new HTTPS connections below ngrok Free's per-minute limit; and
- records the failing HTTP stage and native result code in `player_debug.txt`.

## Subtitles

Press **Y** on a movie or episode to open the subtitle picker. Choose **Off** or
one of the item's subtitle tracks, then press **A** to save the choice and return.
Press **SELECT** to choose audio the same way. Playback starts only when **A** is
pressed on the movie or episode, using both saved choices. Jellyfin burns the
selected subtitle track into the 400x240 transcode, so embedded, external, styled, and
image-based subtitles do not need to be decoded by the 3DS. The selection is
kept when seeking. Subtitle transcoding can take longer to start and uses more
server CPU than playback with subtitles off.

Subtitle size is controlled by Jellyfin/FFmpeg (and by embedded ASS/SSA styling),
not by the 3DS player. Jellyfin has no reliable per-playback font-size query that
works across text, styled, and image subtitle formats.

### Experimental bottom-screen subtitle build

The separately packaged `3dsfin-bottom-subs.3dsx` requests the selected subtitle
track as WebVTT instead of burning it into the video. It displays up to three
centered lines in the upper portion of the bottom screen and moves status,
metadata, seeking, and controls downward. This gives subtitles a consistent,
readable native-console size and reduces server transcoding work. It is intended
for text subtitle tracks; styled ASS/SSA formatting and image subtitles cannot be
preserved by the WebVTT renderer.

Certificate verification is disabled for emulator compatibility. Only connect
to a private endpoint you control. This mode is not intended for real 3DS
hardware or untrusted networks.


## Credits
- thanks to [FourthTube](https://github.com/erievs/FourthTube), for figuring out streaming logic and audio sync issues
