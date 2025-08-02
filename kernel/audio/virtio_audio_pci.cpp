#include "virtio_audio_pci.hpp"
#include "pci.h"
#include "console/kio.h"
#include "memory/page_allocator.h"
#include "memory/memory_access.h"
#include "audio.h"

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

#define VIRTIO_SND_PCM_FMT_FLOAT    19
#define VIRTIO_SND_PCM_FMT_FLOAT64  20

#define VIRTIO_SND_PCM_RATE_44100   6
#define VIRTIO_SND_PCM_RATE_48000   7

#define SND_44100_BUFFER_SIZE 441
#define SND_FLOAT_BYTES 4

#define CONTROL_QUEUE   0
#define EVENT_QUEUE     1
#define TRANSMIT_QUEUE  2
#define RECEIVE_QUEUE   3

typedef struct virtio_snd_hdr { 
    uint32_t code; 
} virtio_snd_hdr; 
static_assert(sizeof(virtio_snd_hdr) == 4, "Sound header must be 4 bytes");

typedef struct virtio_snd_query_info { 
    virtio_snd_hdr hdr; 
    uint32_t start_id; 
    uint32_t count; 
    uint32_t size; 
} virtio_snd_query_info;
static_assert(sizeof(virtio_snd_query_info) == 16, "Query info struct must be 16 bytes");

typedef struct virtio_snd_pcm_hdr { 
    virtio_snd_hdr hdr; 
    uint32_t stream_id; 
} virtio_snd_pcm_hdr; 

typedef struct virtio_snd_info_hdr { 
    uint32_t hda_fn_nid; 
} virtio_snd_info_hdr;

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

typedef struct virtio_snd_event { 
    struct virtio_snd_hdr hdr; 
    uint32_t data; 
}__attribute__((packed)) virtio_snd_event;

bool VirtioAudioDriver::init(){
    uint64_t addr = find_pci_device(VIRTIO_VENDOR, VIRTIO_AUDIO_ID);
    if (!addr){ 
        kprintf("Disk device not found");
        return false;
    }

    uint64_t audio_device_address, audio_device_size;

    virtio_get_capabilities(&audio_dev, addr, &audio_device_address, &audio_device_size);
    pci_register(audio_device_address, audio_device_size);

    uint8_t interrupts_ok = pci_setup_interrupts(addr, AUDIO_IRQ, 1);
    switch(interrupts_ok){
        case 0:
            kprintf("[VIRTIO_AUDIO] Failed to setup interrupts");
            return false;
        case 1:
            kprintf("[VIRTIO_AUDIO] Interrupts setup with MSI-X %i,%i",AUDIO_IRQ);
            break;
        default:
            kprintf("[VIRTIO_AUDIO] Interrupts setup with MSI %i,%i",AUDIO_IRQ);
            break;
    }
    pci_enable_device(addr);

    if (!virtio_init_device(&audio_dev)){
        kprintf("Failed disk initialization");
        return false;
    }

    select_queue(&audio_dev, EVENT_QUEUE);

    for (uint16_t i = 0; i < 128; i++){
        void* buf = kalloc(audio_dev.memory_page, sizeof(virtio_snd_event), ALIGN_64B, true, true);
        virtio_add_buffer(&audio_dev, i, (uintptr_t)buf, sizeof(virtio_snd_event));
    }

    select_queue(&audio_dev, CONTROL_QUEUE);
    audio_dev.common_cfg->queue_msix_vector = 0;
    if (audio_dev.common_cfg->queue_msix_vector != 0){
        kprintf("[VIRTIO_AUDIO error] failed to setup interrupts for event queue");
        return false;
    }
    
    return get_config();
}

bool VirtioAudioDriver::get_config(){
    virtio_snd_config* snd_config = (virtio_snd_config*)audio_dev.device_cfg;

    kprintf("[VIRTIO_AUDIO] %i jacks, %i streams, %i channel maps", snd_config->jacks,snd_config->streams,snd_config->chmaps);

    config_jacks();
    if (!config_streams(snd_config->streams)) return false;
    config_channel_maps();

    return true;
}

void VirtioAudioDriver::config_jacks(){

}

bool VirtioAudioDriver::config_streams(uint32_t streams){
    virtio_snd_query_info* cmd = (virtio_snd_query_info*)kalloc(audio_dev.memory_page, sizeof(virtio_snd_query_info), ALIGN_4KB, true, true);
    cmd->hdr.code = VIRTIO_SND_R_PCM_INFO;
    cmd->count = streams;
    cmd->start_id = 0;
    cmd->size = sizeof(virtio_snd_pcm_info);

    size_t resp_size = sizeof(virtio_snd_hdr) + (streams * cmd->size);
    
    uintptr_t resp = (uintptr_t)kalloc(audio_dev.memory_page, resp_size, ALIGN_64B, true, true);

    if (!virtio_send(&audio_dev, audio_dev.common_cfg->queue_desc, audio_dev.common_cfg->queue_driver, audio_dev.common_cfg->queue_device,
        (uintptr_t)cmd, sizeof(virtio_snd_query_info), resp, resp_size, VIRTQ_DESC_F_WRITE)){
        kfree(cmd, sizeof(virtio_snd_query_info));
        kfree((void*)resp, resp_size);
        return false;
    }

    uint8_t *streams_bytes = (uint8_t*)(resp + sizeof(virtio_snd_hdr));
    
    virtio_snd_pcm_info *stream_info = (virtio_snd_pcm_info*)streams_bytes;
    for (uint32_t stream = 0; stream < streams; stream++){
        uint64_t format = read_unaligned64(&stream_info[stream].formats);
        uint64_t rate = read_unaligned64(&stream_info[stream].rates);

        kprintf("[VIRTIO_AUDIO] Stream %i (%s): Features %x. Format %x. Sample %x. Channels %i-%i",stream, (uintptr_t)(stream_info[stream].direction ? "OUT" : "IN"), stream_info[stream].features, format, rate, stream_info->channels_min, stream_info->channels_max);

        if (!(format & (1 << VIRTIO_SND_PCM_FMT_FLOAT))){
            kprintf("[VIRTIO_AUDIO implementation error] stream does not support Float32 format");
            return false;
        }

        if (!(rate & (1 << VIRTIO_SND_PCM_RATE_44100))){
            kprintf("[VIRTIO_AUDIO implementation error] stream does not support 44.1 kHz sample rate");
            return false;
        }

        if (!stream_set_params(stream, stream_info[stream].features, VIRTIO_SND_PCM_FMT_FLOAT, VIRTIO_SND_PCM_RATE_44100, stream_info->channels_max)){
            kprintf("[VIRTIO_AUDIO error] Failed to configure stream %i",stream);
        }
    }

    return true;
}

typedef struct virtio_snd_pcm_set_params { 
    virtio_snd_pcm_hdr hdr;
    uint32_t buffer_bytes; 
    uint32_t period_bytes; 
    uint32_t features; 
    uint8_t channels; 
    uint8_t format; 
    uint8_t rate; 
 
    uint8_t padding; 
}__attribute__((packed)) virtio_snd_pcm_set_params;
static_assert(sizeof(virtio_snd_pcm_set_params) == 24, "Virtio sound Set Params command needs to be n bytes");

#define SND_PERIOD 1

bool VirtioAudioDriver::stream_set_params(uint32_t stream_id, uint32_t features, uint64_t format, uint64_t rate, uint8_t channels){
    virtio_snd_pcm_set_params* cmd = (virtio_snd_pcm_set_params*)kalloc(audio_dev.memory_page, sizeof(virtio_snd_pcm_set_params), ALIGN_4KB, true, true);
    cmd->hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS;
    cmd->hdr.stream_id = stream_id;
    cmd->features = features;
    cmd->format = format;
    cmd->rate = rate;

    cmd->channels = channels;
    cmd->period_bytes = SND_44100_BUFFER_SIZE * SND_FLOAT_BYTES * channels;
    cmd->buffer_bytes = cmd->period_bytes * SND_PERIOD;

    virtio_snd_info_hdr *resp = (virtio_snd_info_hdr*)kalloc(audio_dev.memory_page, sizeof(virtio_snd_info_hdr), ALIGN_64B, true, true);

    if (!virtio_send(&audio_dev, audio_dev.common_cfg->queue_desc, audio_dev.common_cfg->queue_driver, audio_dev.common_cfg->queue_device,
        (uintptr_t)cmd, sizeof(virtio_snd_pcm_set_params), (uintptr_t)resp, sizeof(virtio_snd_info_hdr), VIRTQ_DESC_F_WRITE)){
        kfree(cmd, sizeof(virtio_snd_query_info));
        kfree((void*)resp, sizeof(virtio_snd_info_hdr));
        return false;
    }
    return true;
}

void VirtioAudioDriver::config_channel_maps(){

}

void VirtioAudioDriver::handle_interrupt(){
    select_queue(&audio_dev, EVENT_QUEUE);
    struct virtq_used* used = (struct virtq_used*)(uintptr_t)audio_dev.common_cfg->queue_device;
    struct virtq_desc* desc = (struct virtq_desc*)(uintptr_t)audio_dev.common_cfg->queue_desc;
    struct virtq_avail* avail = (struct virtq_avail*)(uintptr_t)audio_dev.common_cfg->queue_driver;

    uint16_t new_idx = used->idx;
    if (new_idx != last_used_idx) {
        uint16_t used_ring_index = last_used_idx % 128;
        last_used_idx = new_idx;
        struct virtq_used_elem* e = &used->ring[used_ring_index];
        uint32_t desc_index = e->id;
        uint32_t len = e->len;
        kprintf("Received audio event %i at index %i (len %i - %i)",used->idx, desc_index, len);

        avail->ring[avail->idx % 128] = desc_index;
        avail->idx++;

        *(volatile uint16_t*)(uintptr_t)(audio_dev.notify_cfg + audio_dev.notify_off_multiplier * EVENT_QUEUE) = 0;
    }
    select_queue(&audio_dev, CONTROL_QUEUE);
}