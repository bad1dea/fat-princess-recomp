#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void fp_ios_stall_profile_start(const char* docs);
void fp_ios_stall_register_thread(unsigned guest,const char* name);
void fp_ios_stall_profile_frame(double ms,int eligible);
#ifdef __cplusplus
}
#endif
