#pragma once


#ifdef __cplusplus
extern "C" {
#endif

bool wav_load_as_int16(const char*path, audio_samples *audio);

#ifdef __cplusplus
}
#endif