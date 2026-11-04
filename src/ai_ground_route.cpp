// Opt-in floor checks (=2), flooded routes (=3), and surface lookahead (=4).
#include "ppu_recomp.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern "C" int fp_ai_route_ground_enabled(uint32_t);
extern "C" int fp_ai_route_water_enabled(uint32_t);
extern "C" int fp_ai_route_surface_enabled(uint32_t);
// Gated synthetic probe: 1 original, 2 floor check, 3 floor and flooded-node check.
extern "C" { thread_local unsigned fp_ai_ground_probe_mode=0; }

namespace {
void call(void (*function)(ppu_context*),ppu_context* ctx) {
    function(ctx);
    while (g_trampoline_fn) { auto next=g_trampoline_fn;g_trampoline_fn=nullptr;next(ctx); }
}
float getf(uint32_t address) {
    uint32_t bits=vm_read32(address);float value;std::memcpy(&value,&bits,4);return value;
}
void putf(uint32_t address,float value) {
    uint32_t bits;std::memcpy(&bits,&value,4);vm_write32(address,bits);
}
struct Restore {
    ppu_context* ctx; ppu_context saved;
    void (*pending)(void*)=g_trampoline_fn;
    explicit Restore(ppu_context* c):ctx(c),saved(*c) { g_trampoline_fn=nullptr; }
    ~Restore() { *ctx=saved;g_trampoline_fn=pending; }
};
bool water_enabled(uint32_t owner) {
    return fp_ai_ground_probe_mode==3 || fp_ai_route_water_enabled(owner);
}
float water_limit(uint32_t owner) {
    // Character update 0x000BD800 uses player+0xB8+0x80C for TideWaterConfig.x.
    // Synthetic queries have no owner; their shipped common fixture uses 0.2.
    return fp_ai_ground_probe_mode==3?0.2f:getf(owner+0x8C4);
}
bool flooded(ppu_context* ctx,const Restore& restore,uint32_t scratch,uint32_t position,float limit) {
    *ctx=restore.saved;ctx->gpr[1]=scratch;ctx->gpr[3]=position;
    ctx->gpr[4]=scratch+0xC0;ctx->gpr[5]=scratch+0xCC;ctx->gpr[6]=scratch+0xD8;
    vm_write32(scratch+0xD8,0);
    call(func_000EBD90,ctx);
    const float depth=float(ctx->fpr[1]);
    const uint32_t fluid=vm_read32(scratch+0xD8);
    // Native character update treats any positive depth in an isLava volume
    // as lava, before applying the ordinary water threshold (0x000BE3A0).
    if (fluid && vm_read8(fluid+0xA1C)) limit=0;
    return !std::isfinite(depth) || !std::isfinite(limit) || depth>limit;
}
struct Surface { bool hit;float z;int material; };
Surface surface(ppu_context* ctx,const Restore& restore,uint32_t scratch,uint32_t position,float radius) {
    *ctx=restore.saved;ctx->gpr[1]=scratch;ctx->gpr[3]=position;
    ctx->gpr[4]=scratch+0xC0;ctx->gpr[5]=scratch+0xC4;
    ctx->gpr[6]=7;ctx->gpr[7]=scratch+0xD0;ctx->fpr[1]=radius;
    vm_write32(scratch+0xD0,0xFFFFFFFFu);
    call(func_002839F0,ctx);
    return {bool(ctx->gpr[3]&255u),getf(scratch+0xC0),int32_t(vm_read32(scratch+0xD0))};
}
bool damaging(int material) { return material==1 || material==6; }
void record_surface(uint32_t owner,const float* start,const float* goal,const Surface& next) {
    static FILE* file=[] {const char* p=std::getenv("FP_AI_NAV_SURFACE_TRACE");return p?std::fopen(p,"wx"):nullptr;}();
    static std::mutex mutex;static unsigned count=0;
    if (!file) return;
    std::lock_guard<std::mutex> lock(mutex);
    if (count++>=20000) return;
    const double epoch=std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::fprintf(file,"{\"epoch\":%.6f,\"owner\":\"%08x\",\"start\":[%.6f,%.6f,%.6f],\"goal\":[%.6f,%.6f,%.6f],\"floor_z\":%.6f,\"material\":%d}\n",
        epoch,owner,start[0],start[1],start[2],goal[0],goal[1],goal[2],next.z,next.material);
    std::fflush(file);
}
void neutral(ppu_context* ctx,const Restore& restore,uint32_t scratch,uint32_t owner) {
    const uint32_t index=vm_read32(owner+0xA8);
    for (auto axis:{func_003E4FD0,func_003E5018}) {
        *ctx=restore.saved;ctx->gpr[1]=scratch;ctx->gpr[3]=index;ctx->fpr[1]=0;
        call(axis,ctx);
    }
}
void record(const float* start,const float* goal,bool supported,unsigned rays,const float* point) {
    static FILE* file=[] { const char* p=std::getenv("FP_AI_NAV_SUPPORT_TRACE");return p?std::fopen(p,"wx"):nullptr; }();
    static std::mutex mutex;static unsigned count=0;
    if (!file) return;
    std::lock_guard<std::mutex> guard(mutex);
    if (count++>=20000) return;
    std::fprintf(file,"{\"start\":[%.5f,%.5f,%.5f],\"goal\":[%.5f,%.5f,%.5f],"
        "\"supported\":%s,\"rays\":%u,\"last\":[%.5f,%.5f,%.5f]}\n",
        start[0],start[1],start[2],goal[0],goal[1],goal[2],supported?"true":"false",rays,point[0],point[1],point[2]);
    std::fflush(file);
}

}

// Read-only predicate for the next 0.75 units of a proposed horizontal move.
// Owner zero is reserved for the gated probe and uses shipped common thresholds.
extern "C" int fp_ai_surface_segment(ppu_context* ctx,uint32_t owner,uint32_t from,uint32_t to) {
    float start[3],goal[3];
    for (unsigned j=0;j<3;++j) {
        start[j]=getf(from+j*4);goal[j]=getf(to+j*4);
        if (!std::isfinite(start[j]) || !std::isfinite(goal[j])) return 0;
    }
    const float dx=goal[0]-start[0],dy=goal[1]-start[1],length=std::hypot(dx,dy);
    if (!(length>0.001f) || !std::isfinite(length)) return 0;
    const float contact=owner?getf(owner+0x13FC):0.2f;
    const float limit=owner?getf(owner+0x8C4):0.2f;
    const float radius=owner?getf(owner+0x9D0):0.2f;
    if (!(contact>0) || !std::isfinite(contact) || !std::isfinite(limit) || radius<0 || !std::isfinite(radius)) return 0;
    Restore restore(ctx);
    const uint32_t scratch=uint32_t(restore.saved.gpr[1])-0x200;
    for (unsigned j=0;j<3;++j) putf(scratch+0x80+j*4,start[j]);
    const auto current=surface(ctx,restore,scratch,scratch+0x80,radius);
    if (!current.hit || !std::isfinite(current.z)) return 0;
    const float clearance=start[2]-current.z;
    // Same contact height used by native character material handling at 0x000BE318.
    // Preserve escape from an existing hazard and movement while airborne.
    if ((damaging(current.material) && clearance<contact) || std::fabs(clearance)>=contact) return 0;
    const float distance=std::min(length,0.75f),fraction=distance/length;
    const float next_x=start[0]+dx*fraction,next_y=start[1]+dy*fraction;
    putf(scratch+0x80,next_x);putf(scratch+0x84,next_y);putf(scratch+0x88,start[2]);
    const auto next=surface(ctx,restore,scratch,scratch+0x80,radius);
    if (!next.hit || !std::isfinite(next.z)) return 0;
    // Forecast ground contact at the next support. Current slope clearance
    // disappears on landing; carrying it forward can hide a flooded landing.
    putf(scratch+0xE0,next_x);putf(scratch+0xE4,next_y);putf(scratch+0xE8,next.z);
    const bool reject=damaging(next.material) || flooded(ctx,restore,scratch,scratch+0xE0,limit);
    if (reject) record_surface(owner,start,goal,next);
    return reject;
}

// Called after the original raised collision test at guest 0x00213A7C.
// Return its collision boolean, or 1 to make the original code use graph search.
extern "C" unsigned fp_ai_ground_shortcut(ppu_context* ctx) {
    const unsigned original=unsigned(ctx->gpr[3])&255u;
    if (original || fp_ai_ground_probe_mode==1 ||
        (fp_ai_ground_probe_mode!=2 && fp_ai_ground_probe_mode!=3 && !fp_ai_route_ground_enabled(uint32_t(ctx->gpr[22])))) return original;
    const uint32_t from=uint32_t(ctx->gpr[31]),to=uint32_t(ctx->gpr[30]);
    float start[3],goal[3];
    for (unsigned j=0;j<3;++j) {
        start[j]=getf(from+j*4);goal[j]=getf(to+j*4);
        if (!std::isfinite(start[j]) || !std::isfinite(goal[j])) return 1;
    }
    const float dx=goal[0]-start[0],dy=goal[1]-start[1],length=std::hypot(dx,dy);
    // Longer or steep segments use the native graph. The check stays bounded.
    if (length>48 || std::fabs(goal[2]-start[2])>12) return 1;
    const unsigned steps=std::max(1u,unsigned(std::ceil(length/0.75f)));
    const float nx=length>0.001f?-dy/length*0.25f:0,ny=length>0.001f?dx/length*0.25f:0;
    Restore restore(ctx);
    const uint32_t scratch=uint32_t(restore.saved.gpr[1])-0x200;
    const uint32_t owner=uint32_t(restore.saved.gpr[22]);
    const bool check_water=water_enabled(owner);
    const float limit=check_water?water_limit(owner):0;
    const uint32_t mask=uint32_t(restore.saved.gpr[29]);
    unsigned rays=0;float point[3]{};bool supported=true;
    for (unsigned i=0;i<=steps && supported;++i) for (int side:{0,-1,1}) {
        const float t=float(i)/float(steps);
        point[0]=start[0]+dx*t+float(side)*nx;
        point[1]=start[1]+dy*t+float(side)*ny;
        point[2]=start[2]+(goal[2]-start[2])*t;
        *ctx=restore.saved;ctx->gpr[1]=scratch;
        for (unsigned j=0;j<3;++j) {
            putf(scratch+0x80+j*4,point[j]+(j==2?0.5f:0.f));
            putf(scratch+0x8C+j*4,point[j]-(j==2?0.75f:0.f));
        }
        if (check_water) {
            for (unsigned j=0;j<3;++j) putf(scratch+0xB0+j*4,point[j]);
            if (flooded(ctx,restore,scratch,scratch+0xB0,limit)) { supported=false;break; }
            *ctx=restore.saved;ctx->gpr[1]=scratch;
        }
        ctx->gpr[3]=scratch+0x80;ctx->gpr[4]=scratch+0x8C;ctx->gpr[5]=mask;
        ctx->gpr[6]=scratch+0x98;ctx->gpr[7]=scratch+0xA4;
        ctx->gpr[8]=ctx->gpr[9]=ctx->gpr[10]=0;
        call(func_00313A40,ctx);++rays;
        if (!(ctx->gpr[3]&255u)) { supported=false;break; }
    }
    record(start,goal,supported,rays,point);
    return supported?0:1;
}

// After cached graph decoding at 0x002139DC, before the native success branch.
// A rejected owned result follows the original failure callback and retry path.
extern "C" unsigned fp_ai_graph_water_route(ppu_context* ctx) {
    const unsigned original=unsigned(ctx->gpr[3])&255u;
    const uint32_t owner=uint32_t(ctx->gpr[22]);
    if (!original || fp_ai_ground_probe_mode==1 || fp_ai_ground_probe_mode==2 || fp_ai_ground_probe_mode==4 || !water_enabled(owner)) return original;
    const uint32_t result_slot=uint32_t(ctx->gpr[1])+0x74;
    const uint32_t path=vm_read32(result_slot);
    if (!path) return original;
    const uint32_t begin=vm_read32(path+0x1C),end=vm_read32(path+0x20);
    if (end<begin || (end-begin)%4 || (end-begin)/4>4096) return original;
    Restore restore(ctx);
    const uint32_t scratch=uint32_t(restore.saved.gpr[1])-0x200;
    const float limit=water_limit(owner);
    bool reject=false;
    for (uint32_t at=begin;at<end;at+=4) {
        if (flooded(ctx,restore,scratch,vm_read32(at),limit)) { reject=true;break; }
    }
    if (!reject) return original;
    // Same ownership as the native result destructor: nodes are borrowed.
    if (begin) { ctx->gpr[3]=begin;call(func_002FB000,ctx); }
    ctx->gpr[3]=path;call(func_002FB000,ctx);
    vm_write32(result_slot,0);
    return 0;
}

// The tide can rise after a result was built. Check the selected steering
// target at 0x0001D6DC, then let the interpreter retire/retry that request.
extern "C" int fp_ai_route_flooded_target(ppu_context* ctx) {
    const uint32_t owner=uint32_t(ctx->gpr[31]);
    if (!fp_ai_route_water_enabled(owner)) return 0;
    const float point[3]={float(ctx->fpr[30]),float(ctx->fpr[31]),float(ctx->fpr[29])};
    for (float value:point) if (!std::isfinite(value)) return 1;
    Restore restore(ctx);
    const uint32_t scratch=uint32_t(restore.saved.gpr[1])-0x200;
    for (unsigned j=0;j<3;++j) putf(scratch+0xB0+j*4,point[j]);
    if (flooded(ctx,restore,scratch,scratch+0xB0,water_limit(owner))) return 1;
    if (!fp_ai_route_surface_enabled(owner)) return 0;
    const uint32_t character=vm_read32(owner+0xB4);
    if (!fp_ai_surface_segment(ctx,owner,character+0x14C,scratch+0xB0)) return 0;
    neutral(ctx,restore,scratch,owner);
    return 1;
}

// Only reached after native command 2's range and facing checks have requested
// the small stick correction. Suppress a hazardous correction; do not turn it
// into a path request or change the attack/aim commands.
extern "C" int fp_ai_surface_combat(ppu_context* ctx) {
    const uint32_t owner=uint32_t(ctx->gpr[31]);
    if (!fp_ai_route_surface_enabled(owner)) return 0;
    const uint32_t character=vm_read32(owner+0xB4);
    const float dx=float(ctx->fpr[1]),dy=float(ctx->fpr[30]);
    if (!std::isfinite(dx) || !std::isfinite(dy)) return 0;
    Restore restore(ctx);
    const uint32_t scratch=uint32_t(restore.saved.gpr[1])-0x200;
    putf(scratch+0xB0,getf(character+0x14C)+dx);
    putf(scratch+0xB4,getf(character+0x150)+dy);
    putf(scratch+0xB8,getf(character+0x154));
    if (!fp_ai_surface_segment(ctx,owner,character+0x14C,scratch+0xB0)) return 0;
    neutral(ctx,restore,scratch,owner);
    return 1;
}
