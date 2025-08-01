#include "audio.h"
#include "virtio_audio_pci.hpp"

VirtioAudioDriver *audio_driver;

bool init_audio(){
    audio_driver = new VirtioAudioDriver();
    return audio_driver->init();
}