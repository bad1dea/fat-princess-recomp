#define PPU_NO_INLINE_LOADS
#include "ppu_recomp.h"
#include <cstdint>
#include <cstdio>

extern "C" void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*));
extern "C" void fp_spurs_queues_init();

// The real ABI takes one pointer to an in-memory ELF and returns the byte
// count. The generic framework handler instead expects a path and an output
// pointer, and returns NOT_INITIALIZED. PhysX treats that error as a size.
static void overlay_table_size(ppu_context* ctx) {
    constexpr uint32_t invalid = 0x80410402;
    const uint64_t elf = static_cast<uint32_t>(ctx->gpr[3]);
    ctx->gpr[3] = invalid;
    if (elf < 0x10000 || elf + 52 > 0x100000000ull ||
        vm_read32(elf) != 0x7f454c46 || vm_read8(elf + 4) != 1 ||
        vm_read8(elf + 5) != 2 || vm_read16(elf + 18) != 23) return;
    const uint32_t phoff = vm_read32(elf + 28);
    const uint32_t phsize = vm_read16(elf + 42), count = vm_read16(elf + 44);
    if (phsize != 32 || count == 0 || count > 128 || phoff > 0x100000 ||
        elf + phoff + static_cast<uint64_t>(count) * phsize > 0x100000000ull) return;
    uint32_t physical_end = 0;
    unsigned used = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t ph = elf + phoff + i * phsize;
        if (vm_read32(ph) != 1) continue;
        const uint32_t addr = vm_read32(ph + 12), size = vm_read32(ph + 16);
        if (!size) continue;
        if (size > UINT32_MAX - addr) return;
        if (addr + size > physical_end) physical_end = addr + size;
        ++used;
    }
    // Verified against the local firmware function at libsre+0x6EA0:
    // the table occupies the physical-file-image bytes above the 256 KiB LS.
    ctx->gpr[3] = physical_end > 0x40000 ? physical_end - 0x40000 : 0;
    std::fprintf(stderr, "[fp-ovis] ELF 0x%08X: %u load segments, physical end 0x%X, table size %u\n",
                 static_cast<unsigned>(elf), used, physical_end, static_cast<unsigned>(ctx->gpr[3]));
}

extern "C" void fp_platform_init(void) {
    ps3_hle_register_ctx(0x82f294b2, "cellOvisGetOverlayTableSize", overlay_table_size);
    fp_spurs_queues_init();
}
