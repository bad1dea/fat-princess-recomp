#!/usr/bin/env python3
"""Inline the lifted code's guest stores. usage: lift_inline_stores.py <lift dir>   (idempotent)

The lifted code calls vm_write8/16/32/64 out of line for every guest store (about 20 000 call sites). The runtime's own
function already has a fast path: vm_store_plain, an ordinary mapped address, no store diagnostic armed, no null sweep,
and no SPU reservation on the line. This puts the same test, and the same store, in the header, so the common store is a
handful of instructions (ppu_inline_stores.h, included by each lifted unit only: the runtime's own files define their own accessors) and anything else still goes through the runtime function. Guarded by PPU_NO_INLINE_LOADS like
the inline loads, because the runtime defines the out-of-line accessors itself."""
import sys
from pathlib import Path
d = Path(sys.argv[1]).resolve()
out = d / "ppu_inline_stores.h"
MARK = "/* fp: inline guest stores */"
if out.exists() and MARK in out.read_text() and all('#include "ppu_inline_stores.h"' in f.read_text() for f in d.glob("ppu_recomp_*.cpp")):
    print("lift_inline_stores: already patched"); sys.exit(0)
BLOCK = MARK + r'''
#pragma once
#include "ppu_recomp.h"
#ifdef __cplusplus
extern "C" {
#endif
extern int vm_store_inline_ok;
extern int s_coh_armed;
extern unsigned char s_coh_bitmap[];
extern uint32_t g_barrier_sync_watch;
extern uint32_t g_null_sweep_hi;
#ifdef __cplusplus
}
#endif
/* The same conditions as vm_store_plain in the runtime. */
static inline int vm_store_fast_ok(uint32_t ea, uint32_t n)
{
    return __builtin_expect(vm_store_inline_ok, 1) && ea >= 0x10000u && ea < 0xE0000000u &&
           (ppu_vm_size == 0 || (uint64_t)ea + n <= ppu_vm_size) &&
           __builtin_expect(!g_barrier_sync_watch && !g_null_sweep_hi, 1) &&
           !(s_coh_armed && ((s_coh_bitmap[ea >> 10] >> ((ea >> 7) & 7)) & 1u));
}
static inline void vm_write8_inl(uint64_t a, uint8_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (vm_store_fast_ok(ea, 1)) { *(volatile uint8_t*)(vm_base_c() + ea) = v; return; }
    vm_write8(a, v);
}
static inline void vm_write16_inl(uint64_t a, uint16_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (vm_store_fast_ok(ea, 2)) {
        const uint16_t b = __builtin_bswap16(v);
        if (!(ea & 1u)) __atomic_store_n((uint16_t*)(vm_base_c() + ea), b, __ATOMIC_RELAXED); else memcpy(vm_base_c() + ea, &b, 2);
        return;
    }
    vm_write16(a, v);
}
static inline void vm_write32_inl(uint64_t a, uint32_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (vm_store_fast_ok(ea, 4)) {
        const uint32_t b = __builtin_bswap32(v);
        if (!(ea & 3u)) __atomic_store_n((uint32_t*)(vm_base_c() + ea), b, __ATOMIC_RELAXED); else memcpy(vm_base_c() + ea, &b, 4);
        return;
    }
    vm_write32(a, v);
}
static inline void vm_write64_inl(uint64_t a, uint64_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (vm_store_fast_ok(ea, 8)) {
        const uint64_t b = __builtin_bswap64(v);
        if (!(ea & 7u)) __atomic_store_n((uint64_t*)(vm_base_c() + ea), b, __ATOMIC_RELAXED); else memcpy(vm_base_c() + ea, &b, 8);
        return;
    }
    vm_write64(a, v);
}
#define vm_write8(a, v)  vm_write8_inl((a), (v))
#define vm_write16(a, v) vm_write16_inl((a), (v))
#define vm_write32(a, v) vm_write32_inl((a), (v))
#define vm_write64(a, v) vm_write64_inl((a), (v))
'''
out.write_text(BLOCK)
n = 0
for f in sorted(d.glob("ppu_recomp_*.cpp")):
    t = f.read_text()
    if '#include "ppu_inline_stores.h"' in t: continue
    inc = '#include "ppu_recomp.h"\n'
    if inc not in t: continue
    f.write_text(t.replace(inc, inc + '#include "ppu_inline_stores.h"\n', 1)); n += 1
print("lift_inline_stores: patched", n, "files")
