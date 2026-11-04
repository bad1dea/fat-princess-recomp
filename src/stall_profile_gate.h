#pragma once
#include <cstdint>
struct FPStallProfileGate {
    uint64_t ai_since=0;
    unsigned slow=0;
    bool done=false;
    bool note(uint64_t now,double frame_ms,bool eligible) {
        if(done)return false;
        if(!eligible){ai_since=0;slow=0;return false;}
        if(!ai_since)ai_since=now;
        if(now-ai_since<120000000)return false;
        slow=((slow<<1)|(frame_ms>=50.))&1023u;
        unsigned count=0;for(unsigned bits=slow;bits;bits>>=1)count+=bits&1u;
        if(count<3)return false;
        done=true;return true;
    }
};
