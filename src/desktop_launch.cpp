// Starting the game by running the program itself, with no script: on the first start, set the game up from the packages in import/; find the folder the program sits in, check that the game
// files are there (and say so plainly when they are not), lay out the folders the runtime expects, set what run.sh and
// run.bat used to export, and send the log to play.log only when keep_log is on.
//
// A launcher that already set PS3_VFS_ROOT (the run scripts, the test tools) is left alone.
#include <SDL2/SDL.h>
#include "game_import.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace {
namespace fs = std::filesystem;

fs::path program_dir() {
#ifdef _WIN32
    wchar_t buffer[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, 32768);
    return fs::path(std::wstring(buffer, n)).parent_path();
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) == 0) return fs::weakly_canonical(fs::path(buffer)).parent_path();
    return fs::current_path();
#else
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path() : exe.parent_path();
#endif
}

void put(const char* key, const std::string& value) {   // a value already in the environment wins
    if (std::getenv(key)) return;
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 0);
#endif
}

bool keep_log(const fs::path& dir) {
    if (const char* e = std::getenv("FP_KEEP_LOG")) return e[0] == '1';
    std::ifstream in(dir / "settings.ini");
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line == "keep_log=ON" || line == "keep_log=1" || line == "keep_log=on") return true;
    }
    return false;
}

void stop(const std::string& text) {
    std::fprintf(stderr, "%s\n", text.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Fat Princess", text.c_str(), nullptr);
    std::exit(1);
}

// The game folder must hold these; the decrypted executable goes in elf/.
void check_files(const fs::path& dir) {
    std::vector<std::string> missing;
    for (const char* name : {"game/PARAM.SFO", "game/USRDIR/GAME.self", "game/USRDIR/siege.qrk", "elf/retail-game-verified.elf"})
        if (!fs::is_regular_file(dir / name)) missing.push_back(name);
    if (!missing.empty()) {
        std::string text = "The game files are missing.\n\nNot found next to the program:\n";
        for (const auto& m : missing) text += "  " + m + "\n";
        text += "\nPut your game package (.pkg) and its licence file (.rap) in the import folder next to the program and start "
                "again. The README has the details.\n\n" + dir.string();
        stop(text);
    }
    std::ifstream in(dir / "elf/retail-game-verified.elf", std::ios::binary);
    char magic[4] = {};
    in.read(magic, 4);
    if (!std::memcmp(magic, "SCE\0", 4))
        stop("The executable in the elf folder is still encrypted. It must be the decrypted file (it starts with ELF).");
    if (std::memcmp(magic, "\x7f" "ELF", 4))
        stop("The file in the elf folder is not an executable. It must be the decrypted GAME.self.");
}

bool make_link(const fs::path& target, const fs::path& link) {   // directory link; target is relative to the link's folder
    std::error_code ec;
    if (fs::exists(link, ec) || fs::is_symlink(link, ec)) return true;
#ifdef _WIN32
    const fs::path absolute = fs::weakly_canonical(link.parent_path() / target, ec);
    // A symbolic link needs Developer Mode or rights; a junction needs neither.
    if (CreateSymbolicLinkW(link.c_str(), absolute.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2)) return true;
    std::wstring command = L"cmd.exe /c mklink /J \"" + link.wstring() + L"\" \"" + absolute.wstring() + L"\"";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return fs::exists(link, ec);
#else
    return symlink(target.c_str(), link.c_str()) == 0;
#endif
}
}  // namespace

namespace { std::vector<std::string> g_restart_args; }

// Starts the game again with the same arguments and environment (a new player name is only read at start-up) and ends this one.
extern "C" void fp_restart_game(void) {
    std::fflush(nullptr);
#ifdef _WIN32
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    std::wstring command;   // the saved arguments, quoted: the first start adds the executable path itself
    for (const auto& a : g_restart_args) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, a.c_str(), -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, a.c_str(), -1, w.data(), n);
        command += (command.empty() ? L"\"" : L" \"") + w + L"\"";
    }
    if (!command.empty() && CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        ExitProcess(0);
    }
#else
    std::vector<char*> args;
    for (auto& a : g_restart_args) args.push_back(a.data());
    args.push_back(nullptr);
    if (!args.empty() && args[0]) {
#ifdef __APPLE__
        char buffer[4096];
        uint32_t size = sizeof buffer;
        const std::string exe = _NSGetExecutablePath(buffer, &size) == 0 ? buffer : std::string(args[0]);
#else
        std::error_code ec;
        const std::string exe = fs::read_symlink("/proc/self/exe", ec).string();
#endif
        if (!exe.empty()) execv(exe.c_str(), args.data());
    }
#endif
    // Could not start again: leave the game as it is; the name applies at the next start.
}

extern "C" void fp_desktop_prepare(int* argc, char*** argv) {
    for (int i = 0; i < *argc; ++i) g_restart_args.push_back((*argv)[i]);
    if (std::getenv("PS3_VFS_ROOT")) return;   // started by a script or a test tool
    const fs::path dir = program_dir();
    std::error_code ec;
    fs::current_path(dir, ec);
    if (!fs::is_regular_file(dir / "elf/retail-game-verified.elf") && fp_import_possible(dir)) {   // first start: set the game up from the packages
        std::fprintf(stderr, "First start: setting up the game files from the import folder.\n");
        std::string error;
        if (!fp_import_game(dir, error)) stop("The game files could not be set up.\n\n" + error);
    }
    check_files(dir);
    const std::string d = dir.string();
    fs::create_directories(dir / "data/dev_hdd0/game", ec);
    fs::create_directories(dir / "firmware", ec);
    fs::create_directories(dir / "shader-cache", ec);
    if (!make_link("../game", dir / "data/PS3_GAME") || !make_link("../../../game", dir / "data/dev_hdd0/game/NPUA80139"))
        stop("The game folders could not be linked. On Windows, turn on Developer Mode or run the program once as administrator.");
    // The log goes nowhere unless keep_log is on: no file to clean up, and no time spent writing it.
    if (keep_log(dir)) {
        std::freopen((dir / "play.log").string().c_str(), "w", stderr);
        std::freopen((dir / "play.log").string().c_str(), "a", stdout);
    } else {
#ifdef _WIN32
        std::freopen("NUL", "w", stderr);
        std::freopen("NUL", "w", stdout);
#else
        std::freopen("/dev/null", "w", stderr);
        std::freopen("/dev/null", "w", stdout);
#endif
    }
    put("PS3_VFS_ROOT", d + "/data");
    put("PS3_HDD0_ROOT", d + "/data/dev_hdd0");
    put("PS3_NET_SOCKETS", "1");
    put("SPURS_SYNTHETIC_CHAIN_EVENTS", "0");
    put("GCM_PRESENT_BARRIER", "1");
    put("GCM_SEMA_ACQUIRE", "0");
    put("SPU_MFC_LOOKUP_CACHE", "1");
    put("RSX_RENDERPASS_BATCH", "1");
    put("RSX_DRAW_BATCH", "1");
    put("PS3_VERBOSE", "0");
    put("FP_SHADER_CACHE_DIR", d + "/shader-cache");
    put("FP_STATS", "0");
    put("PS3_DEV_FLASH", d + "/firmware");
    put("PS3_BOOT_PATH", "/dev_hdd0/game/NPUA80139/USRDIR/GAME.self");
    put("PS3_TITLE", "Fat Princess - Native");
    put("PS3RECOMP_VK_WINDOW", "1");
    put("PS3RECOMP_VK_GUEST_PROGRAMS", "1");
    put("AUDIO_RATE", "1");
    put("FP_GAME_DIR", d + "/game/USRDIR");
    put("FP_CONFIG", d + "/settings.ini");
    if (*argc < 2) {
        static std::string elf = (dir / "elf/retail-game-verified.elf").string();
        static char* args[3] = {(*argv)[0], elf.data(), nullptr};
        *argc = 2;
        *argv = args;
        g_restart_args = {args[0], elf};
    }
}
