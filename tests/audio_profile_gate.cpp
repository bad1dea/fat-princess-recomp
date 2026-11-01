#include "../src/audio_profile_gate.h"
#include <cassert>
#include <cstdio>
int main() {
    FPAudioProfileGate g;
    assert(!g.note(14000,100));
    assert(!g.note(14000,200));
    assert(!g.note(0,300)); // missing CPU clock must not trigger
    assert(!g.note(14000,400));
    assert(!g.note(10667,500)); // exactly at budget breaks streak
    assert(!g.note(14000,600));
    assert(!g.note(14000,700));
    assert(g.note(14000,800));
    // Prolonged overload during the cooldown stays bounded.
    for (unsigned t=0;t<10000;++t) assert(!g.note(14000,900+t*100));
    assert(g.note(14000,800+FPAudioProfileGate::cooldown_us));
    for (unsigned t=0;t<10000;++t)
        assert(!g.note(14000,800+2*FPAudioProfileGate::cooldown_us+t));
    assert(g.captures==2);
    std::puts("audio profile gate: passed threshold, streak, invalid clock, cooldown, capture limit");
}
