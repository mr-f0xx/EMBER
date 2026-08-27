/*
    network.h
    Remote music browsing/playback for Ember+ (M5Stack Cardputer ADV).

    Talks to a Subsonic-compatible server (Gonic, Navidrome, Airsonic...):
      - WiFi from /wifi.txt on the SD card (or hardcoded fallback below)
      - server from /subsonic.txt : URL, username, password (3 lines)
      - artists -> albums -> songs browsing via the Subsonic API
        (getIndexes / getArtist / getAlbum, XML, token auth md5(pass+salt))
      - playback via stream.view with HTTP Range (AudioFileSourceHTTPRange)

    The audio pipeline itself stays in main.cpp (playNetSong etc.); this
    module owns WiFi, HTTP/XML plumbing and the browse screens.
*/

#pragma once

#include <Arduino.h>
#include <M5Cardputer.h>

// ---------------------------------------------------------------------------
// UI context handed over by main.cpp (theme colors + fonts), so the network
// screens always match EMBER's active theme.
// ---------------------------------------------------------------------------
struct NetPalette {
    uint16_t bg, header, headerFg, selBg, selFg, fileFg, dimFg;
    int headerH, rowH;
    const lgfx::IFont* fontUI;
    const lgfx::IFont* fontBrowser;
};

// Implemented in main.cpp.
void emb_getNetPalette(NetPalette& p);
void emb_drawStatusIcons();
bool emb_uiIsNet();
bool emb_isNetPlaying();
void emb_playNetSong(int idx, bool jumpToNowPlaying);

// ---------------------------------------------------------------------------
// Network module API
// ---------------------------------------------------------------------------
namespace net {

    // Connect WiFi + load config + authenticate + fetch the artist list.
    // Playback must already be stopped by the caller; afterwards the caller
    // sets uiMode = MODE_NET and redraws.
    void enter();

    // Disconnect WiFi and free the browse lists. Playback must already be
    // stopped by the caller.
    void exit();

    // Periodic housekeeping (transient message expiry).
    void tick();

    // Full redraw of the network screen (list, busy or error state).
    void drawScreen();

    // List navigation (same keys as the file browser: ; . / , Enter).
    void moveCursor(int delta);
    void onEnter();        // open item / play song / retry on error screens
    // songs -> albums -> artists; returns true when already at the artist
    // level, meaning the caller (main.cpp) should exit network mode.
    bool goBack();

    // Next/previous song (n / b, or Now Playing double-tap); only when a
    // song list is loaded.
    void playNext(int delta);

    // Transient status line at the bottom of the network screen.
    void showMessage(const char* msg, uint32_t ttlMs = 3000);
    void clearMessage();

    // Song list access for main.cpp.
    int  songCount();
    const char* songTitle(int idx);            // list row text
    void songMeta(int idx, char* artist, size_t artistSz,
                  char* album, size_t albumSz,
                  char* title, size_t titleSz);
    String streamURL(int idx);                 // full stream.view URL with auth
    int  currentIndex();
    void setCurrent(int idx);
    void setCurrentInvalid();
}

// Hardcoded fallbacks when the SD files are absent (empty = disabled).
// Prefer /wifi.txt and /subsonic.txt on the SD card.
#ifndef NET_WIFI_SSID
#define NET_WIFI_SSID ""
#endif
#ifndef NET_WIFI_PASS
#define NET_WIFI_PASS ""
#endif
#ifndef NET_SERVER_URL
#define NET_SERVER_URL ""
#endif
#ifndef NET_SERVER_USER
#define NET_SERVER_USER ""
#endif
#ifndef NET_SERVER_PASS
#define NET_SERVER_PASS ""
#endif
