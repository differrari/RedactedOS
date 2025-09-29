#pragma once

typedef struct wav_data {
    uint32_t channels;
    uint32_t smpls_per_channel;
    sizedptr samples;
} wav_data;


#ifdef __cplusplus
extern "C" {
#endif

bool wav_load_as_int16(const char*path, wav_data *wav);

#ifdef __cplusplus
}
#endif