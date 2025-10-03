#include "math/math.h"
#include "filesystem/filesystem.h"
#include "syscalls/syscalls.h"
#include "cuatro.h"
#include "wav.h"

float sample_raw_wave(WAVE_TYPE type, uint32_t phase){
    switch (type) {
        case WAVE_TRIG: {
            float t = ((float)phase/(float)PHASE_MAX);
            float trig = 2*(absf(t-floor(t + 0.5f)));
            return trig;
        }
        case WAVE_SAW:
            return (float)(PHASE_MAX - phase) / (float)PHASE_MAX;
        case WAVE_SQUARE:
            return (phase < PHASE_MID) ? 0.f : 1.f;
    }
    return 0;
}

uint32_t sample_wave(WAVE_TYPE type, uint32_t phase, int16_t amplitude){
    return sample_raw_wave(type, phase) * amplitude;
}


bool play_audio_sync(audio_samples *audio, int16_t amplitude){
    intptr_t line = (intptr_t)mixer_open_line();
    if (line != NULL){
        audio->amplitude = amplitude;
        mixer_play_async(line, audio);
        do {
            // TODO: yield cpu
        } while (mixer_still_playing(line));
        mixer_close_line(line);
        return true;
    }
    return false;
}

intptr_t play_audio_async(audio_samples *audio, int16_t amplitude){
    intptr_t line = (intptr_t)mixer_open_line();
    if (line != NULL){
        audio->amplitude = amplitude;
        mixer_play_async(line, audio);
        return line;
    }
    return NULL;
}


static file mixer = { .id = 0 };  // 0 ok as filesystem ids > 256

static bool mixer_open_file(){
    if (mixer.id == 0 && FS_RESULT_SUCCESS != open_file("/dev/audio/output", &mixer)) return false;
    return true;
}

intptr_t mixer_open_line(){
    if (!mixer_open_file()) return NULL;
    mixer_line_data data = { 0, {0, 0} };
    if (sizeof(mixer_line_data) != read_file(&mixer, (char*)&data, sizeof(mixer_line_data))) return NULL;
    return data.line;
}

void mixer_close_line(intptr_t line){
    if (mixer_open_file()){
        mixer_command command = { line, MIXER_CLOSE_LINE, .value=0 };
        write_file(&mixer, (char*)&command, sizeof(mixer_command));
    }
}

void mixer_play_async(intptr_t line, audio_samples* audio){
    if (mixer_open_file()){
        mixer_command command = { line, MIXER_PLAY, .audio=audio };
        write_file(&mixer, (char*)&command, sizeof(mixer_command));
    }
}

bool mixer_still_playing(intptr_t line){
    if (mixer_open_file()){
        mixer_line_data data = { line, {1, 1} };
        if (sizeof(mixer_line_data) == read_file(&mixer, (char*)&data, sizeof(mixer_line_data))){
            if (data.count[0] != 0 || data.count[1] != 0){
                return true;
            }
        }
    }
    return false;
}

