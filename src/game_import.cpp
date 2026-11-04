// First-start setup: turn the player's own package and licence files into the game folder the port runs from.
//
//   import/*.pkg   the game package, and the optional expansion package
//   import/*.rap   the licence files that belong to them
//
// The package is unpacked to game/, GAME.self is decrypted to elf/retail-game-verified.elf, and a key file is written
// next to each licensed .edat. The key tables come from fp_keys.inc, which only builds that carry the private bundle have;
// without it the importer reports that it cannot run and the player copies the files in by hand (see the README).
#include "game_import.h"

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#if defined(__has_include)
#if __has_include("fp_keys.inc")
#include "fp_keys.inc"
#define FP_HAVE_KEYS 1
#endif
#endif

namespace fs = std::filesystem;

namespace {
const char kElfSha256[] = "872e7c6d3428a12c82a20e92bc94698a6231e540ebe12d50018cda9e60450be7";

#ifdef FP_HAVE_KEYS
// ---- AES (128 and 256 bit keys) ---------------------------------------------------------------------------------
struct Tables {
    uint8_t sbox[256], inv[256];
    uint32_t te[256];
    Tables() {
        uint8_t p = 1, q = 1;
        do {
            p = p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1B : 0);
            q ^= q << 1; q ^= q << 2; q ^= q << 4;
            if (q & 0x80) q ^= 0x09;
            const uint8_t x = q ^ (uint8_t)((q << 1) | (q >> 7)) ^ (uint8_t)((q << 2) | (q >> 6)) ^ (uint8_t)((q << 3) | (q >> 5)) ^ (uint8_t)((q << 4) | (q >> 4));
            sbox[p] = x ^ 0x63;
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; ++i) inv[sbox[i]] = (uint8_t)i;
        for (int i = 0; i < 256; ++i) {
            const uint8_t s = sbox[i], s2 = (uint8_t)((s << 1) ^ ((s & 0x80) ? 0x1B : 0)), s3 = s2 ^ s;
            te[i] = ((uint32_t)s2 << 24) | ((uint32_t)s << 16) | ((uint32_t)s << 8) | s3;
        }
    }
};
const Tables& tab() { static const Tables t; return t; }

uint8_t mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); b >>= 1; }
    return r;
}

struct Aes {
    uint32_t rk[60];
    int rounds = 0;
    explicit Aes(const uint8_t* key, int bytes) {
        const Tables& t = tab();
        const int nk = bytes / 4;
        rounds = nk + 6;
        for (int i = 0; i < nk; ++i) rk[i] = (uint32_t)key[4 * i] << 24 | key[4 * i + 1] << 16 | key[4 * i + 2] << 8 | key[4 * i + 3];
        uint8_t rcon = 1;
        for (int i = nk; i < 4 * (rounds + 1); ++i) {
            uint32_t w = rk[i - 1];
            if (i % nk == 0) {
                w = (w << 8) | (w >> 24);
                w = (uint32_t)t.sbox[w >> 24] << 24 | t.sbox[(w >> 16) & 255] << 16 | t.sbox[(w >> 8) & 255] << 8 | t.sbox[w & 255];
                w ^= (uint32_t)rcon << 24;
                rcon = (uint8_t)((rcon << 1) ^ ((rcon & 0x80) ? 0x1B : 0));
            } else if (nk > 6 && i % nk == 4) {
                w = (uint32_t)t.sbox[w >> 24] << 24 | t.sbox[(w >> 16) & 255] << 16 | t.sbox[(w >> 8) & 255] << 8 | t.sbox[w & 255];
            }
            rk[i] = rk[i - nk] ^ w;
        }
    }
    static uint32_t ror(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }
    void encrypt(const uint8_t* in, uint8_t* out) const {
        const Tables& t = tab();
        uint32_t s[4];
        for (int c = 0; c < 4; ++c) s[c] = ((uint32_t)in[4 * c] << 24 | in[4 * c + 1] << 16 | in[4 * c + 2] << 8 | in[4 * c + 3]) ^ rk[c];
        for (int r = 1; r < rounds; ++r) {
            uint32_t n[4];
            for (int c = 0; c < 4; ++c)
                n[c] = t.te[s[c] >> 24] ^ ror(t.te[(s[(c + 1) & 3] >> 16) & 255], 8) ^ ror(t.te[(s[(c + 2) & 3] >> 8) & 255], 16) ^
                       ror(t.te[s[(c + 3) & 3] & 255], 24) ^ rk[4 * r + c];
            std::memcpy(s, n, sizeof s);
        }
        for (int c = 0; c < 4; ++c) {
            const uint32_t w = (uint32_t)t.sbox[s[c] >> 24] << 24 | t.sbox[(s[(c + 1) & 3] >> 16) & 255] << 16 |
                               t.sbox[(s[(c + 2) & 3] >> 8) & 255] << 8 | t.sbox[s[(c + 3) & 3] & 255];
            const uint32_t v = w ^ rk[4 * rounds + c];
            out[4 * c] = v >> 24; out[4 * c + 1] = v >> 16; out[4 * c + 2] = v >> 8; out[4 * c + 3] = (uint8_t)v;
        }
    }
    void decrypt(const uint8_t* in, uint8_t* out) const {   // small inputs only: bytewise
        const Tables& t = tab();
        uint8_t s[16];
        auto add = [&](int r) { for (int c = 0; c < 4; ++c) for (int b = 0; b < 4; ++b) s[4 * c + b] ^= (uint8_t)(rk[4 * r + c] >> (24 - 8 * b)); };
        std::memcpy(s, in, 16);
        add(rounds);
        for (int r = rounds - 1; r >= 0; --r) {
            uint8_t u[16];
            for (int c = 0; c < 4; ++c) for (int b = 0; b < 4; ++b) u[4 * ((c + b) & 3) + b] = s[4 * c + b];   // inverse shift rows
            for (int i = 0; i < 16; ++i) s[i] = t.inv[u[i]];
            add(r);
            if (r) for (int c = 0; c < 4; ++c) {
                const uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
                s[4 * c] = mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9);
                s[4 * c + 1] = mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13);
                s[4 * c + 2] = mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11);
                s[4 * c + 3] = mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14);
            }
        }
        std::memcpy(out, s, 16);
    }
};

void cbc_decrypt(const uint8_t* key, int key_bytes, const uint8_t* iv, const uint8_t* in, size_t n, uint8_t* out) {
    Aes aes(key, key_bytes);
    uint8_t prev[16];
    std::memcpy(prev, iv, 16);
    for (size_t i = 0; i < n; i += 16) {
        uint8_t block[16], plain[16];
        std::memcpy(block, in + i, 16);
        aes.decrypt(block, plain);
        for (int b = 0; b < 16; ++b) out[i + b] = plain[b] ^ prev[b];
        std::memcpy(prev, block, 16);
    }
}

void add_counter(uint8_t* counter, uint64_t n) {   // 128 bit big endian
    for (int i = 15; i >= 0 && n; --i) { const uint64_t v = counter[i] + (n & 255); counter[i] = (uint8_t)v; n = (n >> 8) + (v >> 8); }
}

// XOR data with the keystream of AES-CTR starting at block `first_block` of the stream that begins at iv.
void ctr_xor(const Aes& aes, const uint8_t* iv, uint64_t first_block, size_t skip, uint8_t* data, size_t n) {
    uint8_t counter[16], ks[16];
    std::memcpy(counter, iv, 16);
    add_counter(counter, first_block);
    size_t pos = 0;
    while (pos < n) {
        aes.encrypt(counter, ks);
        for (size_t b = skip; b < 16 && pos < n; ++b) data[pos++] ^= ks[b];
        skip = 0;
        add_counter(counter, 1);
    }
}

// ---- SHA-256 ----------------------------------------------------------------------------------------------------
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    uint64_t total = 0;
    size_t fill = 0;
    static uint32_t rr(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }
    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
            0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
            0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rr(w[i - 15], 7) ^ rr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rr(w[i - 2], 17) ^ rr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (rr(e, 6) ^ rr(e, 11) ^ rr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rr(a, 2) ^ rr(a, 13) ^ rr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const uint8_t* p, size_t n) {
        total += n;
        while (n) {
            const size_t take = std::min(n, 64 - fill);
            std::memcpy(buf + fill, p, take);
            fill += take; p += take; n -= take;
            if (fill == 64) { block(buf); fill = 0; }
        }
    }
    std::string hex() {
        const uint64_t bits = total * 8;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (fill != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(len, 8);
        char out[65];
        for (int i = 0; i < 8; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
        return out;
    }
};

// ---- helpers -----------------------------------------------------------------------------------------------------
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

struct Fail { std::string text; };
[[noreturn]] void fail(const std::string& text) { throw Fail{text}; }

std::vector<uint8_t> read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) fail("Cannot read " + path.string());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A RAP file holds an obfuscated licence key; this undoes the obfuscation.
std::vector<uint8_t> rap_key(const std::vector<uint8_t>& rap) {
    if (rap.size() != 16) fail("A .rap file must be exactly 16 bytes.");
    uint8_t key[16];
    Aes(k_RAP_KEY, 16).decrypt(rap.data(), key);
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 16; ++i) key[k_RAP_PBOX[i]] ^= k_RAP_E1[k_RAP_PBOX[i]];
        for (int i = 15; i > 0; --i) key[k_RAP_PBOX[i]] ^= key[k_RAP_PBOX[i - 1]];
        int borrow = 0;
        for (int i = 0; i < 16; ++i) {
            const int pos = k_RAP_PBOX[i];
            const int value = (key[pos] - borrow) & 255;
            if (borrow != 1 || value != 255) borrow = value < k_RAP_E2[pos];
            key[pos] = (uint8_t)((value - k_RAP_E2[pos]) & 255);
        }
    }
    return std::vector<uint8_t>(key, key + 16);
}

// ---- package -----------------------------------------------------------------------------------------------------
struct PkgEntry { std::string name; uint64_t offset, size; };

struct Pkg {
    std::ifstream in;
    uint64_t data_off = 0, data_size = 0;
    uint8_t iv[16];
    std::string content_id;
    std::vector<PkgEntry> entries;
    Aes aes{k_PKG_KEY, 16};

    explicit Pkg(const fs::path& path) : in(path, std::ios::binary) {
        uint8_t h[0xA0];
        if (!in || !in.read((char*)h, sizeof h) || std::memcmp(h, "\x7f" "PKG", 4)) fail(path.filename().string() + " is not a PS3 package.");
        if (be16(h + 4) != 0x8000) fail(path.filename().string() + " is not a finalized retail package.");
        const uint32_t count = be32(h + 0x14);
        data_off = be64(h + 0x20);
        data_size = be64(h + 0x28);
        std::memcpy(iv, h + 0x70, 16);
        content_id.assign((const char*)h + 0x30, 36);
        std::vector<uint8_t> table(count * 0x20);
        decrypt(0, table.data(), table.size());
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* e = table.data() + i * 0x20;
            const uint32_t name_off = be32(e), name_size = be32(e + 4);
            if (name_size == 0 || name_size > 1024 || name_off + (uint64_t)name_size > data_size) fail("The package file table is damaged.");
            std::string name(name_size, '\0');
            decrypt(name_off, (uint8_t*)name.data(), name_size);
            entries.push_back({name, be64(e + 8), be64(e + 16)});
        }
    }
    void decrypt(uint64_t offset, uint8_t* out, size_t n) {
        in.clear();
        in.seekg((std::streamoff)(data_off + offset));
        if (!in.read((char*)out, (std::streamsize)n)) fail("The package ends early. Download it again.");
        ctr_xor(aes, iv, offset / 16, offset % 16, out, n);
    }
};

// Entry names come from the package: keep every part inside the target folder.
fs::path safe_path(const fs::path& root, const std::string& name) {
    fs::path out = root;
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find_first_of("/\\", start);
        if (end == std::string::npos) end = name.size();
        const std::string part = name.substr(start, end - start);
        if (!part.empty() && part != "." && part != ".." && part.find(':') == std::string::npos) out /= part;
        start = end + 1;
    }
    return out;
}

void extract(Pkg& pkg, const fs::path& to, bool only_usrdir) {   // an expansion adds files; its own PARAM.SFO and icons stay out
    std::vector<uint8_t> chunk(1 << 20);
    for (const auto& e : pkg.entries) {
        if (e.size == 0 || (only_usrdir && e.name.rfind("USRDIR/", 0) != 0)) continue;
        const fs::path dest = safe_path(to, e.name);
        fs::create_directories(dest.parent_path());
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) fail("Cannot write " + dest.string());
        for (uint64_t done = 0; done < e.size;) {
            const size_t n = (size_t)std::min<uint64_t>(chunk.size(), e.size - done);
            pkg.decrypt(e.offset + done, chunk.data(), n);
            out.write((const char*)chunk.data(), (std::streamsize)n);
            done += n;
        }
        if (!out) fail("The disk is full or " + dest.string() + " cannot be written.");
    }
}

// ---- SELF ---------------------------------------------------------------------------------------------------------
std::vector<uint8_t> decrypt_self(const std::vector<uint8_t>& d, const std::map<std::string, std::vector<uint8_t>>& raps) {
    if (d.size() < 0x60 || be32(d.data()) != 0x53434500) fail("GAME.self is not a SELF file.");
    const uint16_t rev = be16(d.data() + 8), type = be16(d.data() + 10);
    const uint32_t meta = be32(d.data() + 12);
    const uint64_t header_size = be64(d.data() + 16), elf_size = be64(d.data() + 24);
    if (rev != 1 || type != 1) fail("GAME.self is not the retail revision this port was made for.");
    const uint64_t app = be64(d.data() + 40), elf = be64(d.data() + 48), phdr = be64(d.data() + 56), shdr = be64(d.data() + 64);
    const uint64_t ctrl = be64(d.data() + 88), ctrl_size = be64(d.data() + 96);
    if (header_size > d.size() || elf_size > (512u << 20) || be32(d.data() + app + 12) != 8) fail("GAME.self has an unexpected layout.");
    if (std::memcmp(d.data() + elf, "\x7f" "ELF\x02\x02", 6)) fail("GAME.self does not hold a big-endian 64 bit program.");

    const uint8_t* npd = nullptr;
    for (uint64_t off = ctrl; off + 16 <= ctrl + ctrl_size;) {
        const uint32_t kind = be32(d.data() + off), size = be32(d.data() + off + 4);
        if (size < 16 || off + size > ctrl + ctrl_size) fail("GAME.self has a damaged control block.");
        if (kind == 3) npd = d.data() + off + 16;
        off += size;
    }
    uint8_t info[64];
    std::memcpy(info, d.data() + meta + 32, 64);
    if (npd) {
        const uint32_t license = be32(npd + 8);
        std::string cid((const char*)npd + 16, 48);
        cid.resize(std::strlen(cid.c_str()));
        std::vector<uint8_t> klic;
        if (license == 1 || license == 2) {
            const auto it = raps.find(cid);
            if (it == raps.end()) fail("The licence file " + cid + ".rap is missing. Put it in the import folder next to the package.");
            klic = rap_key(it->second);
        } else if (license == 3) {
            klic.assign(k_NP_KLIC_FREE, k_NP_KLIC_FREE + 16);
        } else {
            fail("GAME.self has a licence type this port does not handle.");
        }
        uint8_t layer[16], zero[16] = {};
        Aes(k_NP_KLIC_KEY, 16).decrypt(klic.data(), layer);
        cbc_decrypt(layer, 16, zero, info, 64, info);
    }
    cbc_decrypt(k_SELF_ERK, 32, k_SELF_RIV, info, 64, info);
    for (int i = 16; i < 32; ++i) if (info[i] || info[i + 32]) fail("The licence file does not match this game.");

    std::vector<uint8_t> metadata(d.begin() + meta + 96, d.begin() + header_size);
    ctr_xor(Aes(info, 16), info + 32, 0, 0, metadata.data(), metadata.size());
    const uint32_t sections = be32(metadata.data() + 12), key_count = be32(metadata.data() + 16);
    const uint64_t keys_start = 32 + (uint64_t)sections * 48;
    if (keys_start + (uint64_t)key_count * 16 > metadata.size()) fail("GAME.self metadata is damaged.");
    const uint8_t* keys = metadata.data() + keys_start;
    const uint16_t phsize = be16(d.data() + elf + 54), phnum = be16(d.data() + elf + 56), shsize = be16(d.data() + elf + 58), shnum = be16(d.data() + elf + 60);
    const uint64_t phoff = be64(d.data() + elf + 32), shoff = be64(d.data() + elf + 40);

    std::vector<uint8_t> result(elf_size);
    for (uint32_t i = 0; i < sections; ++i) {
        const uint8_t* r = metadata.data() + 32 + i * 48;
        const uint64_t start = be64(r), size = be64(r + 8);
        const uint32_t kind = be32(r + 16), index = be32(r + 20), encrypted = be32(r + 32), key_i = be32(r + 36), iv_i = be32(r + 40), compressed = be32(r + 44);
        if (start + size > d.size()) fail("GAME.self section lies outside the file.");
        std::vector<uint8_t> blob(d.begin() + start, d.begin() + start + size);
        if (encrypted == 3) {
            if (std::max(key_i, iv_i) >= key_count) fail("GAME.self key index is out of range.");
            ctr_xor(Aes(keys + key_i * 16, 16), keys + iv_i * 16, 0, 0, blob.data(), blob.size());
        }
        if (compressed == 2) {
            std::vector<uint8_t> out;
            z_stream z{};
            if (inflateInit(&z) != Z_OK) fail("zlib failed to start.");
            z.next_in = blob.data();
            z.avail_in = (uInt)blob.size();
            uint8_t buf[1 << 16];
            int rc;
            do {
                z.next_out = buf;
                z.avail_out = sizeof buf;
                rc = inflate(&z, Z_NO_FLUSH);
                if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd(&z); fail("GAME.self did not decrypt (wrong licence file?)."); }
                out.insert(out.end(), buf, buf + (sizeof buf - z.avail_out));
            } while (rc != Z_STREAM_END);
            inflateEnd(&z);
            blob.swap(out);
        }
        if (kind == 2) {
            if (index >= phnum) fail("GAME.self program index is out of range.");
            const uint8_t* ph = d.data() + phdr + (uint64_t)index * phsize;
            const uint64_t dest = be64(ph + 8), length = be64(ph + 32);
            if (length != blob.size() || dest + length > result.size()) fail("GAME.self segment size does not match.");
            std::memcpy(result.data() + dest, blob.data(), length);
        }
    }
    std::memcpy(result.data(), d.data() + elf, 64);
    std::memcpy(result.data() + phoff, d.data() + phdr, (size_t)phnum * phsize);
    if (shnum) std::memcpy(result.data() + shoff, d.data() + shdr, (size_t)shnum * shsize);
    return result;
}
#endif  // FP_HAVE_KEYS
}  // namespace

bool fp_import_possible(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir / "import", ec)) return false;
    for (const auto& e : fs::directory_iterator(dir / "import", ec))
        if (e.path().extension() == ".pkg" || e.path().extension() == ".PKG") return true;
    return false;
}

bool fp_import_game(const std::filesystem::path& dir, std::string& error) {
#ifndef FP_HAVE_KEYS
    (void)dir;
    error = "This build cannot set up the game files by itself. Copy them in by hand as the README describes.";
    return false;
#else
    try {
        std::map<std::string, std::vector<uint8_t>> raps;
        std::vector<fs::path> pkgs;
        for (const auto& e : fs::directory_iterator(dir / "import")) {
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (ext == ".rap") {
                std::string stem = e.path().stem().string();   // UP9000-NPUA80139_00-FATPRINCESS00001
                raps[stem] = read_all(e.path());
                const size_t dash = stem.find('-');
                if (dash != std::string::npos) raps[stem.substr(dash + 1)] = raps[stem];   // also by the id without the region prefix
            } else if (ext == ".pkg") {
                pkgs.push_back(e.path());
            }
        }
        const fs::path game = dir / "game";
        bool have_base = false;
        for (const auto& path : pkgs) {
            Pkg pkg(path);
            const bool base = pkg.content_id.find("NPUA80139_00-FATPRINCESS00001") != std::string::npos;
            const bool expansion = pkg.content_id.find("NPUA80139_00-FATPRINCESSEXP01") != std::string::npos;
            if (!base && !expansion) continue;   // another title's package in the same folder
            std::fprintf(stderr, "[fp-import] unpacking %s (%s)\n", path.filename().string().c_str(), pkg.content_id.c_str());
            extract(pkg, game, expansion);
            have_base = have_base || base;
        }
        if (!have_base && !fs::is_regular_file(game / "USRDIR/GAME.self"))
            fail("No Fat Princess package for the US release (NPUA80139) was found in the import folder.");

        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(game, ec)) {   // a key file beside every licensed .edat
            if (!e.is_regular_file() || e.path().extension() != ".edat") continue;
            const auto head = read_all(e.path());
            if (head.size() < 64 || std::memcmp(head.data(), "NPD\0", 4)) continue;
            const uint32_t license = be32(head.data() + 8);
            if (license != 1 && license != 2) continue;
            std::string cid((const char*)head.data() + 16, 48);
            cid.resize(std::strlen(cid.c_str()));
            const auto it = raps.find(cid);
            if (it == raps.end()) { std::fprintf(stderr, "[fp-import] no licence file for %s\n", e.path().filename().string().c_str()); continue; }
            const auto key = rap_key(it->second);
            std::ofstream(e.path().string() + ".rifkey", std::ios::binary | std::ios::trunc).write((const char*)key.data(), 16);
        }

        std::fprintf(stderr, "[fp-import] decrypting GAME.self\n");
        const auto elf = decrypt_self(read_all(game / "USRDIR/GAME.self"), raps);
        Sha256 sha;
        sha.update(elf.data(), elf.size());
        if (sha.hex() != kElfSha256) fail("The executable does not match the version this port was made for. It needs the original US release.");
        fs::create_directories(dir / "elf");
        std::ofstream(dir / "elf/retail-game-verified.elf", std::ios::binary | std::ios::trunc).write((const char*)elf.data(), (std::streamsize)elf.size());
        std::fprintf(stderr, "[fp-import] done\n");
        return true;
    } catch (const Fail& f) {
        error = f.text;
        return false;
    } catch (const std::exception& x) {
        error = std::string("Setup failed: ") + x.what();
        return false;
    }
#endif
}
