// Native Quark archive loading and PNG rendering. No guest CPU/emulator code.
#include <SDL.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#include <zlib.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

struct Entry {
    std::string name;
    uint32_t stored, offset, size, opaque;
    bool compressed;
};

static uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

class Archive {
    std::ifstream file;
    uint64_t file_size;
    std::vector<uint8_t> bytes(size_t count) {
        std::vector<uint8_t> out(count);
        if (count && !file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(count)))
            throw std::runtime_error("truncated archive");
        return out;
    }
public:
    std::vector<Entry> entries;
    explicit Archive(const std::filesystem::path& path) : file(path, std::ios::binary),
            file_size(std::filesystem::file_size(path)) {
        const auto h = bytes(24);
        if (le32(h.data()) != 0x004b5251 || le32(h.data()+4) != 1)
            throw std::runtime_error("unsupported QRK header");
        const uint32_t count = le32(h.data()+8), data_offset = le32(h.data()+12), names_size = le32(h.data()+20);
        if (count > 1000000 || uint64_t(24)+names_size+uint64_t(count)*21 != data_offset || data_offset > file_size)
            throw std::runtime_error("invalid QRK table extent");
        const auto names = bytes(names_size);
        for (uint32_t i = 0; i < count; ++i) {
            const auto r = bytes(21);
            const uint32_t no = le32(r.data()+16);
            if (no >= names.size()) throw std::runtime_error("filename offset outside table");
            size_t end = no;
            while (end < names.size() && names[end]) ++end;
            if (end == names.size()) throw std::runtime_error("unterminated filename");
            Entry e{std::string(reinterpret_cast<const char*>(names.data()+no), end-no),
                    le32(r.data()), le32(r.data()+4), le32(r.data()+8), le32(r.data()+12), r[20] == 1};
            if (r[20] > 1 || e.offset < data_offset || uint64_t(e.offset)+e.stored > file_size)
                throw std::runtime_error("invalid asset extent: "+e.name);
            entries.push_back(e);
        }
    }
    std::vector<uint8_t> read(const Entry& e) {
        if (e.size > 256u*1024*1024) throw std::runtime_error("asset exceeds size limit");
        file.clear(); file.seekg(e.offset);
        auto data = bytes(e.stored);
        if (e.compressed) {
            std::vector<uint8_t> out(e.size);
            uLongf length = e.size;
            const auto rc = uncompress(out.data(), &length, data.data(), static_cast<uLong>(data.size()));
            if (rc != Z_OK || length != e.size) throw std::runtime_error("deflate/length error: "+e.name);
            return out;
        }
        if (data.size() != e.size) throw std::runtime_error("stored asset length mismatch");
        return data;
    }
    const Entry& find(const std::string& name) const {
        for (const auto& e : entries) if (e.name == name) return e;
        throw std::runtime_error("member not found: "+name);
    }
};

static void render(Archive& archive, const std::string& member, const std::string& bmp, bool interactive) {
    auto data = archive.read(archive.find(member));
    int iw = 0, ih = 0, channels = 0;
    if (data.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        !stbi_info_from_memory(data.data(), static_cast<int>(data.size()), &iw, &ih, &channels))
        throw std::runtime_error("invalid PNG");
    if (iw > 8192 || ih > 8192) throw std::runtime_error("image too large");
    uint8_t* decoded = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &iw, &ih, &channels, 4);
    if (!decoded) throw std::runtime_error(stbi_failure_reason());
    std::vector<uint8_t> rgba(decoded, decoded+static_cast<size_t>(iw)*static_cast<size_t>(ih)*4);
    stbi_image_free(decoded);
    if (SDL_Init(interactive ? SDL_INIT_VIDEO | SDL_INIT_EVENTS : 0)) throw std::runtime_error(SDL_GetError());
    SDL_Window* window = interactive ? SDL_CreateWindow(member.c_str(), SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, 960, 720, SDL_WINDOW_SHOWN) : nullptr;
    SDL_Surface* surface = interactive ? nullptr : SDL_CreateRGBSurfaceWithFormat(0, 960, 720, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer* renderer = interactive ? SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE) : SDL_CreateSoftwareRenderer(surface);
    if (!renderer) throw std::runtime_error(SDL_GetError());
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, iw, ih);
    if (!texture || SDL_UpdateTexture(texture, nullptr, rgba.data(), iw*4)) throw std::runtime_error(SDL_GetError());
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    bool running = true;
    int dx = 0, dy = 0;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                    case SDLK_ESCAPE: running = false; break;
                    case SDLK_LEFT: dx -= 8; break;
                    case SDLK_RIGHT: dx += 8; break;
                    case SDLK_UP: dy -= 8; break;
                    case SDLK_DOWN: dy += 8; break;
                    default: break;
                }
            }
        }
        SDL_SetRenderDrawColor(renderer, 25, 30, 40, 255); SDL_RenderClear(renderer);
        const double scale = std::min(900.0/iw, 660.0/ih);
        const int w = static_cast<int>(iw*scale), h = static_cast<int>(ih*scale);
        SDL_Rect dst{(960-w)/2+dx, (720-h)/2+dy, w, h};
        if (SDL_RenderCopy(renderer, texture, nullptr, &dst)) throw std::runtime_error(SDL_GetError());
        SDL_RenderPresent(renderer);
        if (!interactive) running = false;
        else SDL_Delay(16);
    }
    if (!bmp.empty()) {
        if (std::filesystem::exists(bmp)) throw std::runtime_error("output already exists");
        if (interactive) throw std::runtime_error("--bmp is for headless rendering");
        if (SDL_SaveBMP(surface, bmp.c_str())) throw std::runtime_error(SDL_GetError());
    }
    SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer);
    if (surface) SDL_FreeSurface(surface);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    std::cout << "rendered " << member << " (" << iw << "x" << ih << ") natively\n";
}

int main(int argc, char** argv) {
    try {
        if (argc < 3) throw std::runtime_error("usage: fp_assets archive.qrk --list | --verify | --extract member output | --render member [--bmp output.bmp] [--interactive]");
        Archive archive(argv[1]);
        const std::string action = argv[2];
        if (action == "--list") {
            for (const auto& e : archive.entries) std::cout << e.size << '\t' << e.name << '\n';
        } else if (action == "--verify") {
            uint64_t total = 0;
            for (const auto& e : archive.entries) { auto bytes = archive.read(e); total += bytes.size(); }
            std::cout << "verified " << archive.entries.size() << " assets, " << total << " unpacked bytes\n";
        } else if (action == "--extract" && argc == 5) {
            if (std::filesystem::exists(argv[4])) throw std::runtime_error("output already exists");
            auto data = archive.read(archive.find(argv[3]));
            std::ofstream out(argv[4], std::ios::binary);
            if (!out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()))) throw std::runtime_error("output write failed");
            std::cout << "loaded " << data.size() << " bytes from " << argv[3] << '\n';
        } else if (action == "--render" && argc >= 4) {
            std::string bmp; bool interactive = false;
            for (int i = 4; i < argc; ++i) {
                const std::string arg = argv[i];
                if (arg == "--bmp" && i+1 < argc) bmp = argv[++i];
                else if (arg == "--interactive") interactive = true;
                else throw std::runtime_error("unknown render option");
            }
            render(archive, argv[3], bmp, interactive);
        } else throw std::runtime_error("invalid action or arguments");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fp_assets: " << e.what() << '\n'; return 1;
    }
}
