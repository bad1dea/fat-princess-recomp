// Guest-memory base and inline-access limit, published for the lifted code as const-qualified globals (see
// tools/lift_patches.py). The lifted translation units declare them const so the compiler may keep them in a register
// across guest stores; this file defines them and fills them in once the VM exists, before any guest code runs.
#include <cstdint>
extern "C" {
extern uint8_t* vm_base;
extern uint32_t ppu_vm_size;
extern int vm_inline_ok;
uint8_t* g_fp_vm_base;
uint64_t g_fp_inl_limit;
void fp_vm_cache_refresh(void) {
    const uint64_t end = (ppu_vm_size && ppu_vm_size < 0xE0000000u) ? ppu_vm_size : 0xE0000000u;
    g_fp_vm_base = vm_base;
    g_fp_inl_limit = vm_inline_ok ? end - 0x10000u : 0;
}
}
