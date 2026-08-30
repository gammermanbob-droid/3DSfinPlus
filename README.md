# 3DSFin
Video playback of your entire jellyfin catalog, on your New 3ds!
3DSfin is a *work in progress* jellyfin client for New Nintendo 3DS. 

[![GitHub Release](https://img.shields.io/github/v/release/arechawla/3dsfin)](https://github.com/arechawla/3DSfin/releases) ![Downloads](https://img.shields.io/github/downloads/arechawla/3dsfin/total)

*NOTE:* .cia does **not** work as of now. Use .3dsx and launch from homebrew for working version.

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
