/*
    AudioFileSourceHTTPRange
    Seekable HTTP(S) file source for the AudioFileSource interface of
    earlephilhower/ESP8266Audio (v1.9.x), built for Ember+ (M5Stack
    Cardputer ADV, ESP32-S3, no PSRAM).

    Unlike AudioFileSourceHTTPStream (live-radio oriented), this source is
    for remote FILES served with HTTP Range support (Subsonic/Gonic/Navidrome
    stream.view, WebDAV GET...):

      - open()  : GET with "Range: bytes=0-" ; total size parsed from
                  Content-Range (or Content-Length when the server replies
                  200 with no range header)
      - seek()  : reconnects with "Range: bytes=pos-" -- the only sane way
                  to seek on ESP32 without PSRAM. Used by MP3 seeking and
                  the FLAC decoder's metadata reads.
      - read()  : partial-delivery semantics (never blocks the main loop
                  long), so a slow server can't freeze the UI.

    The object is meant to be REUSED across tracks (open() fully resets it)
    so the 16 KB buffer is allocated once per network session.
*/

#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "AudioFileSource.h"

class AudioFileSourceHTTPRange : public AudioFileSource {
public:
    AudioFileSourceHTTPRange(int buffSize = 16384);
    ~AudioFileSourceHTTPRange();

    virtual bool open(const char* url) override;
    virtual uint32_t read(void* data, uint32_t len) override;
    virtual uint32_t readNonBlock(void* data, uint32_t len) override;
    virtual bool seek(int32_t pos, int dir) override;
    virtual bool close() override;
    virtual bool isOpen() override;
    virtual uint32_t getSize() override;
    virtual uint32_t getPos() override;

    int httpCode() { return _httpCode; }
    const char* getContentType() { return _contentType.c_str(); }

private:
    bool connectRange(uint32_t start);
    void endClient();
    uint32_t serve(uint8_t* out, uint32_t len, uint32_t graceMs, uint32_t hardMs);

    HTTPClient      _http;
    WiFiClient      _plain;
    WiFiClientSecure _secure;
    WiFiClient*     _client = nullptr;
    bool            _isSecure = false;

    uint8_t*  _buff = nullptr;
    uint32_t  _buffLen = 0, _buffValid = 0, _buffPos = 0;

    int64_t   _pos = 0;      // bytes delivered to the decoder so far
    int64_t   _size = -1;    // total file size from Content-Range
    int       _httpCode = 0;
    String    _contentType;
    char      _url[256] = {0};
};
