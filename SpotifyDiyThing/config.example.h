#pragma once

// -----------------------------------------------------------------------------
// Device configuration template.
//
// Copy this file to "config.h" (which is .gitignored) and fill in your real
// values. config.h is required to build — it holds the hardcoded WiFi
// credentials and the now-playing API URL the device polls.
// -----------------------------------------------------------------------------

#define WIFI_SSID          "your-wifi-ssid"
#define WIFI_PASSWORD      "your-wifi-password"

// The custom now-playing proxy endpoint. Must return the JSON documented in the
// project README / CLAUDE.md (title/artist/album/albumImageUrl/isPlaying/url).
#define NOWPLAYING_API_URL "https://custom.spotify.url/"

// Ambient-light threshold: the display turns on when the averaged reading on
// LIGHT_SENSE_PIN is above this, and off below it. Lower = stays on in dimmer
// rooms. Tune to taste without touching the sketch.
#define LIGHT_SENSE_THRESHOLD 375
