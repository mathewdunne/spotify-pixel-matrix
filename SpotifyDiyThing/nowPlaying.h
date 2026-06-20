#ifndef NOWPLAYING_H
#define NOWPLAYING_H

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// -----------------------------------------------------------------------------
// Polls the custom now-playing proxy (NOWPLAYING_API_URL from config.h) and
// drives the display. This replaces the old direct Spotify Web API path in
// spotifyLogic.h (which is left in the codebase but no longer called).
//
// Reuses the global `client` (WiFiClientSecure) for the HTTPS poll and the
// display's existing album-art download/JPEG-decode pipeline unchanged.
// -----------------------------------------------------------------------------

static SpotifyDisplay *np_Display = nullptr;

// Spotify i.scdn.co album-art URLs encode the image size as a 4-hex code that
// always follows "ab67616d0000":  b273 = 640px, 1e02 = 300px, 4851 = 64px.
// The API returns the largest (640). We rewrite that code so each display gets
// the size it already decodes for (matrix 64px -> decode 1:1, CYD 300px ->
// decode at half scale), so the existing decode path needs no changes.
#if defined MATRIX_DISPLAY
#define ALBUM_ART_SIZE_CODE "4851" // 64x64
#else
#define ALBUM_ART_SIZE_CODE "1e02" // 300x300
#endif

// The proxy caches server-side for ~20s, so polling faster just returns the same
// data. It also holds the last track through short pauses/skips.
unsigned long delayBetweenNowPlaying = 20000;
unsigned long nowPlayingDueTime = 0;

char lastTrackUrl[200] = "";

// Overwrite the 4-hex size code (the chars right after "ab67616d0000") in place.
// If the album-art pattern isn't present the URL is left untouched.
void rewriteAlbumArtSize(char *url, const char *sizeCode)
{
  char *p = strstr(url, "ab67616d0000");
  if (p != NULL)
  {
    memcpy(p + 12, sizeCode, 4);
  }
}

void nowPlayingSetup(SpotifyDisplay *display)
{
  np_Display = display;
  lastTrackUrl[0] = '\0';
}

void updateNowPlaying(boolean forceUpdate)
{
  if (!forceUpdate && millis() < nowPlayingDueTime)
  {
    return;
  }
  nowPlayingDueTime = millis() + delayBetweenNowPlaying;

  Serial.println("Polling now-playing API");

  // Values copied out of the JSON so they outlive the poll's HTTP/TLS objects.
  bool gotData = false;
  bool isPlaying = false;
  char title[128] = "";
  char artist[128] = "";
  char album[128] = "";
  char trackUrl[128] = "";
  char artUrl[200] = "";

  // Poll inside its own scope. The API uses HTTPS (its own cert) while album art
  // comes from i.scdn.co (a different cert on the global `client`). Sharing one
  // WiFiClientSecure for both — switching it between setInsecure() and
  // setCACert() — corrupts the TLS context and leaks heap on the ESP32. So we
  // give the poll a dedicated client and let RAII tear it (and all its TLS
  // buffers) down here, before the image download opens its own connection.
  {
    WiFiClientSecure apiClient;
    apiClient.setInsecure(); // proxy sits behind a rotating cert; don't pin it

    HTTPClient http;
    if (http.begin(apiClient, NOWPLAYING_API_URL))
    {
      int httpCode = http.GET();
      if (httpCode > 0)
      {
        // The API always returns parseable JSON with isPlaying, even on errors.
        String payload = http.getString();
        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, payload);
        if (!error)
        {
          isPlaying = doc["isPlaying"] | false;
          snprintf(title, sizeof(title), "%s", doc["title"] | "");
          snprintf(artist, sizeof(artist), "%s", doc["artist"] | "");
          snprintf(album, sizeof(album), "%s", doc["album"] | "");
          snprintf(trackUrl, sizeof(trackUrl), "%s", doc["url"] | "");
          snprintf(artUrl, sizeof(artUrl), "%s", doc["albumImageUrl"] | "");
          gotData = true;
        }
        else
        {
          Serial.print("JSON parse failed: ");
          Serial.println(error.c_str());
        }
      }
      else
      {
        Serial.print("HTTP GET failed: ");
        Serial.println(HTTPClient::errorToString(httpCode));
      }
      http.end();
    }
    else
    {
      Serial.println("http.begin failed");
    }
    apiClient.stop();
  } // apiClient, http, payload, doc all freed here

  if (!gotData)
  {
    return;
  }

  if (!isPlaying)
  {
    // Nothing playing (or a non-track) -> keep the last album art on screen.
    Serial.println("Nothing playing, keeping last art");
    return;
  }

  if (strlen(artUrl) == 0)
  {
    Serial.println("Playing, but no album image url");
    return;
  }

  // New track -> refresh the on-screen text (no-op on the matrix display). The
  // local buffers live to the end of this function, so the fabricated
  // CurrentlyPlaying's pointers stay valid for the call.
  if (strcmp(lastTrackUrl, trackUrl) != 0)
  {
    snprintf(lastTrackUrl, sizeof(lastTrackUrl), "%s", trackUrl);

    CurrentlyPlaying cp = {};
    cp.trackName = title;
    cp.albumName = album;
    cp.artists[0].artistName = artist;
    cp.numArtists = 1;
    cp.isPlaying = true;
    np_Display->printCurrentlyPlayingToScreen(cp);
  }

  // Request the size this display already decodes for, then reuse the existing
  // download + JPEG decode pipeline (setAlbumArtUrl / isSameAlbum / displayImage),
  // which downloads through the global `client`.
  rewriteAlbumArtSize(artUrl, ALBUM_ART_SIZE_CODE);

  if (!np_Display->isSameAlbum(artUrl))
  {
    Serial.print("New album art, downloading: ");
    Serial.println(artUrl);
    np_Display->setAlbumArtUrl(artUrl);
    int result = np_Display->displayImage();
    if (result != 1)
    {
      Serial.print("Failed to display image: ");
      Serial.println(result);
      // Clear the stored URL so the next poll retries this album.
      np_Display->setAlbumArtUrl("");
    }
  }
  else
  {
    // Steady state: same album still playing, nothing to redraw.
    Serial.print("Still playing (same album): ");
    Serial.println(title);
  }
}

#endif
