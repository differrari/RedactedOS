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

uint32_t sample_wave(WAVE_TYPE type, uint32_t phase, uint32_t amplitude){
    return sample_raw_wave(type, phase) * amplitude;
}

static void play_int16_async(mixer_input *line, audio_samples* audio, uint32_t amplitude){
    line->channels = audio->channels;
    line->buf[0].left_level = amplitude;
    line->buf[0].right_level = amplitude;
    line->buf[0].sample_count = audio->smpls_per_channel * audio->channels;
    line->buf[0].samples = (int16_t*)audio->samples.ptr;  // this must be last mutation of 'line'.
}

bool play_audio_sync(audio_samples *audio, uint32_t amplitude){
    file mixin;
    if (FS_RESULT_SUCCESS == open_file("/dev/audio/output", &mixin)){
        mixer_input* line = NULL;
        if (sizeof(mixer_input*) == read_file(&mixin, (char*)&line, sizeof(mixer_input*))){
            play_int16_async(line, audio, amplitude);
            while (line->buf[0].samples != NULL){
                // TODO: yield cpu
            }
            // close_file(&mixin);
            mixer_command cmd = { MIXER_CLOSE_LINE, 0 };
            write_file(&mixin, (char*)&cmd, sizeof(mixer_command));
            free((char*)audio->samples.ptr, audio->samples.size);
            return true;
        }
    }
    return false;
}

bool play_audio_async(audio_samples *audio, uint32_t amplitude, file *mixin){
    mixer_input* line = NULL;
    if (sizeof(mixer_input*) == read_file(mixin, (char*)&line, sizeof(mixer_input*))){
        play_int16_async(line, audio, amplitude);
        return true;
    }
    return false;
}

bool play_completed(file *mixin){
    mixer_input* line = NULL;
    if (sizeof(mixer_input*) == read_file(mixin, (char*)&line, sizeof(mixer_input*))){
        return line->buf[0].samples == NULL;
    }
    return true;
}