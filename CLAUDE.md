# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

ESP32 firmware that connects to the Spotify Web API and displays the user's currently playing track (with album art) on either a "Cheap Yellow Display" (CYD, ESP32-2432S028R, 320x240 ILI9341) or a 64x64 HUB75 LED matrix panel driven by an ESP32 Trinity. Optional PN532 NFC reader lets a tag trigger playback of a Spotify URI.

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

## Build / flash (Arduino IDE)

The display selection is controlled by `#define YELLOW_DISPLAY` / `#define MATRIX_DISPLAY` at the top of `SpotifyDiyThing/SpotifyDiyThing.ino` (defaults to `YELLOW_DISPLAY` if neither is set). PlatformIO sets these via `build_flags`, so do not also uncomment them in the `.ino` when building with PlatformIO — pick one mechanism.

For Arduino IDE CYD builds, the `TFT_eSPI` library must be configured by replacing its bundled `User_Setup.h` with `DisplayConfig/User_Setup.h` from this repo. PlatformIO bypasses this by passing the same defines as `build_flags`.

## Architecture

Single-sketch Arduino project; all source lives in `SpotifyDiyThing/` as the `.ino` plus header-only modules (each `.h` contains both declarations and definitions — there are no `.cpp` files except the touchscreen driver).

Entry point `SpotifyDiyThing.ino` wires everything together:

1. `DoubleResetDetector` — double-press of reset within `DRD_TIMEOUT` forces WiFiManager config portal.
2. `spotifyDisplay->displaySetup(&spotify)` — display brought up first so subsequent steps can show progress.
3. SPIFFS mount → `fetchConfigFile` (`configFile.h`) loads stored `refreshToken`, `clientId`, `clientSecret`.
4. `setupWiFiManager` (`WifiManagerHandler.h`) — captive portal SSID `SpotifyDiy` / password `thing123`, also collects Spotify client id/secret as custom fields; on save calls back into `saveConfigFile`.
5. `launchRefreshTokenFlow` (`refreshToken.h`) — runs an on-device web server to complete the Spotify OAuth code-exchange the first time (or when GPIO0 is held low on CYD).
6. `spotifySetup` / `spotifyLoop` (`spotifyLogic.h`) — polls `SpotifyArduino::getCurrentlyPlaying`, drives album-art download + JPEG decode, calls back into the display.
7. `ArduinoOTA` is started with hostname `spotifyscreen` — this is also why the trinity env uploads to `spotifyscreen.local`.

### Display abstraction

`spotifyDisplay.h` defines an abstract `SpotifyDisplay` base class. The `.ino` picks one concrete implementation at compile time via the display defines:

- `cheapYellowLCD.h` → `CheapYellowDisplay` (uses TFT_eSPI; depends on the `User_Setup.h` defines listed in `[common_cyd] build_flags`)
- `matrixDisplay.h` → `MatrixDisplay` (uses ESP32-HUB75-MatrixPanel-I2S-DMA + Adafruit_GFX)

`spotifyLogic.h` only talks to the base class pointer `spotifyDisplay`, so adding a new display means subclassing `SpotifyDisplay` and adding a new `#elif defined MY_DISPLAY` block in the `.ino`. The base class also stores screen/image dimensions and the last album-art URL (used to skip re-downloading when the album hasn't changed — see `isSameAlbum`).

### NFC (optional)

`nfc.h` is only compiled when `NFC_ENABLED` is defined (currently `#define NFC_ENABLED 0` at the top of the `.ino` — note this is a truthy define, the `#ifdef` will be true; flip to `#undef NFC_ENABLED` or remove the line to disable). Uses a modified `Seeed_Arduino_NFC` over SPI. CYD wires the PN532 through the microSD slot (DAT2/CD/CMD/CLK/DAT0); Trinity uses IO21/22/32/33. `writeContextToNfc` toggles whether a swiped tag writes the current playback context back.

### Web flash page

`GitHubPages/` hosts a static ESP Web Tools page (published at `witnessmenow.github.io/Spotify-Diy-Thing`) for flashing prebuilt firmware over WebSerial. Editing the firmware does not update this — prebuilt binaries are managed separately.

## Spotify market

`SPOTIFY_MARKET` is hardcoded near the top of the `.ino` (currently `"CA"`). Change this define if testing/displaying in a different region — incorrect market can cause `getCurrentlyPlaying` to return no track.
