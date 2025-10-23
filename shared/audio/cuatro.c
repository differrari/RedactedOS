#include "math/math.h"
#ifdef KERNEL
#include "filesystem/filesystem.h"
#include "console/kio.h"
#else
#include "files/fs.h"
#endif
#include "syscalls/syscalls.h"
#include "cuatro.h"
#include "audio/mixer.h"
#include "wav.h"
#include "tone.h"

#ifdef KERNEL
#define F_OPEN open_file
#define F_READ read_file
#define F_WRITE write_file
//#define F_CLOSE close_file
#else
#define F_OPEN fopen
#define F_READ fread
#define F_WRITE fwrite
//#define F_CLOSE fclose
#endif

static file mixer = { .id = 0 };  // 0 ok as filesystem ids > 256

static inline bool mixer_open_file(){
    return (mixer.id > 0) || (FS_RESULT_SUCCESS == F_OPEN("/dev/audio/output", &mixer));
}

int8_t mixer_open_line(){
    mixer_line_data data;
    if (mixer_open_file()){
        if (mixer_read_line(-1, &data)) return data.lineId;
    }
    return -1;
}

void mixer_close_line(int8_t lineId){
    if (mixer_open_file() && lineId >= 0){
        mixer_command command = { .lineId = lineId, .command = MIXER_CLOSE_LINE };
        F_WRITE(&mixer, (char*)&command, sizeof(mixer_command));
    }
}

bool mixer_read_line(int8_t lineId, mixer_line_data* data){
    if (mixer_open_file()){
        data->lineId = lineId;
        return (sizeof(mixer_line_data) == F_READ(&mixer, (char*)data, sizeof(mixer_line_data)));
    }
    return false;
}

bool mixer_still_playing(int8_t lineId){
    if (mixer_open_file()){
        mixer_line_data data;
        data.lineId = lineId;
        if (mixer_read_line(lineId, &data)){
            if (data.dbl.buf[0].ptr != 0 || data.dbl.buf[1].ptr != 0) return true;
        }
    }
    return false;
}

bool mixer_mute(){
    if (mixer_open_file()){
        mixer_command command = { .lineId = -1, .command = MIXER_MUTE };
        F_WRITE(&mixer, (char*)&command, sizeof(mixer_command));
    }
    return false; // TODO: return prev setting
}

bool mixer_unmute(){
    if (mixer_open_file()){
        mixer_command command = { .lineId = -1, .command = MIXER_UNMUTE };
        F_WRITE(&mixer, (char*)&command, sizeof(mixer_command));
    }
    return true; // TODO: return prev setting
}

int16_t mixer_master_level(int16_t level){
    if (mixer_open_file()){
        mixer_command command = { .lineId = -1, .command = MIXER_SETLEVEL, .level = level };
        F_WRITE(&mixer, (char*)&command, sizeof(mixer_command));
    }
    return level; // TODO: return prev setting
}

int16_t mixer_line_level(int8_t lineId, int16_t level, int16_t pan){
    if (lineId < 0 || lineId > MIXER_INPUTS) return 0;
    if (mixer_open_file()){
        mixer_command command = { .lineId = lineId, .command = MIXER_SETLEVEL, .level = level, .pan = pan };
        F_WRITE(&mixer, (char*)&command, sizeof(mixer_command));
    }
    return level; // TODO: return prev setting
}

static bool mixer_play_async(int8_t lineId, audio_samples* audio, uint32_t delay_ms, AUDIO_LIFETIME life, int16_t level, int16_t pan){
    mixer_command command = { .lineId = lineId, .command = MIXER_PLAY, .audio = audio, .delay_ms = delay_ms, .life = life, .level = level, .pan = pan };
    return (sizeof(mixer_command) == F_WRITE(&mixer, (char*)&command, sizeof(mixer_command)));
}

bool audio_play_sync(audio_samples *audio, uint32_t delay_ms, AUDIO_LIFETIME life, int16_t level, int16_t pan){
    int8_t lineId = mixer_open_line();
    if (lineId < 0 || false == mixer_play_async(lineId, audio, delay_ms, life, level, pan)) return false;
    do {
        sleep(5);
    } while (mixer_still_playing(lineId));
    mixer_close_line(lineId);
    return true;
}

int8_t audio_play_async(audio_samples *audio, uint32_t delay_ms, AUDIO_LIFETIME life, int16_t level, int16_t pan){
    int8_t lineId = mixer_open_line();
    if (lineId >= 0) {
        if (false == mixer_play_async(lineId, audio, delay_ms, life, level, pan)) {
            mixer_close_line(lineId);
            lineId = -1;
        }
    }
    return lineId;
}

bool audio_update_stream(int8_t lineId, sizedptr samples){
    if (mixer_open_file()){
        mixer_command command = { .lineId = lineId, .command = MIXER_SETBUFFER, .samples = samples };
        return (sizeof(mixer_command) == F_WRITE(&mixer, (char*)&command, sizeof(mixer_command)));
    }
    return false;
}