#!/usr/bin/env python3
"""Exercise the production bounded logger with concurrent SDL threads and a controlled clock."""
from pathlib import Path
import argparse
import os
import subprocess
import shlex
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'third_party/ps3recomp/libs/video/rsx_vulkan_backend.c').read_text()
a=source.index('/* Bounded five-second attribution.')
b=source.index('static VkPipelineCache s_pipe_cache;',a)
pre=r'''
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdarg.h>
#include "rsx_stall_trace.h"
#ifndef REAL_CLOCK
static _Atomic uint64_t clock_value=10000000;
static uint64_t mock_counter(void){return atomic_fetch_add(&clock_value,1);}
static uint64_t mock_frequency(void){return 1000000;}
#define SDL_GetPerformanceCounter mock_counter
#define SDL_GetPerformanceFrequency mock_frequency
#endif
uint32_t cellGcm_flip_request_count(void){return 42;}
'''
main=r'''
#ifndef REAL_CLOCK
static int worker(void* unused){(void)unused;for(int i=0;i<20000;i++)vk_stall_end(ST_SET_WAIT,vk_stall_start(),0);return 0;}
#endif
int main(void){
 SDL_setenv("FP_STALL_TRACE","0",1);assert(vk_stall_start()==0);
 SDL_setenv("FP_STALL_TRACE","1",1);SDL_AtomicSet(&s_stall_mode,0);
 vk_stall_cache_result(1);vk_stall_cache_result(0);assert(s_stall.shader_hits==1 && s_stall.shader_misses==1);
#ifdef REAL_CLOCK
 const uint64_t before=SDL_GetPerformanceCounter();
 for(int i=0;i<200000;i++)vk_stall_end(ST_SET_WAIT,vk_stall_start(),0);
 const double ns=1e9*(double)(SDL_GetPerformanceCounter()-before)/SDL_GetPerformanceFrequency()/200000;
 printf("Backend record including clocks and SDL try-lock: %.1f ns (desktop only)\n",ns);
#else
 SDL_Thread*threads[4];for(int i=0;i<4;i++)threads[i]=SDL_CreateThread(worker,"capture",NULL);
 for(int i=0;i<4;i++)SDL_WaitThread(threads[i],NULL);
 assert(s_stall.ops[ST_SET_WAIT].calls+(uint64_t)SDL_AtomicGet(&s_stall_dropped)==80000);
 for(unsigned kind=0;kind<RSX_STALL_KINDS;kind++)vk_stall_end(kind,vk_stall_start()-10000,1);
 vk_stall_log log={0};vk_stall_append(&log,"prefix\n");
 char huge[17000];memset(huge,'x',sizeof huge-1);huge[sizeof huge-1]=0;
 vk_stall_append(&log,"%s",huge);assert(log.truncated && !strcmp(log.data,"prefix\n"));
 // No event logging happens during record; phase changes also wait for flush.
 for(unsigned i=0;i<100;i++)rsx_vulkan_backend_stall_frame(i,i,100,45,10,5,2,3,20,i%8,0);
 assert(s_stall.phase_count==100 && s_stall.frames==100);
 atomic_fetch_add(&clock_value,6000000);
 rsx_vulkan_backend_stall_frame(100,100,100,45,10,5,2,3,20,6,0);
 assert(!s_stall.frames && !s_stall.event_count && !s_stall.phase_count);
 // Resume clears the previous window, preserving its phase marker only.
 rsx_vulkan_backend_stall_frame(101,101,1,1000,0,0,0,0,0,6,0);
 rsx_vulkan_backend_stall_frame(102,102,1,-1,0,-1,0,0,0,6,1);
 assert(s_stall.frames==0 && s_stall.phase_count==1 && s_stall.phases[0].resume==1);
 puts("PASS: production trace disabled path, concurrent bounded records, deferred phases, overflow, flush and resume reset");
#endif
}
'''

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output-dir',type=Path)
args=parser.parse_args()
flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','sdl2'],text=True))
with tempfile.TemporaryDirectory(prefix='stall-trace-') as temporary:
    directory=Path(temporary)
    fixture=directory/'trace.c'
    fixture.write_text(pre+source[a:b]+main)
    for mode in ['mock','real']:
        binary=directory/mode
        command=shlex.split(os.environ.get('CC','cc'))+['-std=gnu17','-O2','-Wall','-I'+str(root/'third_party/ps3recomp/libs/video'),str(fixture),'-o',str(binary)]+flags
        if mode=='real':command.insert(len(shlex.split(os.environ.get('CC','cc'))),'-DREAL_CLOCK')
        subprocess.run(command,check=True)
        result=subprocess.run([str(binary)],text=True,capture_output=True,check=True)
        if args.output_dir:
            args.output_dir.mkdir(parents=True,exist_ok=True)
            (args.output_dir/(mode+'.log')).write_text(result.stdout+result.stderr)
        if mode=='mock':
            assert len(result.stderr.splitlines())<=48
            assert result.stderr.count('[stall-phase]')<=8
            assert result.stderr.count('[stall-event]')<=8
            assert result.stderr.count('[stall-frame]')<=4
        print(result.stdout.strip())
