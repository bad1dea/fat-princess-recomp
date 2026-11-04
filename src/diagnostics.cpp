#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include "ppu_recomp.h"

// Recovered command arguments: 0x800 contains offsets into the token buffer;
// 0x900 is the token count. Opt-in observation only, no guest state changes.
void fp_trace_command(ppu_context* ctx, unsigned site) {
    static const bool enabled = std::getenv("FP_TRACE_COMMANDS") != nullptr;
    if (!enabled) return;
    const uint32_t args = static_cast<uint32_t>(ctx->gpr[4]);
    if (args < 0x10000 || args > 0xdffff000) return;
    const uint32_t count = vm_read32(args + 0x900);
    if (count > 64) return;
    char message[2048];
    size_t used = 0;
    for (uint32_t i = 0; i < count && used + 2 < sizeof(message); ++i) {
        uint32_t offset = vm_read32(args + 0x800 + 4 * i);
        if (offset >= 0x800) break;
        if (i) message[used++] = ' ';
        for (uint32_t j = offset; j < 0x800 && used + 1 < sizeof(message); ++j) {
            unsigned char c = vm_read8(args + j);
            if (!c) break;
            message[used++] = c >= 32 && c < 127 ? c : '?';
        }
    }
    message[used] = 0;
    std::fprintf(stderr, "[fp-command] site=%08X lr=%08X argc=%u %s\n",
                 site, static_cast<uint32_t>(ctx->lr), count, message);
}

void fp_trace_map_objects(ppu_context* ctx) {
    if (!std::getenv("FP_TRACE_COMMANDS")) return;
    const uint32_t sp = static_cast<uint32_t>(ctx->gpr[1]);
    for (unsigned offset : {0x70u, 0x90u, 0x80u}) {
        const uint32_t begin = vm_read32(sp + offset + 4);
        const uint32_t end = vm_read32(sp + offset + 8);
        std::fprintf(stderr, "[fp-map] list=+%02X begin=%08X end=%08X count=%u\n",
                     offset, begin, end, (end - begin) / 4);
        if (begin < 0x10000 || end < begin || end > 0xe0000000 || end - begin > 16384) continue;
        for (uint32_t at = begin; at < end; at += 4) {
            const uint32_t object = vm_read32(at);
            if (object < 0x10000 || object > 0xdfffff00) continue;
            std::fprintf(stderr, "[fp-map-object] list=%02X index=%u object=%08X "
                         "words=%08X %08X %08X %08X %08X %08X\n",
                         offset, (at - begin) / 4, object, vm_read32(object),
                         vm_read32(object+4), vm_read32(object+8), vm_read32(object+12),
                         vm_read32(object+16), vm_read32(object+20));
        }
    }
}

// Observe a guest-requested flip without changing the guest or its rendering.
// The centre pixel is a diagnostic, not evidence of shader/visual correctness.
#if !defined(_WIN32) && !defined(__APPLE__)
#if defined(PS3RECOMP_RSX_VULKAN)
extern "C" uint32_t rsx_vulkan_backend_readback_center(void);
extern "C" uint32_t rsx_vulkan_backend_guest_draws(void);
#else
extern "C" uint32_t rsx_null_backend_readback_center(void);
#endif
#endif
extern "C" void fp_report_game_frame(unsigned count) {
    if (count > 3 && count % 300 != 0) return;
#if !defined(_WIN32) && !defined(__APPLE__)
#if defined(PS3RECOMP_RSX_VULKAN)
    std::fprintf(stderr, "[fp-frame] guest frame %u presented; Vulkan centre=0x%08X guest shader draws=%u\n",
                 count, rsx_vulkan_backend_readback_center(), rsx_vulkan_backend_guest_draws());
#else
    std::fprintf(stderr, "[fp-frame] guest frame %u presented; null centre=0x%08X\n",
                 count, rsx_null_backend_readback_center());
#endif
#else
    std::fprintf(stderr, "[fp-frame] guest frame %u presented\n", count);
#endif
}

// WO parser name buffer and factory return, recovered from 0x00206678.
void fp_trace_world_object(ppu_context* ctx, unsigned site) {
    if (!std::getenv("FP_TRACE_COMMANDS")) return;
    char name[256];
    for (unsigned i = 0; i < sizeof(name); ++i) {
        name[i] = vm_read8(static_cast<uint32_t>(ctx->gpr[1]) + 0x3f0 + i);
        if (!name[i]) break;
    }
    name[255] = 0;
    std::fprintf(stderr, "[fp-wo] site=%08X name=%s result=%08X\n",
                 site, name, static_cast<uint32_t>(ctx->gpr[3]));
}

void fp_trace_factory_failure(ppu_context* ctx, unsigned site) {
    if (!std::getenv("FP_TRACE_COMMANDS")) return;
    std::fprintf(stderr, "[fp-factory] site=%08X r3=%08X r29=%08X scene=%08X deferred=%u ctr=%08X lr=%08X\n",
        site, static_cast<uint32_t>(ctx->gpr[3]), static_cast<uint32_t>(ctx->gpr[29]),
        vm_read32(0xF036B4), vm_read8(0xF018C8),
        static_cast<uint32_t>(ctx->ctr), static_cast<uint32_t>(ctx->lr));
}
