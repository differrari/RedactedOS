#include "audio.h"
#include "virtio_audio_pci.hpp"
#include "kernel_processes/kprocess_loader.h"
#include "console/kio.h"
#include "math/math.h"
#include "audio/cuatro.h"
#include "audio/wav.h"
#include "theme/theme.h"
#include "syscalls/syscalls.h"

VirtioAudioDriver *audio_driver;

bool init_audio(){
    audio_driver = new VirtioAudioDriver();
    return audio_driver->init();
}

void audio_handle_interrupt(){
    audio_driver->handle_interrupt();
}

sizedptr audio_request_buffer(uint32_t device){
    return audio_driver->out_dev->request_buffer();
}

void audio_submit_buffer(){
    audio_driver->out_dev->submit_buffer(audio_driver);
}

// TODO: get sample rate, channels etc. from audio driver. 
//       Currently ASSUMES output stream has two channels in U32 format.
const float SAMPLE_RATE = 44100.f;


void play_wave(WAVE_TYPE type, float freq, float seconds, uint32_t amplitude){
    uint32_t phase = 0;
    // TODO: validate freqency to ensure phase_incr is sensible value.
    uint32_t phase_incr = (uint32_t)(freq * (float)PHASE_MAX / SAMPLE_RATE);
    uint32_t samples_remaining = ceil(SAMPLE_RATE * seconds);

    while (samples_remaining > 0){
        sizedptr buf = audio_request_buffer(audio_driver->out_dev->stream_id);
        uint32_t* buffer = (uint32_t*)buf.ptr;
        uint32_t samples_in_buffer = (uint32_t)min(buf.size/2, samples_remaining);
        uint32_t sample = 0;
        size_t slot = 0;
        while (sample++ < samples_in_buffer){
            float wave = sample_raw_wave(type, phase) * amplitude;
            buffer[slot++] = wave;      // Left ch.
            buffer[slot++] = wave;      // Right ch.
            phase = (phase + phase_incr) & PHASE_MASK;
        }
        while (slot < buf.size){
            buffer[slot++] = WAVE_MID_VALUE;
        }
        audio_submit_buffer();
        samples_remaining -= samples_in_buffer;
    }
}

void play_silence(float seconds){
    if (seconds < 0.01) return;
    sizedptr buf = audio_request_buffer(audio_driver->out_dev->stream_id);
    uint32_t buffer_count = (uint32_t)ceil(SAMPLE_RATE * seconds * 2 / buf.size);
    do{
        uint32_t* buffer = (uint32_t*)buf.ptr;
        size_t slot = 0;
        while (slot < buf.size){
            buffer[slot++] = WAVE_MID_VALUE;
        }
        audio_submit_buffer();
        buf = audio_request_buffer(audio_driver->out_dev->stream_id);
    } while (--buffer_count > 0);
}

static inline uint32_t int16_to_uint32(int16_t sample, uint32_t amplitude){
    return (uint64_t)(((int64_t)sample * amplitude) >> 16) + WAVE_MID_VALUE;
}

void play_int16_samples(int16_t *samples, size_t smpls_per_channel, size_t channels, uint32_t amplitude){
    size_t samples_remaining = smpls_per_channel * channels;
    while (samples_remaining > 0){
        sizedptr buf = audio_request_buffer(audio_driver->out_dev->stream_id);
        uint32_t* buffer = (uint32_t*)buf.ptr;
        size_t samples_in_buffer = (size_t)min(buf.size, samples_remaining);
        size_t slot = 0;
        while (slot < samples_in_buffer){
            uint32_t sample = int16_to_uint32(*samples++, amplitude);
            buffer[slot++] = sample;        // Left ch.
            buffer[slot++] = (channels == 1) ? sample : int16_to_uint32(*samples++, amplitude);  // Right ch.
        }
        while (slot < buf.size){
            buffer[slot++] = WAVE_MID_VALUE;
        }
        audio_submit_buffer();
        samples_remaining -= samples_in_buffer / (3 - channels);  // TODO: There must be a better way...
    }
}

void play_startup(){
    wav_data wav = {};
    if (wav_load_as_int16("/boot/redos/startup.wav", &wav)){
        play_int16_samples((int16_t*)wav.samples.ptr, wav.smpls_per_channel, wav.channels, AUDIO_LEVEL_MAX/2);
        free((void*)wav.samples.ptr, wav.samples.size);
    }else{
        play_wave(WAVE_SAW, 440, 0.1, AUDIO_LEVEL_MAX/2);
        play_silence(0.05);
        play_wave(WAVE_SAW, 494, 0.1, AUDIO_LEVEL_MAX/2);
        play_silence(0.05);
        play_wave(WAVE_SAW, 523, 0.3, AUDIO_LEVEL_MAX/2);
        play_silence(0.5);
    }
}

int audio_mixer(int argc, char* argv[]){
    play_startup();
    while (1) sleep(1000);  // Idle sound process for time(!) being
    return 0;
}

process_t* init_audio_mixer(){
    return create_kernel_process("Audio out", audio_mixer, 0, 0);
}

driver_module audio_module = (driver_module){
    .name = "audio",
    .mount = "/dev/audio",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = init_audio,
    .fini = 0,
    .open = 0,
    .read = 0,
    .write = 0,
    .seek = 0,
    .readdir = 0,
};
