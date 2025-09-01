#include "audio.h"
#include "virtio_audio_pci.hpp"
#include "kernel_processes/kprocess_loader.h"
#include "console/kio.h"
#include "math/math.h"
#include "audio/cuatro.h"

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

void make_wave(WAVE_TYPE type, float freq, float seconds){
    uint32_t period = 441/((freq/100.f) * 2);//TODO: improve this formula
    uint32_t accumulator = 0;

    for (int i = 0; i < (int)(seconds * 100); i++){
        sizedptr buf = audio_request_buffer(audio_driver->out_dev->stream_id);
        
        uint32_t num_samples = buf.size;
        uint32_t *buffer = (uint32_t*)buf.ptr;
        for (uint32_t sample = 0; sample < num_samples; sample++){
            buffer[sample] = sample_wave(type, accumulator, period, UINT32_MAX/2);
            accumulator++;
        }
        audio_submit_buffer();
    }
}

int play_test_audio(int argc, char* argv[]){      
    make_wave(WAVE_SAW, 261.63, 1);
    return 0;
}

process_t* init_audio_mixer(){
    return create_kernel_process("audiotest", play_test_audio, 0, 0);
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