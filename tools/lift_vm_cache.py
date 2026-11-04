#!/usr/bin/env python3
"""Make the lifted code's guest-memory accessors cheap. usage: lift_vm_cache.py <lift dir>   (idempotent)"""
import sys
from pathlib import Path
header = Path(sys.argv[1]).resolve() / "ppu_recomp.h"
# The lifted code reads vm_base, its inline-access limit and the gcm ref-poll word on every guest load and store. They never change
# once guest code runs, so: the ref-poll word is a constant (ppu_hle_inject_base keeps its initial 0x20000000; src/patches.cpp checks
# it at startup), and the base and limit are const-qualified globals that src/vm_cache.cpp fills in before the first guest
# instruction. Declared const here, the compiler may keep them in registers across guest stores, with no out-of-line calls.
text = header.read_text()
VM_OLD = [
 "__attribute__((const, noinline)) static uint8_t* vm_base_c(void) { return vm_base; }",
 "__attribute__((const, noinline)) static uint32_t vm_ref_ea_c(void) { return ppu_hle_inject_base + 0x2008u; }",
]
LIM_START = "__attribute__((const, noinline)) static uint64_t vm_inl_limit_c(void)\n{"
if VM_OLD[0] in text and LIM_START in text:
    text = text.replace(VM_OLD[0], '#ifdef __cplusplus\nextern "C" {\n#endif\nextern uint8_t* const g_fp_vm_base; extern const uint64_t g_fp_inl_limit;\n#ifdef __cplusplus\n}\n#endif\n'
                        'static inline uint8_t* vm_base_c(void) { return g_fp_vm_base; }', 1)
    i = text.index(LIM_START); j = text.index("\n}\n", i) + 3
    text = text[:i] + "static inline uint64_t vm_inl_limit_c(void) { return g_fp_inl_limit; }\n" + text[j:]
    text = text.replace(VM_OLD[1], "static inline uint32_t vm_ref_ea_c(void) { return 0x20002008u; }", 1)
    header.write_text(text)
print("lift_vm_cache:", "patched" if "g_fp_vm_base" in header.read_text() else "NOT patched")
