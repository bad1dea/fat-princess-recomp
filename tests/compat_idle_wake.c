#define _GNU_SOURCE
#include "../third_party/ps3recomp/runtime/platform/win32_compat.h"
#include <assert.h>
#include <stdio.h>
#define N 4
static SRWLOCK lock=SRWLOCK_INIT;
static CONDITION_VARIABLE cv=CONDITION_VARIABLE_INIT;
static int epoch,done,entered;
static DWORD WINAPI worker(void* unused){
 (void)unused;AcquireSRWLockExclusive(&lock);entered++;WakeAllConditionVariable(&cv);ReleaseSRWLockExclusive(&lock);
 for(int round=1;round<=1000;round++){
  AcquireSRWLockExclusive(&lock);
  while(epoch<round)assert(SleepConditionVariableSRW(&cv,&lock,5000,0));
  done++;WakeAllConditionVariable(&cv);ReleaseSRWLockExclusive(&lock);
 }
 return 0;
}
int main(){
 for(int i=0;i<10000;i++){WakeConditionVariable(&cv);WakeAllConditionVariable(&cv);}
 assert(cv==NULL); // idle notifications must not create a future wake
 AcquireSRWLockExclusive(&lock);
 assert(!SleepConditionVariableSRW(&cv,&lock,20,0));
 ReleaseSRWLockExclusive(&lock);
 for(int i=0;i<10000;i++){WakeConditionVariable(&cv);WakeAllConditionVariable(&cv);}
 AcquireSRWLockExclusive(&lock);
 assert(!SleepConditionVariableSRW(&cv,&lock,20,0));
 ReleaseSRWLockExclusive(&lock);
 HANDLE threads[N];for(int i=0;i<N;i++)threads[i]=CreateThread(NULL,0,worker,NULL,0,NULL);
 AcquireSRWLockExclusive(&lock);
 while(entered<N)assert(SleepConditionVariableSRW(&cv,&lock,5000,0));
 for(int round=1;round<=1000;round++){
  epoch=round;WakeAllConditionVariable(&cv);
  while(done<N*round)assert(SleepConditionVariableSRW(&cv,&lock,5000,0));
 }
 ReleaseSRWLockExclusive(&lock);
 assert(WaitForMultipleObjects(N,threads,TRUE,5000)==WAIT_OBJECT_0);
 for(int i=0;i<N;i++)CloseHandle(threads[i]);
 puts("CV: 40000 idle wakes, no future replay, 4000 worker handshakes, broadcast to all: passed");
}
