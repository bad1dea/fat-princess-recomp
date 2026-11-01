#include "ios_stall_profile.h"
#include "stall_profile_gate.h"
#include "profile_query.h"
#include "ios_lifecycle.h"
#include <mach/mach.h>
#include <mach/arm/thread_status.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <pthread.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>
#include <string>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include <algorithm>
namespace {
using Clock=std::chrono::steady_clock;
constexpr unsigned limit=24,ticks=200;
struct Target {mach_port_t port=0;uint64_t tid=0;unsigned guest=0;char name[64]{};};
Target registry[limit];unsigned registered=0,omitted=0;
std::mutex registry_mutex;
std::atomic<bool> enabled{false},requested{false},scene_active{false};
std::string output;
uint64_t us(){return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();}
struct State {uint64_t cpu=0;int run=0;bool valid=false;};
State state(mach_port_t port){
    thread_basic_info_data_t info{};mach_msg_type_number_t n=THREAD_BASIC_INFO_COUNT;
    if(thread_info(port,THREAD_BASIC_INFO,reinterpret_cast<thread_info_t>(&info),&n)!=KERN_SUCCESS)return {};
    return {(uint64_t)(info.user_time.seconds+info.system_time.seconds)*1000000ull+
        (uint64_t)info.user_time.microseconds+(uint64_t)info.system_time.microseconds,info.run_state,true};
}
struct MachQuery {
    kern_return_t suspend(mach_port_t p)const{return thread_suspend(p);}
    kern_return_t read(mach_port_t p,arm_thread_state64_t&r,mach_msg_type_number_t&n)const {
        return thread_get_state(p,ARM_THREAD_STATE64,reinterpret_cast<thread_state_t>(&r),&n);
    }
    kern_return_t resume(mach_port_t p)const{return thread_resume(p);}
};
void capture(){
    std::fprintf(stderr,"[stall-profile] capture starting: max8s25Hz\n");
    Target targets[limit];unsigned count=0,missing=0;
    {
        std::lock_guard<std::mutex>lock(registry_mutex);missing=omitted;
        uint64_t self=0;pthread_threadid_np(nullptr,&self);
        for(unsigned i=0;i<registered;i++)
            if(registry[i].tid!=self && mach_port_mod_refs(mach_task_self(),registry[i].port,MACH_PORT_RIGHT_SEND,1)==KERN_SUCCESS)targets[count++]=registry[i];
    }
    struct Sample {uint64_t pc,lr,tid;};
    std::vector<Sample> samples;samples.reserve(count*ticks);
    State before[limit]{},after[limit]{};unsigned runs[limit][8]{};bool alive[limit];
    uint64_t overhead=0,maximum=0;unsigned suspended=0,unavailable=0;
    kern_return_t error=KERN_SUCCESS;bool stop=false;
    for(unsigned i=0;i<count;i++){
        const uint64_t start=us();before[i]=state(targets[i].port);alive[i]=true;
        const uint64_t cost=us()-start;overhead+=cost;maximum=std::max(maximum,cost);
        if(cost>2000 || overhead>80000)stop=true;
    }
    const uint64_t begin=us();
    MachQuery query;
    uint64_t next=begin+40000;
    for(unsigned tick=0;tick<ticks && !stop;tick++){
        std::this_thread::sleep_until(Clock::time_point(std::chrono::microseconds(std::min<uint64_t>(next,begin+8000000ull))));
        if(us()>=begin+8000000ull)break;
        if(!fp_ios_is_active() || !scene_active.load())break;
        for(unsigned i=0;i<count;i++){
            if(!alive[i])continue;
            if(tick%5==0){
                const uint64_t a=us();const State s=state(targets[i].port);
                const uint64_t cost=us()-a;overhead+=cost;maximum=std::max(maximum,cost);
                if(s.valid)runs[i][s.run>=0 && s.run<8?s.run:0]++;
                if(cost>2000 || overhead>80000){stop=true;break;}
            }
            arm_thread_state64_t registers{};mach_msg_type_number_t n=ARM_THREAD_STATE64_COUNT;
            const uint64_t start=us();
            // The helper contains only suspend/read/resume; all processing follows resume.
            const FPProfileQueryResult result=fp_profile_query(query,targets[i].port,registers,n);
            const uint64_t cost=us()-start;overhead+=cost;maximum=std::max(maximum,cost);
            if(!result.held){alive[i]=false;unavailable++;if(cost>2000 || overhead>80000){stop=true;break;}continue;}
            ++suspended;
            if(result.resume!=KERN_SUCCESS){error=result.resume;stop=true;break;}
            if(result.read!=KERN_SUCCESS){alive[i]=false;unavailable++;if(cost>2000 || overhead>80000){stop=true;break;}continue;}
            if(n==ARM_THREAD_STATE64_COUNT)samples.push_back({arm_thread_state64_get_pc(registers),arm_thread_state64_get_lr(registers),targets[i].tid});
            if(cost>2000 || overhead>80000){stop=true;break;}
        }
        next=us()+40000; // no catch-up burst if the sampler itself was delayed
    }
    for(unsigned i=0;i<count;i++){
        const uint64_t start=us();after[i]=state(targets[i].port);
        const uint64_t cost=us()-start;overhead+=cost;maximum=std::max(maximum,cost);
        mach_port_deallocate(mach_task_self(),targets[i].port);
    }
    const uint64_t end=us();
    std::map<std::tuple<uint64_t,uint64_t,uint64_t>,unsigned>histogram;
    for(const auto&s:samples)histogram[{s.tid,s.pc,s.lr}]++;
    std::vector<std::pair<unsigned,std::tuple<uint64_t,uint64_t,uint64_t>>>ranked;
    for(const auto&entry:histogram)ranked.push_back({entry.second,entry.first});
    std::sort(ranked.rbegin(),ranked.rend());
    const std::string temporary=output+".tmp";
    if(FILE*f=std::fopen(temporary.c_str(),"w")){
        std::fprintf(f,"\nSTALL CPU PROFILE\nbase %llx\nduration-us: %llu\nquery-total-us: %llu\nquery-max-us: %llu\nsuspended: %u\nmach-error: %d\nomitted-threads: %u\nunavailable-targets: %u\nminimum-period-us: 40000\n",
            (unsigned long long)(uintptr_t)_dyld_get_image_header(0),(unsigned long long)(end-begin),
            (unsigned long long)overhead,(unsigned long long)maximum,suspended,error,missing,unavailable);
        for(unsigned i=0;i<count;i++){
            const double cpu=before[i].valid && after[i].valid && after[i].cpu>=before[i].cpu?(after[i].cpu-before[i].cpu)/1000.:-1.;
            std::fprintf(f,"thread guest=%u tid=%llu name=%.63s cpu_ms=%.3f running_or_runnable=%u waiting=%u uninterruptible=%u stopped=%u\n",
                targets[i].guest,(unsigned long long)targets[i].tid,targets[i].name,cpu,
                runs[i][TH_STATE_RUNNING],runs[i][TH_STATE_WAITING],runs[i][TH_STATE_UNINTERRUPTIBLE],runs[i][TH_STATE_STOPPED]);
        }
        std::fprintf(f,"Samples cover wall time, including waits; sample percentages are not CPU percentages.\nCALLER COUNTS (count tid pc lr)\n");
        std::set<uint64_t>addresses;
        for(unsigned i=0;i<std::min(size_t(1024),ranked.size());i++){
            const auto&[n,key]=ranked[i];const auto&[tid,pc,lr]=key;
            std::fprintf(f,"%u %llu %llx %llx\n",n,(unsigned long long)tid,(unsigned long long)pc,(unsigned long long)lr);
            addresses.insert(pc);addresses.insert(lr);
        }
        unsigned named=0;
        for(uint64_t pc:addresses){
            if(named==128)break;Dl_info info{};
            if(dladdr(reinterpret_cast<void*>(pc),&info) && info.dli_fbase!=reinterpret_cast<const void*>(_dyld_get_image_header(0))){
                const char*name=info.dli_fname?std::strrchr(info.dli_fname,'/'):nullptr;
                std::fprintf(f,"pc %llx %.48s %.128s\n",(unsigned long long)pc,name?name+1:"?",info.dli_sname?info.dli_sname:"?");named++;
            }
        }
        std::fclose(f);std::rename(temporary.c_str(),output.c_str());
    }
    std::fprintf(stderr,"[stall-profile] samples=%zu threads=%u duration_ms=%.1f query_ms=%.3f max_query_ms=%.3f error=%d\n",samples.size(),count,(end-begin)/1000.,overhead/1000.,maximum/1000.,error);
}
}
extern "C" void fp_ios_stall_register_thread(unsigned guest,const char*name){
    if(!enabled.load())return;
    Target t{};t.guest=guest;t.port=mach_thread_self();pthread_threadid_np(nullptr,&t.tid);
    std::snprintf(t.name,sizeof t.name,"%.63s",name?name:"unknown");
    std::lock_guard<std::mutex>lock(registry_mutex);
    for(unsigned i=0;i<registered;i++)if(registry[i].guest==guest){mach_port_deallocate(mach_task_self(),registry[i].port);registry[i]=t;return;}
    if(registered<limit)registry[registered++]=t;
    else{omitted++;mach_port_deallocate(mach_task_self(),t.port);}
}
extern "C" void fp_ios_stall_profile_frame(double ms,int eligible){
    if(!enabled.load())return;scene_active.store(eligible!=0);
    static FPStallProfileGate gate;if(gate.note(us(),ms,eligible!=0))requested.store(true);
}
extern "C" void fp_ios_stall_profile_start(const char*docs){
    if(!output.empty())return;output=std::string(docs)+"/stall-profile.txt";std::remove(output.c_str());
    const char*option=std::getenv("FP_STALL_PROFILE");if(!option || std::strcmp(option,"1"))return;
    const char*audio=std::getenv("FP_AUDIO_PROFILE");
    if(std::getenv("FP_PROF") || (audio && !std::strcmp(audio,"1"))){
        std::fprintf(stderr,"[stall-profile] off while another instruction profiler is enabled\n");return;
    }
    enabled.store(true);
    std::thread([]{
        pthread_setname_np("stall-capture");
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY,0);
        for(;;){if(requested.exchange(false)){capture();return;}std::this_thread::sleep_for(std::chrono::milliseconds(100));}
    }).detach();
    std::fprintf(stderr,"[stall-profile] armed once: AI120s, 3/10 frames >=50ms, max8s25Hz, query budget80ms\n");
}
