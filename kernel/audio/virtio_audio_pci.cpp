#include "virtio_audio_pci.hpp"
#include "pci.h"
#include "console/kio.h"
#include "memory/page_allocator.h"

struct virtio_snd_hdr { 
    uint32_t code; 
}; 

bool VirtioAudioDriver::init(){
    uint64_t addr = find_pci_device(VIRTIO_VENDOR, VIRTIO_AUDIO_ID);
    if (!addr){ 
        kprintf("Disk device not found");
        return false;
    }

    pci_enable_device(addr);

    uint64_t audio_device_address, audio_device_size;

    virtio_get_capabilities(&audio_dev, addr, &audio_device_address, &audio_device_size);
    pci_register(audio_device_address, audio_device_size);
    if (!virtio_init_device(&audio_dev)) {
        kprintf("Failed disk initialization");
        return false;
    }

    get_config();

    return true;
}

void VirtioAudioDriver::get_config(){
    virtio_snd_config* snd_config = (virtio_snd_config*)audio_dev.device_cfg;

    kprintf("There are %i jacks, %i streams, %i channels", snd_config->jacks,snd_config->streams,snd_config->chmaps);

    config_jacks();
    // for (uint32_t stream = 0; stream < snd_config->streams; stream++)
    config_streams(snd_config->streams);
    config_channels();

}

void VirtioAudioDriver::config_jacks(){

}

struct virtio_snd_query_info { 
    virtio_snd_hdr hdr; 
    uint32_t start_id; 
    uint32_t count; 
    uint32_t size; 
};
static_assert(sizeof(virtio_snd_query_info) == 16, "Query info struct must be 16 bytes");

struct virtio_snd_pcm_hdr { 
    virtio_snd_hdr hdr; 
    uint32_t stream_id; 
}; 

struct virtio_snd_info_hdr { 
    uint32_t hda_fn_nid; 
};

typedef struct virtio_snd_pcm_info { 
    virtio_snd_info_hdr info_hdr; 
    uint32_t features; /* 1 << VIRTIO_SND_PCM_F_XXX */ 
    uint64_t formats; /* 1 << VIRTIO_SND_PCM_FMT_XXX */ 
    uint64_t rates; /* 1 << VIRTIO_SND_PCM_RATE_XXX */ 
    uint8_t direction; 
    uint8_t channels_min; 
    uint8_t channels_max; 
 
    uint8_t padding[5]; 
}__attribute__((packed)) virtio_snd_pcm_info;
static_assert(sizeof(virtio_snd_pcm_info) == 32, "PCM Info must be 32");

#define VIRTIO_SND_R_PCM_INFO       0x0100
#define VIRTIO_SND_R_PCM_SET_PARAMS 0x0101 
#define VIRTIO_SND_R_PCM_PREPARE    0x0102 
#define VIRTIO_SND_R_PCM_RELEASE    0x0103 
#define VIRTIO_SND_R_PCM_START      0x0104 
#define VIRTIO_SND_R_PCM_STOP       0x0105

#define VIRTIO_SND_S_OK         0x8000
#define VIRTIO_SND_S_BAD_MSG    0x8001
#define VIRTIO_SND_S_NOT_SUPP   0x8002
#define VIRTIO_SND_S_IO_ERR     0x8003

bool VirtioAudioDriver::config_streams(uint32_t streams){
    virtio_snd_query_info* cmd = (virtio_snd_query_info*)kalloc(audio_dev.memory_page, sizeof(virtio_snd_query_info), ALIGN_4KB, true, true);
    cmd->hdr.code = VIRTIO_SND_R_PCM_INFO;
    cmd->count = streams;
    cmd->start_id = 0;
    cmd->size = sizeof(virtio_snd_pcm_info);

    kprintf("Sending size %i", cmd->size);

    virtio_snd_pcm_info* resp = (virtio_snd_pcm_info*)kalloc(audio_dev.memory_page, 3 * sizeof(virtio_snd_pcm_info), ALIGN_4KB, true, true);

    if (!virtio_send(&audio_dev, audio_dev.common_cfg->queue_desc, audio_dev.common_cfg->queue_driver, audio_dev.common_cfg->queue_device,
        (uintptr_t)cmd, 16, (uintptr_t)resp, 3 * sizeof(virtio_snd_pcm_info), VIRTQ_DESC_F_WRITE)){
        kfree((void*)cmd, sizeof(virtio_snd_query_info));
        kfree((void*)resp, streams * sizeof(virtio_snd_pcm_info));
        return false;
    }

    kprintf("Response: Features %x. Format %x. Sample %x",resp->features, resp->formats, resp->rates);

    return true;
}

void VirtioAudioDriver::config_channels(){

}