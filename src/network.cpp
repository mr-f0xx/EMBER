/*
    network.cpp
    Subsonic/Gonic remote browsing + playback support for Ember+.

    On-disk layout (SD card root -- invisible in the browser, which only
    shows the Music folder):
      /wifi.txt      line 1 = SSID, line 2 = password (comments allowed)
      /subsonic.txt  line 1 = server URL (e.g. http://192.168.1.10:4533)
                     line 2 = username
                     line 3 = password
    Hardcoded fallbacks live at the bottom of network.h.

    Server protocol: Subsonic API, XML output, token auth (t=md5(pass+salt)).
    Works with Gonic, Navidrome, Airsonic-Advanced, stock Subsonic...
*/

#include "network.h"
#include <SD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/md5.h>
#include <esp_system.h>
#include <string.h>
#include <stdlib.h>

static const char* CFG_FILE   = "/subsonic.txt";
static const char* WIFI_FILE  = "/wifi.txt";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static const int MAX_LIST = 200;
static const int NET_ID_MAX   = 24;
static const int NET_NAME_MAX = 96;

// One shared list pool, re-allocated for each browse level (artists /
// albums / songs) -- artists need the most, so size for that.
static char (*listIds)[NET_ID_MAX]     = nullptr;
static char (*listNames)[NET_NAME_MAX] = nullptr;
static int  listCount = 0;

static int  cursor_ = 0, scroll_ = 0, visibleRows_ = 0;
static int  netLevel = 0;                 // 0 artists, 1 albums, 2 songs
static int  cursorMem[3] = {0, 0, 0};

static char artistId[NET_ID_MAX] = "", artistName[NET_NAME_MAX] = "";
static char albumId[NET_ID_MAX]  = "", albumName[NET_NAME_MAX]  = "";

static char srvBase[160] = "", srvUser[64] = "", srvPass[64] = "";

enum NetState {
    NET_CFG_FAIL, NET_WIFI, NET_WIFI_FAIL, NET_PING, NET_AUTH_FAIL,
    NET_LOAD_ARTISTS, NET_ARTISTS, NET_LOAD_ALBUMS, NET_ALBUMS,
    NET_LOAD_SONGS, NET_SONGS, NET_NO_LIST, NET_ERROR
};
static NetState state_ = NET_CFG_FAIL;

static char message_[160] = "";
static uint32_t messageUntil_ = 0;
static NetPalette p_;

static int currentSong_ = -1;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static bool asciiOnly(const char* s) {
    for (const unsigned char* q = (const unsigned char*)s; *q; q++)
        if (*q >= 0x80) return false;
    return true;
}

static void pickFont(LovyanGFX& d, const char* s) {
    d.setFont(asciiOnly(s) ? p_.fontBrowser : p_.fontUI);
}

static String trimToWidth(LovyanGFX& d, const String& text, int maxW) {
    if (d.textWidth(text.c_str()) <= maxW) return text;
    int ellW = d.textWidth("~");
    String out = text;
    while (out.length() > 1 && d.textWidth(out.c_str()) > (maxW - ellW))
        out.remove(out.length() - 1);
    out += "~";
    return out;
}

// ---------------------------------------------------------------------------
// Config files
// ---------------------------------------------------------------------------
static void readConfigLines(const char* path, String* lines, int maxLines) {
    File f = SD.open(path);
    if (!f) return;
    int got = 0;
    while (f.available() && got < maxLines) {
        String l = f.readStringUntil('\n');
        l.trim();
        if (l.length() == 0 || l[0] == '#') continue;
        lines[got++] = l;
    }
    f.close();
}

static bool loadConfig() {
    srvBase[0] = srvUser[0] = srvPass[0] = 0;
    String lines[3];
    readConfigLines(CFG_FILE, lines, 3);
    const char* url  = lines[0].length() ? lines[0].c_str() : NET_SERVER_URL;
    const char* user = lines[1].length() ? lines[1].c_str() : NET_SERVER_USER;
    const char* pass = lines[2].length() ? lines[2].c_str() : NET_SERVER_PASS;
    strncpy(srvBase, url,  sizeof(srvBase) - 1);
    strncpy(srvUser, user, sizeof(srvUser) - 1);
    strncpy(srvPass, pass, sizeof(srvPass) - 1);
    srvBase[sizeof(srvBase) - 1] = 0;
    srvUser[sizeof(srvUser) - 1] = 0;
    srvPass[sizeof(srvPass) - 1] = 0;
    while (srvBase[0] && srvBase[strlen(srvBase) - 1] == '/')
        srvBase[strlen(srvBase) - 1] = 0;              // strip trailing slashes
    return srvBase[0] && srvUser[0] && srvPass[0];
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
static void drawBusyFrame(const char* text, int dotCount) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, d.width(), d.height(), p_.bg);
    d.fillRect(0, 0, d.width(), p_.headerH, p_.header);
    d.setFont(p_.fontUI);
    d.setTextColor(p_.headerFg, p_.header);
    d.setCursor(4, (p_.headerH - d.fontHeight()) / 2);
    d.print("Réseau");
    emb_drawStatusIcons();
    d.setTextColor(p_.dimFg, p_.bg);
    int y = p_.headerH + (d.height() - p_.headerH) / 2 - d.fontHeight();
    d.setCursor((d.width() - d.textWidth(text)) / 2, y);
    d.print(text);
    String dots;
    for (int i = 0; i < dotCount; i++) dots += '.';
    d.setCursor((d.width() - d.textWidth("...")) / 2, y + d.fontHeight() + 6);
    d.print(dots);
}

static bool connectWifi() {
    String lines[2];
    readConfigLines(WIFI_FILE, lines, 2);
    const char* ssid = lines[0].length() ? lines[0].c_str() : NET_WIFI_SSID;
    const char* pass = lines[1].length() ? lines[1].c_str() : NET_WIFI_PASS;
    if (!ssid[0]) return false;

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.disconnect(true);
    WiFi.begin(ssid, pass);

    uint32_t t0 = millis();
    int dots = 0;
    unsigned long lastDot = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
        unsigned long now = millis();
        if (now - lastDot >= 150) {
            lastDot = now;
            dots = (dots + 1) % 4;
            drawBusyFrame("Connexion WiFi", dots);
        }
        delay(20);
    }
    return WiFi.status() == WL_CONNECTED;
}

// ---------------------------------------------------------------------------
// Subsonic API plumbing (token auth, XML)
// ---------------------------------------------------------------------------
static void md5Hex(const char* input, char* outHex) {
    uint8_t hash[16];
    mbedtls_md5((const unsigned char*)input, strlen(input), hash);
    for (int i = 0; i < 16; i++) sprintf(outHex + i * 2, "%02x", hash[i]);
}

static String urlEncode(const char* s) {
    static const char* hex = "0123456789ABCDEF";
    String out;
    for (const char* p = s; *p; p++) {
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// Token auth: t = md5(password + salt), random salt per request. This is the
// scheme every actively maintained Subsonic server expects (Gonic included),
// and it never sends the password itself.
static String authParams() {
    static const char* chars = "abcdefghijklmnopqrstuvwxyz0123456789";
    char salt[12];
    for (int i = 0; i < 8; i++) salt[i] = chars[esp_random() % 36];
    salt[8] = 0;
    char in[96];
    snprintf(in, sizeof(in), "%s%s", srvPass, salt);
    char token[40];
    md5Hex(in, token);
    return String("u=") + urlEncode(srvUser) + "&t=" + token + "&s=" + salt +
           "&v=1.16.1&c=EmberPlus&f=xml";
}

// GET a small text response (list/status endpoints). Returns the HTTP code,
// body in `out` (capped). Timeouts are short so a dead server never freezes
// the device.
static int httpGetText(const String& url, String& out, int maxBytes = 65535) {
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    WiFiClient* client = &plain;
    bool https = url.startsWith("https://");
    if (https) {
        secure.setInsecure();
        secure.setTimeout(8);
        secure.setHandshakeTimeout(10);
        client = &secure;
    } else {
        plain.setTimeout(8);
    }
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(10000);
    http.setUserAgent("EmberPlus/1.0 (M5Cardputer)");
    if (!http.begin(*client, url)) return -1;
    int code = http.GET();
    if (code == HTTP_CODE_OK) {
        WiFiClient* stream = http.getStreamPtr();
        if (stream) {
            out.reserve(256);
            out = "";
            uint32_t t0 = millis();
            while (stream->connected() && out.length() < (unsigned)maxBytes && millis() - t0 < 12000) {
                if (stream->available()) {
                    uint8_t buf[512];
                    int n = stream->read(buf, sizeof(buf));
                    if (n <= 0) break;
                    for (int i = 0; i < n && out.length() < (unsigned)maxBytes; i++) out += (char)buf[i];
                } else delay(2);
            }
        }
    }
    http.end();
    return code;
}

// Unescape the five XML entities in place.
static void xmlUnescape(char* s) {
    // &amp; &lt; &gt; &quot; &apos;
    char* out = s;
    char* p = s;
    while (*p) {
        if (p[0] == '&' && p[1] && p[2] == ';') {
            switch (p[1]) {
                case 'a': *out++ = '&'; p += 5; continue;   // &amp;
                case 'l': *out++ = '<'; p += 4; continue;   // &lt;
                case 'g': *out++ = '>'; p += 4; continue;   // &gt;
                case 'q': *out++ = '"'; p += 6; continue;   // &quot;
                case 'A': *out++ = '\''; p += 6; continue;  // &apos;
            }
        }
        *out++ = *p++;
    }
    *out = 0;
}

// Extract the value of `attr` from the (which)-th occurrence of "<tag".
// Generic enough for <artist id=".." name=".."/>, <album .../>, <song
// title=".." .../> -- attribute order does not matter.
static bool xmlAttr(const String& xml, const char* tag, int which,
                    const char* attr, char* out, int outLen) {
    String open = String("<") + tag;
    int idx = 0;
    for (int i = 0; i <= which; i++) {
        idx = xml.indexOf(open, idx);
        if (idx < 0) return false;
        idx += open.length();
    }
    // The tag runs from idx (attributes) to the next '>' -- attribute values
    // never contain '>' unescaped (gt is &gt;).
    String pat = String(attr) + "=\"";
    int a = xml.indexOf(pat, idx);
    if (a < 0) return false;
    int close = xml.indexOf('>', idx);
    if (close < 0 || a > close) return false;
    a += pat.length();
    int end = xml.indexOf('"', a);
    if (end < 0 || end > close) return false;
    int n = end - a;
    if (n >= outLen) n = outLen - 1;
    memcpy(out, xml.c_str() + a, n);
    out[n] = 0;
    xmlUnescape(out);
    return true;
}

static int xmlCount(const String& xml, const char* tag) {
    String open = String("<") + tag;
    int count = 0, idx = 0;
    while ((idx = xml.indexOf(open, idx)) >= 0) {
        count++;
        idx += open.length();
    }
    return count;
}

// ---------------------------------------------------------------------------
// Lists
// ---------------------------------------------------------------------------
static void freeLists() {
    if (listIds)   { free(listIds);   listIds   = nullptr; }
    if (listNames) { free(listNames); listNames = nullptr; }
    listCount = 0;
    cursor_ = scroll_ = 0;
    currentSong_ = -1;
}

static bool allocLists() {
    freeLists();
    listIds   = (char(*)[NET_ID_MAX])malloc(MAX_LIST * NET_ID_MAX);
    listNames = (char(*)[NET_NAME_MAX])malloc(MAX_LIST * NET_NAME_MAX);
    return listIds && listNames;
}

static void addItem(const char* id, const char* name) {
    if (listCount >= MAX_LIST) return;
    strncpy(listIds[listCount], id, NET_ID_MAX - 1);
    listIds[listCount][NET_ID_MAX - 1] = 0;
    strncpy(listNames[listCount], name, NET_NAME_MAX - 1);
    listNames[listCount][NET_NAME_MAX - 1] = 0;
    listCount++;
}

// ---------------------------------------------------------------------------
// Subsonic endpoints
// ---------------------------------------------------------------------------
static String endpoint(const char* view) {
    return String(srvBase) + "/rest/" + view + "?" + authParams();
}

static bool pingServer() {
    String body;
    int code = httpGetText(endpoint("ping.view"), body, 8192);
    if (code != HTTP_CODE_OK) return false;
    // status="ok" on success; an <error code=..> element otherwise
    return body.indexOf("status=\"ok\"") >= 0 && body.indexOf("<error") < 0;
}

static bool loadArtists() {
    if (!allocLists()) return false;
    String body;
    // getIndexes.view (folder-based browsing) returns artist ids from a
    // *different* namespace than getArtist.view (ID3-based) expects -- an id
    // from here fed into getArtist.view fails with "couldn't find an artist
    // with that id" even though it looks valid. getArtists.view (plural) is
    // the ID3-based artist list and its ids are what getArtist.view actually
    // wants, so use that instead.
    int code = httpGetText(endpoint("getArtists.view"), body);
    if (code != HTTP_CODE_OK) return false;
    int n = xmlCount(body, "artist");
    char id[NET_ID_MAX], name[NET_NAME_MAX];
    for (int i = 0; i < n && listCount < MAX_LIST; i++) {
        if (xmlAttr(body, "artist", i, "id", id, sizeof(id)) &&
            xmlAttr(body, "artist", i, "name", name, sizeof(name))) {
            addItem(id, name);
        }
    }
    return true;
}

static bool loadAlbums() {
    if (!allocLists()) return false;
    String body;
    int code = httpGetText(endpoint("getArtist.view") + "&id=" + urlEncode(artistId), body);
    if (code != HTTP_CODE_OK) return false;
    int n = xmlCount(body, "album");
    char id[NET_ID_MAX], name[NET_NAME_MAX];
    for (int i = 0; i < n && listCount < MAX_LIST; i++) {
        if (xmlAttr(body, "album", i, "id", id, sizeof(id)) &&
            xmlAttr(body, "album", i, "name", name, sizeof(name))) {
            addItem(id, name);
        }
    }
    return true;
}

static bool loadSongs() {
    if (!allocLists()) return false;
    String body;
    int code = httpGetText(endpoint("getAlbum.view") + "&id=" + urlEncode(albumId), body);
    if (code != HTTP_CODE_OK) return false;
    int n = xmlCount(body, "song");
    char id[NET_ID_MAX], title[NET_NAME_MAX];
    for (int i = 0; i < n && listCount < MAX_LIST; i++) {
        if (xmlAttr(body, "song", i, "id", id, sizeof(id)) &&
            xmlAttr(body, "song", i, "title", title, sizeof(title))) {
            addItem(id, title);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void drawHeader() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, d.width(), p_.headerH, p_.header);
    d.setFont(p_.fontUI);
    d.setTextColor(p_.headerFg, p_.header);
    d.setCursor(4, (p_.headerH - d.fontHeight()) / 2);
    String crumb;
    if (netLevel == 0)      crumb = "Réseau";
    else if (netLevel == 1) crumb = artistName;
    else                    crumb = String(artistName) + "  /  " + albumName;
    d.print(trimToWidth(d, crumb, d.width() - 90));
    emb_drawStatusIcons();
}

static void drawCentered(const char* line1, const char* line2) {
    auto& d = M5Cardputer.Display;
    d.setFont(p_.fontUI);
    d.setTextColor(p_.dimFg, p_.bg);
    int y = p_.headerH + (d.height() - p_.headerH) / 2 - d.fontHeight();
    d.setCursor((d.width() - d.textWidth(line1)) / 2, y);
    d.print(line1);
    if (line2 && line2[0]) {
        d.setCursor((d.width() - d.textWidth(line2)) / 2, y + d.fontHeight() + 6);
        d.print(line2);
    }
}

// Small hand-drawn glyphs in the browser's spirit: folder for artists/albums,
// a music note for songs.
static void drawRowGlyph(LovyanGFX& d, int x, int y, uint16_t color, bool song) {
    if (!song) {
        d.fillRect(x, y, 6, 2, color);
        d.fillRect(x, y + 2, 12, 7, color);
    } else {
        d.fillCircle(x + 3, y + 6, 3, color);
        d.fillRect(x + 5, y + 1, 2, 6, color);
        d.fillRect(x + 5, y + 1, 5, 2, color);
    }
}

static void drawRow(int idx) {
    if (idx < scroll_ || idx >= scroll_ + visibleRows_ || idx >= listCount) return;
    auto& d = M5Cardputer.Display;
    int y = p_.headerH + (idx - scroll_) * p_.rowH;
    bool sel = (idx == cursor_);
    bool cur = (netLevel == 2 && idx == currentSong_);
    uint16_t bg = sel ? p_.selBg : p_.bg;
    uint16_t fg = sel ? p_.selFg : (cur ? p_.dimFg : p_.fileFg);

    if (sel) d.fillRoundRect(2, y + 1, d.width() - 4, p_.rowH - 2, 4, bg);
    else     d.fillRect(0, y, d.width(), p_.rowH, bg);

    drawRowGlyph(d, 6, y + (p_.rowH - 10) / 2, fg, netLevel == 2);
    int textX = 6 + 12 + 5;
    pickFont(d, listNames[idx]);
    d.setTextColor(fg, bg);
    String text(listNames[idx]);
    int availW = d.width() - textX - 4;
    d.setCursor(textX, y + (p_.rowH - d.fontHeight()) / 2);
    d.print(trimToWidth(d, text, availW));
}

static void drawMessageBar() {
    if (!message_[0]) return;
    auto& d = M5Cardputer.Display;
    int y = d.height() - p_.rowH;
    d.fillRect(0, y, d.width(), p_.rowH, p_.bg);
    d.setFont(p_.fontUI);
    d.setTextColor(p_.dimFg, p_.bg);
    d.setCursor(4, y + (p_.rowH - d.fontHeight()) / 2);
    d.print(trimToWidth(d, String(message_), d.width() - 8));
}

static const char* footerRetry = "Entrée : réessayer    ` ou w : quitter";

void net::drawScreen() {
    auto& d = M5Cardputer.Display;
    emb_getNetPalette(p_);
    visibleRows_ = (d.height() - p_.headerH) / p_.rowH;

    d.fillRect(0, 0, d.width(), d.height(), p_.bg);
    drawHeader();

    switch (state_) {
        case NET_CFG_FAIL:
            drawCentered("Serveur non configuré", "ajoutez /subsonic.txt");
            d.setFont(p_.fontUI);
            d.setTextColor(p_.fileFg, p_.bg);
            d.setCursor(4, d.height() - p_.rowH * 2 - 2);
            d.print(footerRetry);
            break;
        case NET_WIFI_FAIL:
            drawCentered("Connexion WiFi impossible", "vérifiez /wifi.txt");
            d.setFont(p_.fontUI);
            d.setTextColor(p_.fileFg, p_.bg);
            d.setCursor(4, d.height() - p_.rowH * 2 - 2);
            d.print(footerRetry);
            break;
        case NET_AUTH_FAIL:
            drawCentered("Identifiants refusés", "vérifiez /subsonic.txt");
            d.setFont(p_.fontUI);
            d.setTextColor(p_.fileFg, p_.bg);
            d.setCursor(4, d.height() - p_.rowH * 2 - 2);
            d.print(footerRetry);
            break;
        case NET_NO_LIST:
            drawCentered("Aucune musique", "sur le serveur");
            break;
        case NET_ERROR:
            drawCentered("Erreur serveur", "impossible de charger");
            d.setFont(p_.fontUI);
            d.setTextColor(p_.fileFg, p_.bg);
            d.setCursor(4, d.height() - p_.rowH * 2 - 2);
            d.print(footerRetry);
            break;
        default:
            if (listCount == 0) {
                d.setFont(p_.fontBrowser);
                d.setTextColor(p_.dimFg, p_.bg);
                d.setCursor(4, p_.headerH + 6);
                d.print("(vide)");
            } else {
                for (int row = 0; row < visibleRows_; row++) {
                    int idx = scroll_ + row;
                    if (idx >= listCount) break;
                    drawRow(idx);
                }
            }
            break;
    }
    drawMessageBar();
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------
static void openSelected() {
    if (state_ != NET_ARTISTS && state_ != NET_ALBUMS && state_ != NET_SONGS) return;
    if (cursor_ >= listCount) return;

    if (netLevel == 0) {                       // artist -> albums
        strncpy(artistId, listIds[cursor_], NET_ID_MAX - 1);
        artistId[NET_ID_MAX - 1] = 0;
        strncpy(artistName, listNames[cursor_], NET_NAME_MAX - 1);
        artistName[NET_NAME_MAX - 1] = 0;
        cursorMem[0] = cursor_;
        netLevel = 1;
        state_ = NET_LOAD_ALBUMS;
        drawBusyFrame("Chargement des albums", 0);
        if (!loadAlbums()) state_ = NET_ERROR;
        else state_ = listCount ? NET_ALBUMS : NET_NO_LIST;
        cursor_ = cursorMem[1] < listCount ? cursorMem[1] : 0;
        scroll_ = 0;
    } else if (netLevel == 1) {                // album -> songs
        strncpy(albumId, listIds[cursor_], NET_ID_MAX - 1);
        albumId[NET_ID_MAX - 1] = 0;
        strncpy(albumName, listNames[cursor_], NET_NAME_MAX - 1);
        albumName[NET_NAME_MAX - 1] = 0;
        cursorMem[1] = cursor_;
        netLevel = 2;
        state_ = NET_LOAD_SONGS;
        drawBusyFrame("Chargement des titres", 0);
        if (!loadSongs()) state_ = NET_ERROR;
        else state_ = listCount ? NET_SONGS : NET_NO_LIST;
        cursor_ = cursorMem[2] < listCount ? cursorMem[2] : 0;
        scroll_ = 0;
    } else {                                   // song -> play
        emb_playNetSong(cursor_, true);
    }
}

void net::onEnter() {
    if (state_ == NET_ARTISTS || state_ == NET_ALBUMS || state_ == NET_SONGS) {
        openSelected();
    } else {
        // error screen: retry the whole chain
        enter();
        drawScreen();
    }
}

bool net::goBack() {
    if (netLevel == 2) {
        cursorMem[2] = cursor_;
        netLevel = 1;
        state_ = NET_LOAD_ALBUMS;
        drawBusyFrame("Chargement des albums", 0);
        if (!loadAlbums()) state_ = NET_ERROR;
        else state_ = listCount ? NET_ALBUMS : NET_NO_LIST;
        cursor_ = cursorMem[1] < listCount ? cursorMem[1] : 0;
        scroll_ = 0;
        return false;
    } else if (netLevel == 1) {
        cursorMem[1] = cursor_;
        netLevel = 0;
        state_ = NET_LOAD_ARTISTS;
        drawBusyFrame("Chargement des artistes", 0);
        if (!loadArtists()) state_ = NET_ERROR;
        else state_ = listCount ? NET_ARTISTS : NET_NO_LIST;
        cursor_ = cursorMem[0] < listCount ? cursorMem[0] : 0;
        scroll_ = 0;
        return false;
    }
    // already at the artist level: the caller exits network mode
    return true;
}

void net::moveCursor(int delta) {
    if (listCount == 0) return;
    int oldCursor = cursor_, oldScroll = scroll_;
    cursor_ += delta;
    if (cursor_ < 0) cursor_ = listCount - 1;
    if (cursor_ >= listCount) cursor_ = 0;
    if (cursor_ < scroll_) scroll_ = cursor_;
    if (cursor_ >= scroll_ + visibleRows_) scroll_ = cursor_ - visibleRows_ + 1;
    if (scroll_ != oldScroll) drawScreen();
    else if (cursor_ != oldCursor) {
        drawRow(oldCursor);
        drawRow(cursor_);
    }
}

void net::playNext(int delta) {
    if (state_ != NET_SONGS || listCount == 0) return;
    int base = (currentSong_ >= 0) ? currentSong_ : cursor_;
    int idx = (base + delta) % listCount;
    if (idx < 0) idx += listCount;
    cursor_ = idx;
    if (cursor_ < scroll_) scroll_ = cursor_;
    if (cursor_ >= scroll_ + visibleRows_) scroll_ = cursor_ - visibleRows_ + 1;
    emb_playNetSong(idx, true);
}

// ---------------------------------------------------------------------------
// Enter / exit / housekeeping
// ---------------------------------------------------------------------------
void net::enter() {
    emb_getNetPalette(p_);
    visibleRows_ = (M5Cardputer.Display.height() - p_.headerH) / p_.rowH;
    freeLists();
    message_[0] = 0;
    cursorMem[0] = cursorMem[1] = cursorMem[2] = 0;
    artistId[0] = artistName[0] = albumId[0] = albumName[0] = 0;
    netLevel = 0;

    if (!loadConfig()) { state_ = NET_CFG_FAIL; return; }

    state_ = NET_WIFI;
    drawBusyFrame("Connexion WiFi", 0);
    if (!connectWifi()) { state_ = NET_WIFI_FAIL; return; }

    state_ = NET_PING;
    drawBusyFrame("Connexion au serveur", 0);
    if (!pingServer()) { state_ = NET_AUTH_FAIL; return; }

    state_ = NET_LOAD_ARTISTS;
    drawBusyFrame("Chargement des artistes", 0);
    if (!loadArtists()) { state_ = NET_ERROR; return; }
    state_ = listCount ? NET_ARTISTS : NET_NO_LIST;
    cursor_ = scroll_ = 0;
}

void net::exit() {
    freeLists();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    state_ = NET_CFG_FAIL;
    message_[0] = 0;
    netLevel = 0;
}

void net::tick() {
    if (message_[0] && messageUntil_ && millis() > messageUntil_) {
        message_[0] = 0;
        if (emb_uiIsNet()) drawScreen();
    }
}

// ---------------------------------------------------------------------------
// Song list access (main.cpp)
// ---------------------------------------------------------------------------
int  net::songCount()           { return (netLevel == 2) ? listCount : 0; }
const char* net::songTitle(int idx) {
    return (idx >= 0 && idx < listCount) ? listNames[idx] : "";
}
void net::songMeta(int idx, char* artist, size_t artistSz,
                   char* album, size_t albumSz,
                   char* title, size_t titleSz) {
    strncpy(artist, artistName, artistSz - 1); artist[artistSz - 1] = 0;
    strncpy(album,  albumName,  albumSz - 1);  album[albumSz - 1]  = 0;
    const char* t = songTitle(idx);
    strncpy(title, t, titleSz - 1);            title[titleSz - 1] = 0;
}
String net::streamURL(int idx) {
    return String(srvBase) + "/rest/stream.view?id=" + urlEncode(listIds[idx]) +
           "&" + authParams();
}
int  net::currentIndex()        { return currentSong_; }
void net::setCurrent(int idx)   { currentSong_ = idx; }
void net::setCurrentInvalid()   { currentSong_ = -1; }

void net::showMessage(const char* msg, uint32_t ttlMs) {
    strncpy(message_, msg, sizeof(message_) - 1);
    message_[sizeof(message_) - 1] = 0;
    messageUntil_ = ttlMs ? (millis() + ttlMs) : 0;
}

void net::clearMessage() {
    message_[0] = 0;
    messageUntil_ = 0;
}
