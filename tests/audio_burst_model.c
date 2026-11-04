#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "../third_party/ps3recomp/libs/audio/audio_queue_budget.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
static void trial(int physical_rate,int max_blocks,int expect_starved){
 SDL_AudioStream*s=SDL_NewAudioStream(AUDIO_F32SYS,2,48000,AUDIO_F32SYS,2,physical_rate);
 if(!s){puts(SDL_GetError());exit(1);}
 const double period=256./48000.,quantum=1024./physical_rate,duration=30.;
 double t=0,next_producer=0,next_device=quantum,origin=-2*period,last=-1.;
 unsigned long long blocks=0,starved=0;int queue=0,waits=0;
 float input[512*2],output[512*2];
 while(t<duration){
  if(next_device<=next_producer){
   t=next_device;next_device+=quantum;
   // Two 512-device-frame CoreAudio callbacks in one 1024-frame I/O burst.
   for(int callback=0;callback<2;callback++){
    int remaining=512*2*sizeof(float);
    while(remaining){
     if(SDL_AudioStreamAvailable(s)==0){
      int take=queue<512?queue:512;queue-=take;
      for(int i=0;i<512*2;i++)input[i]=i<take*2?0.25f:0.f;
      if(t>2)starved+=512-take;
      if(SDL_AudioStreamPut(s,input,sizeof input)<0)exit(2);
     }
     int available=SDL_AudioStreamAvailable(s);if(!available)continue;
     int n=available<remaining?available:remaining;
     if(SDL_AudioStreamGet(s,output,n)!=n)exit(3);remaining-=n;
    }
   }
  }else{
   t=next_producer;
   double due=origin+blocks*period;
   if(t-due>0.050){origin=t-blocks*period;due=t;}
   if(due<last+0.003)due=last+0.003;
   if(t+1e-10<due){next_producer=due;continue;}
   if(queue>(max_blocks-1)*256){waits++;next_producer=t+0.001;continue;}
   queue+=256;blocks++;last=t;next_producer=t+0.000001;
  }
 }
 printf("physical=%d max_queue_blocks=%d producer_blocks_s=%.3f SDL_source_zero_frames_after_warmup=%llu waits=%d\n",physical_rate,max_blocks,blocks/t,starved,waits);
 if(expect_starved){assert(blocks/t<180 && starved>50000);}
 else{assert(fabs(blocks/t-187.5)<0.2 && starved==0);}
 SDL_FreeAudioStream(s);
}
int main(void){
 SDL_setenv("SDL_AUDIODRIVER","dummy",1);
 SDL_SetHint(SDL_HINT_AUDIO_RESAMPLING_MODE,"medium");
 if(SDL_InitSubSystem(SDL_INIT_AUDIO))return 9;
 trial(48000,4,0);trial(44100,4,1);
 const unsigned budget=fp_audio_queue_budget(48000,44100,1024,512,256,4);
 assert(budget==1536);trial(44100,budget/256,0);
 SDL_QuitSubSystem(SDL_INIT_AUDIO);
 puts("PASS: stateful 48->44.1k conversion plus measured burst budget preserves producer clock without SDL zero-fill");
}
