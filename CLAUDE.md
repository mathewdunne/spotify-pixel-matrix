# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

ESP32 firmware that polls a custom now-playing HTTP proxy and displays the currently playing Spotify track (with album art) on either a "Cheap Yellow Display" (CYD, ESP32-2432S028R, 320x240 ILI9341) or a 64x64 HUB75 LED matrix panel driven by an ESP32 Trinity. Optional PN532 NFC reader lets a tag trigger playback of a Spotify URI.

**Networking model (current):** WiFi credentials and the now-playing API URL are **hardcoded** in `SpotifyDiyThing/config.h` (gitignored). The device connects to WiFi directly and polls the proxy endpoint (default `https://spotify.mathewdunne.ca/`) for now-playing JSON; it no longer talks to the Spotify Web API or stores any OAuth token. This replaced the original WiFiManager captive-portal + on-device OAuth refresh-token flow, which Spotify broke by expiring refresh tokens after ~6 months. The old flow's code (`WifiManagerHandler.h`, `refreshToken.h`, `configFile.h`, and most of `spotifyLogic.h`) is **left in place but no longer called** — easy to revert, but bypassed.

## Build / flash (PlatformIO)

`platformio.ini` sets `src_dir = SpotifyDiyThing` and `default_envs = trinity`. Environments:

- `env:cyd` — Cheap Yellow Display (TFT_eSPI, `-DYELLOW_DISPLAY`, `-DTFT_INVERSION_OFF`)
- `env:cyd2usb` — CYD variant with two USB ports (same as `cyd` but `-DTFT_INVERSION_ON`)
- `env:trinity` — ESP32 Trinity / HUB75 matrix (`-DMATRIX_DISPLAY`); uploads OTA via `upload_protocol = espota`, `upload_port = spotifyscreen.local`

Note: at the time of writing, only `[env:trinity]` is uncommented in `platformio.ini`. The `cyd` / `cyd2usb` blocks are present but commented out — uncomment them (and the shared `[common_cyd]` block) before building for CYD hardware.

Common commands:

```
pio run -e trinity                 # build
pio run -e trinity -t upload       # upload (OTA for trinity, serial for cyd)
pio device monitor                 # 115200 baud, esp32_exception_decoder filter
```

**Before building:** copy `SpotifyDiyThing/config.example.h` → `SpotifyDiyThing/config.h` and fill in your WiFi credentials + now-playing API URL. `config.h` is gitignored and required to compile (see **Now-playing API & config** below).

## Build / flash (Arduino IDE)

The display selection is controlled by `#define YELLOW_DISPLAY` / `#define MATRIX_DISPLAY` at the top of `SpotifyDiyThing/SpotifyDiyThing.ino` (defaults to `YELLOW_DISPLAY` if neither is set). PlatformIO sets these via `build_flags`, so do not also uncomment them in the `.ino` when building with PlatformIO — pick one mechanism.

For Arduino IDE CYD builds, the `TFT_eSPI` library must be configured by replacing its bundled `User_Setup.h` with `DisplayConfig/User_Setup.h` from this repo. PlatformIO bypasses this by passing the same defines as `build_flags`.

## Architecture

Single-sketch Arduino project; all source lives in `SpotifyDiyThing/` as the `.ino` plus header-only modules (each `.h` contains both declarations and definitions — there are no `.cpp` files except the touchscreen driver).

Entry point `SpotifyDiyThing.ino` wires everything together (current flow):

1. `spotifyDisplay->displaySetup(&spotify)` — display brought up first. The `spotify` object is kept **only** so the display can call `getImage()` to download album art; no Spotify access token is set (and none is needed — `getImage` makes an unauthenticated GET).
2. SPIFFS mount — used to buffer the downloaded album art at `/album.jpg`.
3. `connectWiFi()` (in the `.ino`) — `WiFi.begin(WIFI_SSID, WIFI_PASSWORD)` using the hardcoded values from `config.h`, with a 30s timeout that restarts on failure. Sets hostname `spotifyscreen`.
4. `nowPlayingSetup` (`nowPlaying.h`) — stores the display pointer and poll timing.
5. `ArduinoOTA` is started with hostname `spotifyscreen` — this is also why the trinity env uploads to `spotifyscreen.local`.

In `loop()`, `updateNowPlaying` (`nowPlaying.h`) polls `NOWPLAYING_API_URL` (every ~20s — the proxy caches ~20s server-side), parses the JSON with ArduinoJson, and on a playing track drives the display's existing album-art download/decode path. See the **Now-playing API** section below.

The old setup steps — `DoubleResetDetector`, `fetchConfigFile`, `setupWiFiManager`, `launchRefreshTokenFlow`, `spotifySetup`/`spotifyRefreshToken` — are no longer called from the `.ino`, but the functions still exist in their headers.

### Display abstraction

`spotifyDisplay.h` defines an abstract `SpotifyDisplay` base class. The `.ino` picks one concrete implementation at compile time via the display defines:

- `cheapYellowLCD.h` → `CheapYellowDisplay` (uses TFT_eSPI; depends on the `User_Setup.h` defines listed in `[common_cyd] build_flags`)
- `matrixDisplay.h` → `MatrixDisplay` (uses ESP32-HUB75-MatrixPanel-I2S-DMA + Adafruit_GFX)

Both `nowPlaying.h` (current) and `spotifyLogic.h` (legacy) only talk to the base class pointer `spotifyDisplay`, so adding a new display means subclassing `SpotifyDisplay` and adding a new `#elif defined MY_DISPLAY` block in the `.ino`. The base class also stores screen/image dimensions and the last album-art URL (used to skip re-downloading when the album hasn't changed — see `isSameAlbum`).

### Now-playing API & config (`nowPlaying.h`, `config.h`)

`config.h` (gitignored — copy from `config.example.h`) holds these `#define`s: `WIFI_SSID`, `WIFI_PASSWORD`, `NOWPLAYING_API_URL`, and `LIGHT_SENSE_THRESHOLD` (ambient-light cutoff for turning the display on/off — moved out of the `.ino` so it's tunable per device). **`config.h` must exist to build.** Never commit it (it has the WiFi password); it's listed in `.gitignore`.

`nowPlaying.h` polls `NOWPLAYING_API_URL` over HTTPS (reusing the global `client` with `setInsecure()`, since the proxy sits behind a rotating cert) and parses the now-playing JSON. The expected response shape: `{ isPlaying, title, artist, album, albumImageUrl, url }`; `isPlaying` is always present, even on errors. Behavior:

- **isPlaying:false** → do nothing (keep the last album art on screen).
- **Playing** → if `url` changed, redraw track text (a minimal `CurrentlyPlaying` is fabricated to reuse `printCurrentlyPlayingToScreen`; no-op on the matrix). If the album art changed (`isSameAlbum`), reuse the unchanged `setAlbumArtUrl` → `displayImage()` download/decode pipeline.

**Album-art sizing:** the proxy returns the *largest* (`640px`) `i.scdn.co` URL. The displays each decode a specific size (matrix 64px 1:1, CYD 300px at half scale). `rewriteAlbumArtSize()` overwrites the 4-hex size code that always follows `ab67616d0000` in the URL (`b273`=640, `1e02`=300, `4851`=64) so each display fetches the size it already decodes for — the decode paths in `cheapYellowLCD.h`/`matrixDisplay.h` are untouched. The target code is chosen at compile time (`4851` for `MATRIX_DISPLAY`, else `1e02`). If the URL doesn't match the album-art pattern, it's used as-is.

Note: the API carries no progress/duration, so the CYD progress bar (`displayTrackProgress`) is effectively gone, and CYD touch next/prev no longer works (it called the now-unused token-backed Spotify API).

### NFC (optional)

`nfc.h` is only compiled when `NFC_ENABLED` is defined (currently `#define NFC_ENABLED 0` at the top of the `.ino` — note this is a truthy define, the `#ifdef` will be true; flip to `#undef NFC_ENABLED` or remove the line to disable). Uses a modified `Seeed_Arduino_NFC` over SPI. CYD wires the PN532 through the microSD slot (DAT2/CD/CMD/CLK/DAT0); Trinity uses IO21/22/32/33. `writeContextToNfc` toggles whether a swiped tag writes the current playback context back. Note: tag-triggered *playback* calls the Spotify API, which needs an access token — so with the current tokenless flow, NFC playback won't work (reads/writes of tags still run).

### Web flash page

`GitHubPages/` hosts a static ESP Web Tools page (published at `witnessmenow.github.io/Spotify-Diy-Thing`) for flashing prebuilt firmware over WebSerial. Editing the firmware does not update this — prebuilt binaries are managed separately.

## Spotify market (legacy / unused)

`SPOTIFY_MARKET` (`"CA"`, near the top of the `.ino`) only mattered for the old on-device `getCurrentlyPlaying` call. The current flow polls the proxy instead, so this define is no longer used — any region handling now lives server-side in the proxy.
