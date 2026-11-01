#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void fp_ios_lifecycle_start(void);
int fp_ios_is_active(void);
unsigned fp_ios_resume_generation(void);
int fp_ios_audio_output_info(unsigned* rate,unsigned* frames);
#ifdef __cplusplus
}
#endif
