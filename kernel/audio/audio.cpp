#include "audio.h"
#include "virtio_audio_pci.hpp"
#include "kernel_processes/kprocess_loader.h"
#include "console/kio.h"
#include "math/math.h"
#include "audio/cuatro.h"
#include "graph/graphics.h"
#include "theme/theme.h"

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

void make_wave(WAVE_TYPE type, float freq, float seconds, uint32_t amplitude){
    gpu_clear(0x1fb03f);
    uint32_t period = 441/((freq/100.f) * 2);//TODO: improve this formula
    kprintf("Period %i",period);
    uint32_t accumulator = 0;
    uint32_t increment = (uint32_t)(freq * (float)UINT32_MAX / 44100.0);
    gpu_size size = gpu_get_screen_size();
    uint32_t previous_pixel = UINT32_MAX;

    //TODO: distorsion
    //size of the buffer should be bigger, 
    //palloc should be 64 pages
    //in the virtio driver, cmd_index is waiting for the device to catch up
    
    for (int i = 0; i < seconds * 100; i++){
        sizedptr buf = audio_request_buffer(audio_driver->out_dev->stream_id);
        
        uint32_t num_samples = buf.size;
        uint32_t *buffer = (uint32_t*)buf.ptr;
        for (uint32_t sample = 0; sample < num_samples; sample++){
            float wave = sample_raw_wave(type, accumulator, period);
            uint32_t min = 64 * num_samples;
            buffer[sample] = wave * amplitude;

            accumulator += increment;
            if (accumulator >= min && accumulator < min + size.width){
                gpu_point p = (gpu_point){ accumulator - min, 100-(uint32_t)(100*wave)};
                if (previous_pixel != UINT32_MAX && abs(p.y-previous_pixel) > 10){
                    gpu_draw_line({ p.x - 1, previous_pixel}, p, 0xFFB4DD13);
                }
                previous_pixel = p.y;
                gpu_draw_pixel(p, 0xFFB4DD13);
            }
        }
        audio_submit_buffer();
    }
    gpu_flush();
    while(1);
}

int play_test_audio(int argc, char* argv[]){      
    make_wave(WAVE_SQUARE, 100, 3, UINT32_MAX/4);
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