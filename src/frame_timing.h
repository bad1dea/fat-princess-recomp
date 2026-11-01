#pragma once
#include <cstdint>
// FIFO work excludes a nested present callback (pacing, UI and presentation).
// Timestamps are supplied by the owning thread; a zero CPU reading is unavailable.
struct FPFifoTiming {
    bool active=false, cpu_valid=true;
    uint64_t wall_start=0,cpu_start=0,wall_total=0,cpu_total=0;
    void resume(uint64_t wall,uint64_t cpu) {
        if(active)return;
        active=true;wall_start=wall;cpu_start=cpu;
    }
    void pause(uint64_t wall,uint64_t cpu) {
        if(!active)return;
        active=false;
        if(wall>=wall_start)wall_total+=wall-wall_start;
        if(cpu_start && cpu>=cpu_start)cpu_total+=cpu-cpu_start;
        else cpu_valid=false;
    }
};
