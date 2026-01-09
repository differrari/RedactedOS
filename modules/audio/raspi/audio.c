#include "audio/audio.h"

bool init_audio(){ return false; }

sizedptr audio_request_buffer(uint32_t device){ return (sizedptr){}; }
void audio_submit_buffer(){}

process_t* init_audio_mixer() { return 0; }

system_module audio_module = (system_module){
    .name = "audio",
    .mount = "/audio",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = 0,
    .fini = 0,
    .open = 0,
    .read = 0,
    .write = 0,
    .close = 0,
    .sread = 0,
    .swrite = 0,
    .readdir = 0,
};
