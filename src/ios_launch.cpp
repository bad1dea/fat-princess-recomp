// iOS start-up: there is no launcher script or command line, so do what run.sh does on the desktop. Game files
// live in the app's Documents folder (shared through the Files app): game/, elf/ and data/ as in the desktop
// package. The log goes to Documents/play.log.
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

extern "C" void fp_ios_prepare(int* argc, char*** argv) {
    const char* home = std::getenv("HOME");
    if (!home) return;
    static std::string docs = std::string(home) + "/Documents";
    if (chdir(docs.c_str()) != 0) return;
    std::freopen((docs + "/play.log").c_str(), "w", stderr);
    std::freopen((docs + "/play.log").c_str(), "a", stdout);
    setvbuf(stderr, nullptr, _IONBF, 0);
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string d = docs;
    struct { const char* key; std::string value; } env[] = {
        {"PS3_VFS_ROOT", d + "/data"}, {"PS3_HDD0_ROOT", d + "/data/dev_hdd0"}, {"PS3_NET_SOCKETS", "1"},
        {"SPURS_SYNTHETIC_CHAIN_EVENTS", "0"}, {"GCM_PRESENT_BARRIER", "1"}, {"GCM_SEMA_ACQUIRE", "0"},
        {"SPU_MFC_LOOKUP_CACHE", "1"}, {"RSX_RENDERPASS_BATCH", "1"}, {"RSX_DRAW_BATCH", "1"}, {"PS3_VERBOSE", "0"},
        {"FP_SHADER_CACHE_DIR", d + "/shader-cache"}, {"FP_STATS", "1"}, {"PS3_DEV_FLASH", d + "/firmware"},
        {"PS3_BOOT_PATH", "/dev_hdd0/game/NPUA80139/USRDIR/GAME.self"}, {"PS3_TITLE", "Fat Princess"},
        {"PS3RECOMP_VK_WINDOW", "1"}, {"PS3RECOMP_VK_GUEST_PROGRAMS", "1"}, {"AUDIO_RATE", "1"},
        {"FP_PRESENT_TIMING", "1"}, {"FP_GAME_DIR", d + "/game/USRDIR"}, {"FP_CONFIG", d + "/settings.ini"},
    };
    for (const auto& e : env) setenv(e.key, e.value.c_str(), 0);
    if (*argc < 2) {
        static char* args[3] = {(*argv)[0], const_cast<char*>("elf/retail-game-verified.elf"), nullptr};
        *argc = 2;
        *argv = args;
    }
    std::fprintf(stderr, "[ios] documents %s\n", docs.c_str());
}
#endif
