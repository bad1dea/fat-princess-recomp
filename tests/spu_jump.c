/* Exercise the actual job-transfer macros, including a copied outer landing
 * pad and unchanged signal mask. No signal handler participates in a jump. */
#include "../third_party/ps3recomp/runtime/spu/spu_jump.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static jmp_buf current;
static volatile unsigned visited;
static void nested(void) {
    jmp_buf outer;
    memcpy(outer,current,sizeof outer);
    switch(SPU_JOB_SETJMP(current)) {
    case 0:visited|=1;SPU_JOB_LONGJMP(current,2);
    case 2:visited|=2;break;
    default:assert(0);
    }
    memcpy(current,outer,sizeof outer);
    visited|=4;
    SPU_JOB_LONGJMP(current,1);
}
int main(void) {
    sigset_t before,after;
    assert(!sigprocmask(SIG_SETMASK,NULL,&before));
    switch(SPU_JOB_SETJMP(current)) {
    case 0:nested();assert(0);break;
    case 1:visited|=8;break;
    default:assert(0);
    }
    assert(visited==15);
    assert(!sigprocmask(SIG_SETMASK,NULL,&after));
    for(int n=1;n<32;n++)assert(sigismember(&before,n)==sigismember(&after,n));
    /* Print a native timing; this is transfer overhead, never an FPS claim. */
    const clock_t begin=clock();
    for(volatile unsigned i=0;i<100000;i++)if(!SPU_JOB_SETJMP(current))SPU_JOB_LONGJMP(current,1);
    printf("PASS: nested saved landing pad, halt/restart values, unchanged signal mask; 100000 pairs %.3f CPU ms\n",1000.*(clock()-begin)/CLOCKS_PER_SEC);
}
