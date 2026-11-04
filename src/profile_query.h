#pragma once
struct FPProfileQueryResult {int suspend=0,read=0,resume=0;bool held=false;};
// Ops.read is the native register query. Resume also runs if a future reader throws.
template<class Ops,class Port,class Registers,class Count>
FPProfileQueryResult fp_profile_query(Ops&ops,Port port,Registers&registers,Count&count) {
    FPProfileQueryResult result;result.suspend=ops.suspend(port);
    if(result.suspend)return result;
    result.held=true;
    struct Resume {Ops&ops;Port port;bool armed=true;~Resume(){if(armed)ops.resume(port);}} guard{ops,port};
    result.read=ops.read(port,registers,count);
    result.resume=ops.resume(port);guard.armed=false;
    return result;
}
