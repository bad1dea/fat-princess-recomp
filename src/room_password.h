#pragma once
#include <cstdint>
#include <cstring>

namespace fp {
// func_001EA968 hashes all 16 bytes of the eight-character UTF-16 buffer,
// alternating two CRC-CCITT registers. Padding participates in the hash.
inline uint32_t room_password_key(const uint8_t* bytes) {
    uint16_t crc[2] = {0xffff, 0xffff};
    for (unsigned i = 0; i < 16; ++i) {
        uint16_t c = uint16_t(crc[i & 1] ^ (uint16_t(bytes[i]) << 8));
        for (unsigned bit = 0; bit < 8; ++bit)
            c = uint16_t((c << 1) ^ ((c & 0x8000) ? 0x1021 : 0));
        crc[i & 1] = c;
    }
    return (uint32_t(crc[0]) << 16) | crc[1];
}

inline bool room_password_text(const uint8_t* bytes, uint32_t key, char* out, unsigned capacity) {
    if (!out || !capacity) return false;
    out[0] = 0;
    if (!key || room_password_key(bytes) != key) return false;
    unsigned n = 0;
    bool ended = false;
    char text[25] = {};
    for (unsigned i = 0; i < 8; ++i) {
        uint16_t c = uint16_t((uint16_t(bytes[i * 2]) << 8) | bytes[i * 2 + 1]);
        if (!c) { ended = true; continue; }
        if (ended || c < 0x20 || (c >= 0xd800 && c <= 0xdfff) || c == 0x7f) return false;
        if (c < 0x80) text[n++] = char(c);
        else if (c < 0x800) {
            text[n++] = char(0xc0 | (c >> 6)); text[n++] = char(0x80 | (c & 63));
        } else {
            text[n++] = char(0xe0 | (c >> 12)); text[n++] = char(0x80 | ((c >> 6) & 63));
            text[n++] = char(0x80 | (c & 63));
        }
    }
    if (!n || n >= capacity) return false;
    std::memcpy(out, text, n + 1);
    return true;
}
}
