#!/usr/bin/env python3
"""Compile SPU sources with a target created in a child directory, as in the game."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "CMakeLists.txt").read_text()
blocks = []
for group in ("FP_SPU_SOURCES", "FP_OVERLAY_SOURCES"):
    match = re.search(r"set_source_files_properties\(\$\{" + group + r"\}.*?\)\s*", source, re.S)
    assert match, f"missing source properties for {group}"
    blocks.append(match.group())
with tempfile.TemporaryDirectory(prefix="fp-spu-flags-") as tmp:
    project = Path(tmp)
    (project / "engine").mkdir()
    (project / "runtime/spu").mkdir(parents=True)
    (project / "runtime/spu/flag_marker.h").write_text("#define FP_INCLUDE_FLAG 1\n")
    (project / "engine/CMakeLists.txt").write_text("add_executable(MyGameRecomp main.c)\n")
    (project / "engine/main.c").write_text("int task(void); int overlay(void); int main(void){return task()+overlay();}\n")
    check = """#include "flag_marker.h"
#ifndef SPU_LS_LEAN
#error SPU_LS_LEAN did not reach the target
#endif
#ifndef SPU_USE_DISPATCH_MT
#error SPU_USE_DISPATCH_MT did not reach the target
#endif
int FUNCTION(void){return FP_INCLUDE_FLAG-1;}
"""
    for name in ("task", "overlay"):
        (project / (name + ".c")).write_text(check.replace("FUNCTION", name))
    cmake = """cmake_minimum_required(VERSION 3.20)
project(SPUCompileFlags C)
set(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
add_subdirectory(engine)
set(FP_SPU_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/task.c")
set(FP_OVERLAY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/overlay.c")
target_sources(MyGameRecomp PRIVATE ${FP_SPU_SOURCES} ${FP_OVERLAY_SOURCES})
"""
    (project / "CMakeLists.txt").write_text(cmake + "\n".join(blocks))
    subprocess.run(["cmake", "-S", str(project), "-B", str(project / "build")], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", str(project / "build")], check=True, stdout=subprocess.DEVNULL)
print("SPU flags: include path, lean access and indirect dispatch reached both source groups")
