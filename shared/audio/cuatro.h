#pragma once

#include "types.h"

typedef int16_t audio_sample_t;

typedef enum AUDIO_LIFETIME {
    AUDIO_OFF = 0,
    AUDIO_ONESHOT,          // play samples once
    AUDIO_ONESHOT_FREE,     // play samples once then free_sized() their memory
    AUDIO_LOOP,             // play samples repeatedly - with delay, if set
    AUDIO_STREAM,           // assign samples to available dblbuf entry
} AUDIO_LIFETIME;

typedef enum PAN_RANGE {
    PAN_LEFT = -32767,
    PAN_HALFLEFT = -16383,
    PAN_CENTRE = 0,
    PAN_CENTER = 0,
    PAN_HALFRIGHT = 16383,
    PAN_RIGHT = 32767,
} PAN_RANGE;

typedef struct audio_samples {
    sizedptr samples;
    uint32_t smpls_per_channel;  // TODO: eliminate.
    uint8_t channels;
} audio_samples;

typedef struct mixer_dblbuf {
    sizedptr buf[2];
    uint8_t buf_idx;
} mixer_dblbuf;

typedef struct mixer_line_data {
    int8_t  lineId;
    mixer_dblbuf dbl;
} mixer_line_data;


#define MIXER_INPUTS        6
#define AUDIO_LEVEL_MAX     INT16_MAX
#define SIGNAL_LEVEL_MAX    32767
#define SIGNAL_LEVEL_MIN    (-32767)

#ifdef __cplusplus
extern "C" {
#endif

bool audio_play_sync(audio_samples *audio, uint32_t delay_ms, AUDIO_LIFETIME life, int16_t level, int16_t pan);
int8_t audio_play_async(audio_samples *audio, uint32_t delay_ms, AUDIO_LIFETIME life, int16_t level, int16_t pan);
bool audio_update_stream(int8_t lineId, sizedptr samples);

int8_t mixer_open_line();
void mixer_close_line(int8_t lineId);
bool mixer_read_line(int8_t lineId, mixer_line_data* data);
bool mixer_still_playing(int8_t lineId);
bool mixer_mute();
bool mixer_unmute();
int16_t mixer_master_level(int16_t level);
int16_t mixer_line_level(int8_t lineId, int16_t level, int16_t pan);

#ifdef __cplusplus
}
#endif