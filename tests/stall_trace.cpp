#include "../src/frame_timing.h"
#include "../third_party/ps3recomp/libs/video/rsx_stall_trace.h"
#include <cassert>
#include <cstdio>
#include <chrono>
int main() {
    FPFifoTiming fifo;
    fifo.resume(100,10);fifo.resume(150,20); // nested resume cannot lose work
    fifo.pause(200,40);assert(fifo.wall_total==100 && fifo.cpu_total==30);
    fifo.pause(900,90);assert(fifo.wall_total==100);
    // Presentation/pacing runs between pause and resume and is excluded.
    fifo.resume(1000,100);fifo.pause(1100,150);
    assert(fifo.wall_total==200 && fifo.cpu_total==80 && fifo.cpu_valid);
    fifo.resume(1200,0);fifo.pause(1300,0);assert(!fifo.cpu_valid);
    rsx_stall_trace t{};
    for(unsigned i=0;i<1000;i++) {
        unsigned kind=i%RSX_STALL_KINDS;
        rsx_stall_record(&t,kind,i*100+1,i*100+11,17,i,32,8);
    }
    assert(t.event_count==1000);
    uint64_t calls=0,ticks=0,bytes=0;
    for(auto&o:t.ops){calls+=o.calls;ticks+=o.ticks;bytes+=o.bytes;assert(o.maximum==10);}
    assert(calls==1000 && ticks==10000 && bytes==32000);
    // The ring contains exactly the most recent 32 records, in wrapped order.
    for(unsigned i=968;i<1000;i++) {
        auto&e=t.events[i%RSX_STALL_EVENTS];
        assert(e.start==i*100+1 && e.end==i*100+11 && e.flip==i && e.thread==17);
    }
    rsx_stall_record(&t,RSX_STALL_KINDS,1,20,0,0,0,8);
    rsx_stall_record(&t,0,20,1,0,0,0,8);
    assert(t.event_count==1000);
    rsx_stall_record(&t,0,1,8,0,0,0,8);assert(t.event_count==1000);
    for(unsigned i=0;i<1000;i++) {
        rsx_stall_frame f{};f.tick=i+1;f.frame=i;f.ms=i%100;f.draws=i;f.phase=6;
        rsx_stall_frame_add(&t,f);
    }
    assert(t.frames==1000 && t.max_ms==99 && t.total_ms==49500 && t.draws==499500 && t.max_draws==999);
    for(auto&f:t.worst)assert(f.ms==99 && f.phase==6);
    assert(t.over50==490 && t.over100==0 && t.over250==0);
    rsx_stall_frame invalid{};invalid.ms=-1;rsx_stall_frame_add(&t,invalid);assert(t.frames==1000);
    t={}; // each window starts fresh: no stale maximum or event reuse
    assert(t.frames==0 && t.event_count==0 && t.worst[0].tick==0);
    constexpr unsigned N=1000000;
    auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<N;i++)rsx_stall_record(&t,i%RSX_STALL_KINDS,i+1,i+5,1,0,0,8);
    double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/N;
    uint64_t sum=0;for(auto&o:t.ops)sum+=o.calls;assert(sum==N);
    std::printf("PASS: bounded operation ring, overflow, thresholds, per-window frame aggregates, FIFO excludes nested presentation; record %.1f ns (aggregation only)\n",ns);
}
