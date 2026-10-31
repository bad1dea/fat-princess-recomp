// iOS start-up: there is no launcher script or command line, so do what run.sh does on the desktop. Game files
// live in the app's Documents folder (shared through the Files app): game/ and elf/ as in the desktop package,
// or one .zip of them dropped there (or opened with the app), which is unpacked on the next launch.
// The log goes to Documents/play.log.
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <zlib.h>

namespace {
bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

void make_dirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

uint32_t le32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
uint16_t le16(const unsigned char* p) { return uint16_t(p[0] | p[1] << 8); }

// Stored and deflated entries of an ordinary (non-Zip64) archive, which covers the game folder.
bool unzip(const std::string& zip, const std::string& dest) {
    FILE* f = std::fopen(zip.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    const long tail = size < 65557 ? size : 65557;
    std::vector<unsigned char> end(static_cast<size_t>(tail));
    std::fseek(f, size - tail, SEEK_SET);
    if (std::fread(end.data(), 1, end.size(), f) != end.size()) { std::fclose(f); return false; }
    long eocd = -1;
    for (long i = tail - 22; i >= 0; --i)
        if (le32(&end[size_t(i)]) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { std::fclose(f); return false; }
    const uint16_t count = le16(&end[size_t(eocd) + 10]);
    long cd = long(le32(&end[size_t(eocd) + 16]));
    std::vector<unsigned char> in(1 << 16), out(1 << 16);
    unsigned done = 0;
    for (uint16_t n = 0; n < count; ++n) {
        unsigned char h[46];
        std::fseek(f, cd, SEEK_SET);
        if (std::fread(h, 1, 46, f) != 46 || le32(h) != 0x02014b50) break;
        const uint16_t method = le16(h + 10), name_len = le16(h + 28), extra = le16(h + 30), comment = le16(h + 32);
        const uint32_t csize = le32(h + 20), local = le32(h + 42);
        std::string name(name_len, '\0');
        if (std::fread(name.data(), 1, name_len, f) != name_len) break;
        cd += 46 + name_len + extra + comment;
        if (name.empty() || name[0] == '/' || name.find("..") != std::string::npos || name.rfind("__MACOSX", 0) == 0)
            continue;
        const std::string target = dest + "/" + name;
        if (name.back() == '/') { make_dirs(target); continue; }
        make_dirs(target.substr(0, target.rfind('/')));
        unsigned char lh[30];
        std::fseek(f, long(local), SEEK_SET);
        if (std::fread(lh, 1, 30, f) != 30 || le32(lh) != 0x04034b50) break;
        std::fseek(f, long(local) + 30 + le16(lh + 26) + le16(lh + 28), SEEK_SET);
        FILE* o = std::fopen(target.c_str(), "wb");
        if (!o) break;
        bool ok = true;
        if (method == 0) {
            for (uint32_t left = csize; left && ok;) {
                const size_t take = left < in.size() ? left : in.size();
                ok = std::fread(in.data(), 1, take, f) == take && std::fwrite(in.data(), 1, take, o) == take;
                left -= uint32_t(take);
            }
        } else if (method == 8) {
            z_stream z{};
            ok = inflateInit2(&z, -MAX_WBITS) == Z_OK;
            int rc = Z_OK;
            for (uint32_t left = csize; ok && rc != Z_STREAM_END;) {
                if (z.avail_in == 0 && left) {
                    const size_t take = left < in.size() ? left : in.size();
                    if (std::fread(in.data(), 1, take, f) != take) { ok = false; break; }
                    z.next_in = in.data(); z.avail_in = unsigned(take); left -= uint32_t(take);
                }
                z.next_out = out.data(); z.avail_out = unsigned(out.size());
                rc = inflate(&z, Z_NO_FLUSH);
                if (rc != Z_OK && rc != Z_STREAM_END) ok = false;
                const size_t have = out.size() - z.avail_out;
                if (have && std::fwrite(out.data(), 1, have, o) != have) ok = false;
                if (!left && z.avail_in == 0 && rc == Z_OK && have == 0) ok = false;
            }
            inflateEnd(&z);
        } else {
            ok = false;
        }
        std::fclose(o);
        if (!ok) { std::fclose(f); return false; }
        ++done;
    }
    std::fclose(f);
    std::fprintf(stderr, "[ios] unpacked %u files from %s\n", done, zip.c_str());
    return done > 0;
}

bool has_zip(const std::string& docs) {
    for (const std::string& dir : {docs, docs + "/Inbox"}) {
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        bool found = false;
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".zip") == 0) { found = true; break; }
        }
        closedir(d);
        if (found) return true;
    }
    return false;
}

// Unpack the first .zip in Documents or Documents/Inbox (where "Open in" puts files) when the game is missing.
void import_zip(const std::string& docs) {
    if (exists(docs + "/elf/retail-game-verified.elf")) return;
    for (const std::string& dir : {docs, docs + "/Inbox"}) {
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        std::string found;
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".zip") == 0) { found = dir + "/" + n; break; }
        }
        closedir(d);
        if (found.empty()) continue;
        std::fprintf(stderr, "[ios] importing %s\n", found.c_str());
        if (unzip(found, docs)) std::remove(found.c_str());
        else std::fprintf(stderr, "[ios] could not unpack %s\n", found.c_str());
        return;
    }
}
}

extern "C" void fp_prof_start(void);
extern "C" void fp_ios_diagnostics_start(const char* docs);
extern "C" bool fp_ios_fetch_game(const char* docs);   // ios_setup.mm: test builds download the game with a key
extern "C" void fp_ios_setup_close(void);

extern "C" void fp_ios_prepare(int* argc, char*** argv) {
    const char* home = std::getenv("HOME");
    if (!home) return;
    static std::string docs = std::string(home) + "/Documents";
    if (chdir(docs.c_str()) != 0) return;
    const int log_fd = open((docs + "/play.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0600);
    if (log_fd >= 0) {
        std::fflush(stdout); std::fflush(stderr);
        dup2(log_fd, STDERR_FILENO); dup2(log_fd, STDOUT_FILENO);
        if (log_fd != STDOUT_FILENO && log_fd != STDERR_FILENO) close(log_fd);
        std::clearerr(stdout); std::clearerr(stderr);
    }
    setvbuf(stderr, nullptr, _IONBF, 0);
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stderr, "[ios] documents %s\n", docs.c_str());
    if (!exists(docs + "/elf/retail-game-verified.elf") && !has_zip(docs)) fp_ios_fetch_game(docs.c_str());
    import_zip(docs);
    fp_ios_setup_close();
    if (!exists(docs + "/elf/retail-game-verified.elf"))
        std::fprintf(stderr, "[ios] no game files: copy game/ and elf/ (or a zip of them) into this folder\n");
    // The same layout run.sh prepares on the desktop.
    make_dirs(docs + "/data/dev_hdd0/game");
    make_dirs(docs + "/firmware");
    make_dirs(docs + "/shader-cache");
    if (!exists(docs + "/data/PS3_GAME")) symlink("../game", (docs + "/data/PS3_GAME").c_str());
    if (!exists(docs + "/data/dev_hdd0/game/NPUA80139"))
        symlink("../../../game", (docs + "/data/dev_hdd0/game/NPUA80139").c_str());
    const std::string d = docs;
    struct { const char* key; std::string value; } env[] = {
        {"PS3_VFS_ROOT", d + "/data"}, {"PS3_HDD0_ROOT", d + "/data/dev_hdd0"}, {"PS3_NET_SOCKETS", "1"},
        {"SPURS_SYNTHETIC_CHAIN_EVENTS", "0"}, {"GCM_PRESENT_BARRIER", "1"}, {"GCM_SEMA_ACQUIRE", "0"},
        {"SPU_MFC_LOOKUP_CACHE", "1"}, {"RSX_RENDERPASS_BATCH", "1"}, {"RSX_DRAW_BATCH", "1"}, {"PS3_VERBOSE", "0"},
        {"FP_SHADER_CACHE_DIR", d + "/shader-cache"}, {"FP_STATS", "1"}, {"PS3_DEV_FLASH", d + "/firmware"},
        {"PS3_BOOT_PATH", "/dev_hdd0/game/NPUA80139/USRDIR/GAME.self"}, {"PS3_TITLE", "Fat Princess"},
        {"PS3RECOMP_VK_WINDOW", "1"}, {"PS3RECOMP_VK_GUEST_PROGRAMS", "1"}, {"AUDIO_RATE", "1"},
        {"FP_AUDIO_PUSH", "1"}, {"AUDIO_SENTINEL", "1"}, {"FP_PRESENT_TIMING", "1"}, {"FP_GAME_DIR", d + "/game/USRDIR"}, {"FP_CONFIG", d + "/settings.ini"},
        // Diagnostic image copies drain the graphics queue; sample them less often on phones.
        {"PS3RECOMP_VK_READBACK_EVERY", "300"}, {"FP_VK_SC_CHECK_MS", "0"}, {"RSX_SUBMIT_SUMMARY", "1"},
    };
    // env.txt in the documents folder (KEY=VALUE per line) overrides and adds settings, for testing over USB.
    if (FILE* f = std::fopen((docs + "/env.txt").c_str(), "r")) {
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
            const size_t eq = l.find('=');
            if (l.empty() || l[0] == '#' || eq == std::string::npos) continue;
            std::string value = l.substr(eq + 1);
            if (value.rfind("./", 0) == 0) value = docs + value.substr(1);
            setenv(l.substr(0, eq).c_str(), value.c_str(), 1);
            std::fprintf(stderr, "[ios] env.txt: %s\n", l.substr(0, eq).c_str());
        }
        std::fclose(f);
    }
    for (const auto& e : env) setenv(e.key, e.value.c_str(), 0);
    // Keep ordinary remote log captures free of CPU sampling and profile-file writes.
    // FP_PROF_ENABLE=1 plus FP_PROF opts back into instruction sampling.
    const char* profile_enabled = std::getenv("FP_PROF_ENABLE");
    if (!profile_enabled || std::strcmp(profile_enabled, "1") != 0) {
        unsetenv("FP_PROF");
        std::fprintf(stderr, "[ios] CPU sampler disabled; diagnostic log uploads remain enabled\n");
    }
    fp_ios_diagnostics_start(docs.c_str());
    fp_prof_start();
    // SDL's window only finds MoltenVK through an explicit path; it ships inside the app bundle.
    char exe[1024];
    uint32_t len = sizeof exe;
    if (_NSGetExecutablePath(exe, &len) == 0) {
        std::string dir(exe);
        dir.resize(dir.rfind('/'));
        setenv("SDL_VULKAN_LIBRARY", (dir + "/Frameworks/MoltenVK.framework/MoltenVK").c_str(), 0);
    }
    if (*argc < 2) {
        static char* args[3] = {(*argv)[0], const_cast<char*>("elf/retail-game-verified.elf"), nullptr};
        *argc = 2;
        *argv = args;
    }
}
#endif
