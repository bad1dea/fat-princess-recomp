#!/usr/bin/env python3
"""Hook the game's `echo` console command to the native menu bridge (src/menu_bridge.cpp).

The decrypted lifted sources keep `echo` as a bare return. This turns it into a call to fp_menu_echo(), then runs every
console command the bridge queued (fp_menu_reply) through the game's console, which is how the menu pages show values.
Run on the decrypted lift after the prepare step. Safe to run twice. Usage: apply_menu_bridge.py LIFT_DIR
"""
import sys
from pathlib import Path

OLD = "void func_009413C0(ppu_context* __restrict ctx) {\n        return;\n"
NEW = ("void func_009413C0(ppu_context* __restrict ctx) {\n"
       "        extern void fp_menu_echo(ppu_context*); extern int fp_menu_reply(ppu_context*, uint32_t*);\n"
       "        fp_menu_echo(ctx);\n"
       "        { const uint64_t keep_lr = ctx->lr, keep_r3 = ctx->gpr[3], keep_r4 = ctx->gpr[4], keep_r5 = ctx->gpr[5], keep_r6 = ctx->gpr[6];\n"
       "          uint32_t text = 0;\n"
       "          while (fp_menu_reply(ctx, &text)) {\n"
       "            ctx->gpr[3] = text;\n"
       "            { uint32_t tmp = vm_read32(ctx->gpr[2] + -0x42EC); float ftmp; memcpy(&ftmp, &tmp, 4); ctx->fpr[1] = ftmp; }\n"
       "            ctx->gpr[4] = keep_r4; ctx->gpr[5] = keep_r5; ctx->gpr[6] = keep_r6;\n"
       "            ctx->lr = 0x009413C4; func_001DE460(ctx); DRAIN_TRAMPOLINE(ctx);\n"
       "          }\n"
       "          ctx->lr = keep_lr; ctx->gpr[3] = keep_r3; ctx->gpr[4] = keep_r4; ctx->gpr[5] = keep_r5; ctx->gpr[6] = keep_r6; }\n"
       "        return;\n")

root = Path(sys.argv[1])
files = sorted(root.rglob("ppu_recomp_068.cpp"))
if not files:
    sys.exit("ppu_recomp_068.cpp not found under " + str(root))
path = files[0]
text = path.read_text()
if NEW in text:
    print("menu bridge already applied")
elif text.count(OLD) == 1:
    path.write_text(text.replace(OLD, NEW, 1))
    print("menu bridge applied to", path)
else:
    sys.exit("echo handler not found in " + str(path))
