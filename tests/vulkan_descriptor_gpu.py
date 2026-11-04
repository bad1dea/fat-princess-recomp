#!/usr/bin/env python3
"""Real Vulkan regression for descriptor reuse; needs cc, glslc and libvulkan."""
from pathlib import Path
import importlib.util
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("descriptor_source", root / "tests/vulkan_descriptor_cache.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
shader = r'''#version 450
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
layout(set=0,binding=0,std140) uniform A { uvec4 color; } a;
layout(set=0,binding=1,std140) uniform B { uvec4 mode; } b;
#ifdef SPLIT
layout(set=0,binding=16) uniform texture2D tex;
#else
layout(set=0,binding=16) uniform texture2D tex[16];
#endif
layout(set=0,binding=48) uniform sampler samplers[16];
layout(set=0,binding=80,rgba8) writeonly uniform image2D target;
void main(){
#ifdef SPLIT
 vec4 value=texelFetch(sampler2D(tex,samplers[0]),ivec2(0),0);
#else
 vec4 value=texelFetch(sampler2D(tex[0],samplers[0]),ivec2(0),0);
#endif
 imageStore(target,ivec2(0),vec4(a.color)/255.0*value*(b.mode.x==0u?0.5:1.0));
}
'''
# Keep artifacts when FP_VULKAN_TEST_DIR is supplied for reviewing failures.
owned = tempfile.TemporaryDirectory(prefix="fp-descriptor-gpu-")
work = Path(os.environ.get("FP_VULKAN_TEST_DIR", owned.name))
work.mkdir(parents=True, exist_ok=True)
preamble = module.preamble[:module.preamble.index("static void update(")]
preamble += "static PFN_vkUpdateDescriptorSets pvkUpdateDescriptorSets=vkUpdateDescriptorSets;\n"
(work / "descriptor-cache-under-test.h").write_text(preamble + module.state + module.helper)
(work / "shader.comp").write_text(shader)
for mode in ("normal", "split"):
    command = ["glslc", "--target-env=vulkan1.1", str(work / "shader.comp"), "-o", str(work / (mode + ".spv"))]
    if mode == "split":
        command += ["-DSPLIT=1"]
    subprocess.run(command, check=True)
subprocess.run(["cc", "-std=gnu17", "-O2", "-Wall", "-Wextra", "-I" + str(work),
                str(root / "tests/vulkan_descriptor_gpu.c"), "-lvulkan", "-lm", "-o", str(work / "test")], check=True)
pixels = []
for mode in ("0", "1"):
    result = subprocess.run([str(work / "test"), str(work / "normal.spv"), str(work / "split.spv"), mode],
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    (work / ("run-" + mode + ".log")).write_text(result.stdout)
    print(result.stdout, end="", flush=True)
    result.check_returncode()
    pixels.append([line for line in result.stdout.splitlines() if line.startswith("PIXEL")])
assert pixels[0] == pixels[1], "cached and uncached GPU pixels differ"
print("PASS: cached and uncached GPU pixel sequences match")
