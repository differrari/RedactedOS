#pragma once

#include "types.h"
#include "virtio/virtio_pci.h"

#define VIRTIO_AUDIO_ID 0x1059

typedef struct virtio_snd_config { 
    uint32_t jacks; 
    uint32_t streams; 
    uint32_t chmaps; 
} virtio_snd_config;

class VirtioAudioDriver {
public:
    bool init();
private:
    bool get_config();
    void config_jacks();

    bool config_streams(uint32_t streams);
    bool stream_set_params(uint32_t stream_id, uint32_t features, uint64_t format, uint64_t rate, uint8_t channels);

    void config_channels();
    virtio_device audio_dev;
};