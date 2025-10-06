#include "math/math.h"
#include "filesystem/filesystem.h"
#include "syscalls/syscalls.h"
#include "cuatro.h"
#include "wav.h"
#include "tone.h"


bool play_audio_sync(audio_samples *audio, int16_t amplitude){
    int8_t lineId = mixer_open_line();
    if (lineId < 0 || lineId > MIXER_INPUTS) return false;
    audio->amplitude = amplitude;
    mixer_play_async(lineId, audio);
    do {
        sleep(100);
    } while (mixer_still_playing(lineId));
    mixer_close_line(lineId);
    return true;
}

int8_t play_audio_async(audio_samples *audio, int16_t amplitude){
    int8_t lineId = mixer_open_line();
    if (lineId < 0 || lineId > MIXER_INPUTS) return -1;
    audio->amplitude = amplitude;
    mixer_play_async(lineId, audio);
    return lineId;
}


static file mixer = { .id = 0 };  // 0 ok as filesystem ids > 256

static bool mixer_open_file(){
    if (mixer.id == 0 && FS_RESULT_SUCCESS != open_file("/dev/audio/output", &mixer)) return false;
    return true;
}

int8_t mixer_open_line(){
    if (!mixer_open_file()) return NULL;
    mixer_line_data data = { -1, {0, 0} };
    if (sizeof(mixer_line_data) != read_file(&mixer, (char*)&data, sizeof(mixer_line_data))) return NULL;
    return data.lineId;
}

void mixer_close_line(int8_t lineId){
    if (mixer_open_file()){
        mixer_command command = { lineId, MIXER_CLOSE_LINE, .value = 0 };
        write_file(&mixer, (char*)&command, sizeof(mixer_command));
    }
}

void mixer_play_async(int8_t lineId, audio_samples* audio){
    if (mixer_open_file()){
        mixer_command command = { lineId, MIXER_PLAY, .audio = audio };
        write_file(&mixer, (char*)&command, sizeof(mixer_command));
    }
}

bool mixer_still_playing(int8_t lineId){
    if (mixer_open_file()){
        mixer_line_data data = { lineId, {1, 1} };
        if (sizeof(mixer_line_data) == read_file(&mixer, (char*)&data, sizeof(mixer_line_data))){
            if (data.count[0] != 0 || data.count[1] != 0){
                // TODO: this won't work for streaming outputs (when implemented) - race condition
                return true;
            }
        }
    }
    return false;
}

bool mixer_mute(){
    if (mixer_open_file()){
        mixer_command command = { -1, MIXER_MUTE, .value=0 };
        write_file(&mixer, (char*)&command, sizeof(mixer_command));
    }
    return false; // TODO: return prev setting
}

bool mixer_unmute(){
    if (mixer_open_file()){
        mixer_command command = { -1, MIXER_UNMUTE, .value=0 };
        write_file(&mixer, (char*)&command, sizeof(mixer_command));
    }
    return true; // TODO: return prev setting
}

uint32_t mixer_set_level(int16_t level){
    if (mixer_open_file()){
        mixer_command command = { -1, MIXER_SETLEVEL, .value = (uintptr_t)level };
        write_file(&mixer, (char*)&command, sizeof(mixer_command));
    }
    return level; // TODO: return prev setting
}
