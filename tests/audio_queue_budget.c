#include "../third_party/ps3recomp/libs/audio/audio_queue_budget.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
int main(void){
 assert(fp_audio_queue_budget(48000,48000,1024,512,256,4)==1024);
 assert(fp_audio_queue_budget(48000,44100,1024,512,256,4)==1536);
 assert(fp_audio_queue_budget(48000,44100,1024,256,256,4)==1280);
 assert(fp_audio_queue_budget(48000,44100,512,512,256,4)==1024);
 assert(fp_audio_queue_budget(48000,44100,1024,4096,256,4)==8192);
 assert(fp_audio_queue_budget(48000,48000,2048,512,256,4)==2048);
 assert(fp_audio_queue_budget(48000,0,1024,512,256,4)==1024);
 assert(fp_audio_queue_budget(UINT_MAX,8000,2000,512,256,4)==16384);
 assert(fp_audio_queue_budget(48000,44100,1024,512,0,4)==0);
 puts("PASS: whole-callback source budgets, 48/44.1k rates, I/O quantum, invalid inputs and bounded capacity");
}
