/*
    AudioFileSourceHTTPRange implementation -- see the header for rationale.
*/

#include "AudioFileSourceHTTPRange.h"

AudioFileSourceHTTPRange::AudioFileSourceHTTPRange(int buffSize) {
    _buffLen = (buffSize > 4096) ? (uint32_t)buffSize : 4096;
    _buff = (uint8_t*)malloc(_buffLen);
}

AudioFileSourceHTTPRange::~AudioFileSourceHTTPRange() {
    endClient();
    if (_buff) { free(_buff); _buff = nullptr; }
}

void AudioFileSourceHTTPRange::endClient() {
    _http.end();
    if (_client) _client->stop();
    _client = nullptr;
    _isSecure = false;
}

// GET with "Range: bytes=start-" and parse the total size. Returns false if
// the server refuses the range (a 200 response for start > 0 means the
// server ignored it -- seeking would corrupt playback, so treat as failure).
bool AudioFileSourceHTTPRange::connectRange(uint32_t start) {
    endClient();
    bool https = (strncmp(_url, "https://", 8) == 0);
    if (https) {
        _secure.setInsecure();          // no CA store on a no-PSRAM MCU
        _secure.setTimeout(8);
        _secure.setHandshakeTimeout(10);
        _client = &_secure;
        _isSecure = true;
    } else {
        _plain.setTimeout(8);
        _client = &_plain;
    }

    _http.setReuse(false);
    _http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    _http.setTimeout(10000);
    _http.setUserAgent("EmberPlus/1.0 (M5Cardputer)");

    if (!_http.begin(*_client, _url)) {
        endClient();
        return false;
    }
    char range[32];
    snprintf(range, sizeof(range), "bytes=%u-", (unsigned)start);
    _http.addHeader("Range", range);

    _httpCode = _http.GET();
    if (_httpCode != HTTP_CODE_OK && _httpCode != HTTP_CODE_PARTIAL_CONTENT) {
        endClient();
        return false;
    }

    _contentType = _http.header("Content-Type");
    String cr = _http.header("Content-Range");   // "bytes 0-123456/7890123"
    int slash = cr.lastIndexOf('/');
    if (slash > 0) {
        _size = cr.substring(slash + 1).toInt();
    } else if (_httpCode == HTTP_CODE_OK) {
        _size = _http.getSize();                 // no range support: full file
        if (start > 0) {                         // ...but we asked for an offset
            endClient();
            return false;
        }
    }
    _buffValid = 0;
    _buffPos = 0;
    _pos = start;
    return true;
}

bool AudioFileSourceHTTPRange::open(const char* url) {
    if (!url || !url[0]) return false;
    strncpy(_url, url, sizeof(_url) - 1);
    _url[sizeof(_url) - 1] = 0;
    return connectRange(0);
}

bool AudioFileSourceHTTPRange::seek(int32_t pos, int dir) {
    uint32_t target;
    if (dir == SEEK_SET)      target = (pos < 0) ? 0 : (uint32_t)pos;
    else if (dir == SEEK_CUR) target = (uint32_t)(_pos + pos);
    else return false;
    if (target == (uint32_t)_pos) return true;
    return connectRange(target);
}

bool AudioFileSourceHTTPRange::close() {
    endClient();
    return true;
}

bool AudioFileSourceHTTPRange::isOpen() {
    return _http.connected();
}

uint32_t AudioFileSourceHTTPRange::getSize() {
    return (_size > 0) ? (uint32_t)_size : 0;
}

uint32_t AudioFileSourceHTTPRange::getPos() {
    return (uint32_t)_pos;
}

// Partial-read delivery: returns whatever is available quickly; only a
// true 0 after hardMs of silence (or a dead socket) means end-of-file.
uint32_t AudioFileSourceHTTPRange::serve(uint8_t* out, uint32_t len, uint32_t graceMs, uint32_t hardMs) {
    uint32_t copied = 0;
    uint32_t t0 = millis();

    while (copied < len) {
        if (_buffPos >= _buffValid) {
            if (!_client || !_http.connected()) return copied;
            WiFiClient* stream = _http.getStreamPtr();
            if (!stream) return copied;
            int avail = stream->available();
            if (avail <= 0) {
                uint32_t waited = millis() - t0;
                if (waited >= hardMs) return copied;
                if (copied > 0 && waited >= graceMs) return copied;
                delay(2);
                continue;
            }
            int want = (int)((_buffLen < (uint32_t)avail) ? _buffLen : (uint32_t)avail);
            int got = stream->read(_buff, want);
            if (got < 0) got = 0;
            _buffValid = (uint32_t)got;
            _buffPos = 0;
            if (got == 0) {
                uint32_t waited = millis() - t0;
                if (copied > 0 || waited >= hardMs) return copied;
                delay(2);
                continue;
            }
        }
        uint32_t take = _buffValid - _buffPos;
        uint32_t want = len - copied;
        if (take > want) take = want;
        memcpy(out + copied, _buff + _buffPos, take);
        _buffPos += take;
        copied += take;
        _pos += take;
    }
    return copied;
}

uint32_t AudioFileSourceHTTPRange::read(void* data, uint32_t len) {
    if (!data || !_buff) return 0;
    return serve((uint8_t*)data, len, 40, 2500);
}

uint32_t AudioFileSourceHTTPRange::readNonBlock(void* data, uint32_t len) {
    if (!data || !_buff) return 0;
    return serve((uint8_t*)data, len, 8, 800);
}
