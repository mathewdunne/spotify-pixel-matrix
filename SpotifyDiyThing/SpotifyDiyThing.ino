/*******************************************************************
    Displays Album Art on a 320 x 240 ESP32.

    Parts:
    ESP32 With Built in 320x240 LCD with Touch Screen (ESP32-2432S028R)
    https://github.com/witnessmenow/Spotify-Diy-Thing#hardware-required

 *******************************************************************/

// ----------------------------
// Display type
// ---------------------------

// This project currently supports the following displays
// (Uncomment the required #define)

// 1. Cheap yellow display (Using TFT-eSPI library)
// #define YELLOW_DISPLAY

// 2. Matrix Displays (Like the ESP32 Trinity)
// #define MATRIX_DISPLAY

// If no defines are set, it will default to CYD
#if !defined(YELLOW_DISPLAY) && !defined(MATRIX_DISPLAY)
#define YELLOW_DISPLAY // Default to Yellow Display for display type
#endif

#define NFC_ENABLED 0
#define LIGHT_SENSE_PIN 35
// LIGHT_SENSE_THRESHOLD is defined in config.h (tunable per device)

#define NUM_SAMPLES 10
uint16_t lightSenseSamples[NUM_SAMPLES];
uint8_t sampleIndex = 0;

// This causes issues in certain circumstances e.g. Play an album and let it auto play to related songs
bool writeContextToNfc = true;

// ----------------------------
// Library Defines - Need to be defined before library import
// ----------------------------

#define ESP_DRD_USE_SPIFFS true

// ----------------------------
// Standard Libraries
// ----------------------------
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <FS.h>
#include "SPIFFS.h"

// ----------------------------
// Additional Libraries - each one of these will need to be installed.
// ----------------------------

#include <WiFiManager.h>
// Captive portal for configuring the WiFi

// If installing from the library manager (Search for "WifiManager")
// https://github.com/tzapu/WiFiManager

#include <ESP_DoubleResetDetector.h>
// A library for checking if the reset button has been pressed twice
// Can be used to enable config mode
// Can be installed from the library manager (Search for "ESP_DoubleResetDetector")
// https://github.com/khoih-prog/ESP_DoubleResetDetector

#include <SpotifyArduino.h>

// including a "spotify_server_cert" variable
// header is included as part of the SpotifyArduino libary
#include <SpotifyArduinoCert.h>

#include <ArduinoJson.h>

#include <ArduinoOTA.h>

WiFiClientSecure client;

//------- Replace the following! ------

// Country code, including this is advisable
#define SPOTIFY_MARKET "CA"
//------- ---------------------- ------

// ----------------------------
// Internal includes
// ----------------------------
#include "refreshToken.h"

#include "spotifyDisplay.h"

#include "spotifyLogic.h"

#include "configFile.h"

#include "serialPrint.h"

#include "WifiManagerHandler.h"

// Hardcoded WiFi + now-playing API config (gitignored; see config.example.h)
#include "config.h"

// ----------------------------
// Display Handling Code
// ----------------------------

#if defined YELLOW_DISPLAY

#include "cheapYellowLCD.h"
CheapYellowDisplay cyd;
SpotifyDisplay *spotifyDisplay = &cyd;

#elif defined MATRIX_DISPLAY
#include "matrixDisplay.h"
MatrixDisplay matrixDisplay;
SpotifyDisplay *spotifyDisplay = &matrixDisplay;

#endif
// ----------------------------

// Now-playing poller — uses `spotifyDisplay` and `client` defined above
#include "nowPlaying.h"

#ifdef NFC_ENABLED
#include "nfc.h"
#endif

void drawWifiManagerMessage(WiFiManager *myWiFiManager)
{
  spotifyDisplay->drawWifiManagerMessage(myWiFiManager);
}

bool isDisplaying = false;

// Connect to WiFi using the hardcoded credentials from config.h. Replaces the
// old WiFiManager captive-portal flow.
void connectWiFi()
{
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("spotifyscreen");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 30000)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi connection failed, restarting...");
    delay(1000);
    ESP.restart();
  }

  Serial.print("Connected. IP address: ");
  Serial.println(WiFi.localIP());
}

void setup()
{
  Serial.begin(115200);

  spotifyDisplay->displaySetup(&spotify);

#ifdef NFC_ENABLED
  if (nfcSetup(&spotify, spotifyDisplay))
  {
    Serial.println("NFC Good");
  }
  else
  {
    Serial.println("NFC Bad");
  }
#endif

  // Initialise SPIFFS — used to buffer the downloaded album art at /album.jpg.
  bool spiffsInitSuccess = SPIFFS.begin(false) || SPIFFS.begin(true);
  if (!spiffsInitSuccess)
  {
    Serial.println("SPIFFS initialisation failed!");
    while (1)
      yield(); // Stay here twiddling thumbs waiting
  }
  Serial.println("\r\nInitialisation done.");

  // Connect with the hardcoded credentials from config.h. The old WiFiManager
  // captive portal + SPIFFS config + Spotify OAuth refresh-token flow are left
  // in the codebase (refreshToken.h / WifiManagerHandler.h / configFile.h) but
  // are no longer called.
  connectWiFi();

  // The display keeps the `spotify` object only to download album art via
  // getImage(); no Spotify access token is needed for that.
  nowPlayingSetup(spotifyDisplay);

  spotifyDisplay->showDefaultScreen();

  pinMode(LIGHT_SENSE_PIN, INPUT);

  // Initialize lightSenseSamples array
  for (int i = 0; i < NUM_SAMPLES; i++)
  {
    lightSenseSamples[i] = 0;
  }

  ArduinoOTA.setHostname("spotifyscreen");
  ArduinoOTA.onStart([]()
                     { Serial.println("OTA update start"); });
  ArduinoOTA.onEnd([]()
                   { Serial.println("\nOTA update complete"); });
  ArduinoOTA.onError([](ota_error_t error)
                     { Serial.printf("OTA Error [%u]\n", error); });

  ArduinoOTA.begin(); // Start OTA service
}

void loop()
{
  bool forceUpdate = false;

#ifdef NFC_ENABLED
  if (writeContextToNfc)
  {
    forceUpdate = nfcLoop(lastTrackUri, lastTrackContextUri);
  }
  else
  {
    forceUpdate = nfcLoop(lastTrackUri);
  }

#endif

  // Update the moving average
  lightSenseSamples[sampleIndex] = analogRead(LIGHT_SENSE_PIN);
  sampleIndex = (sampleIndex + 1) % NUM_SAMPLES;

  uint32_t sum = 0;
  for (int i = 0; i < NUM_SAMPLES; i++)
  {
    sum += lightSenseSamples[i];
  }
  uint16_t lightSense = sum / NUM_SAMPLES;

  // Serial.println(lightSense);
  if (lightSense > LIGHT_SENSE_THRESHOLD)
  {
    bool justWokeUp = !isDisplaying;
    if (justWokeUp)
    {
      // Sensor just uncovered — the screen was blanked. Redraw the last album
      // art straight from the buffered file so it reappears immediately, even
      // if nothing is playing now (in which case the poll below redraws nothing).
      spotifyDisplay->redrawAlbumArt();
      isDisplaying = true;
    }

    spotifyDisplay->checkForInput();

    updateNowPlaying(forceUpdate || justWokeUp);
  }
  else
  {
    if (isDisplaying)
    {
      spotifyDisplay->showDefaultScreen();
      isDisplaying = false;
    }
  }

  ArduinoOTA.handle(); // Listen for OTA updates
}
