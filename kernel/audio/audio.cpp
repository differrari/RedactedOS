#include "audio.h"
#include "virtio_audio_pci.hpp"
#include "kernel_processes/kprocess_loader.h"

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

int play_test_audio(int argc, char* argv[]){      
    for (int i = 0; i < 100; i++){
        sizedptr buf = audio_request_buffer(audio_driver->out_dev->stream_id);
        uint32_t num_samples = buf.size/sizeof(uint32_t);
        uint32_t *buffer = (uint32_t*)buf.ptr;
        for (uint32_t sample = 0; sample < num_samples; sample++){
            buffer[sample] = sample < num_samples/2 == 0 ? 0x88888888 : UINT32_MAX;
        }
        audio_submit_buffer();
    }
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