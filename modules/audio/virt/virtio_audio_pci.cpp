#include "virtio_audio_pci.hpp"
#include "pci.h"
#include "console/kio.h"
#include "memory/page_allocator.h"
#include "std/memory_access.h"
#include "syscalls/syscalls.h"
#include "audio/audio.h"
#include "std/memory.h"
#include "audio/OutputAudioDevice.hpp"

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

#define VIRTIO_SND_PCM_FMT_S16       5
#define VIRTIO_SND_PCM_FMT_U32      18 
#define VIRTIO_SND_PCM_FMT_FLOAT    19
#define VIRTIO_SND_PCM_FMT_FLOAT64  20

#define VIRTIO_SND_PCM_RATE_44100   6
#define VIRTIO_SND_PCM_RATE_48000   7

#define SND_SAMPLE_RATE     VIRTIO_SND_PCM_RATE_44100
#define SND_BUFFER_SIZE     AUDIO_DRIVER_BUFFER_SIZE
#define SND_SAMPLE_FORMAT   VIRTIO_SND_PCM_FMT_S16
#define SND_SAMPLE_BYTES    sizeof(int16_t)
#define SND_PERIOD          1
#define TOTAL_PERIOD_SIZE   SND_BUFFER_SIZE * SND_SAMPLE_BYTES * channels
#define TOTAL_BUF_SIZE      TOTAL_PERIOD_SIZE * SND_PERIOD

#define CONTROL_QUEUE   0
#define EVENT_QUEUE     1
#define TRANSMIT_QUEUE  2
#define RECEIVE_QUEUE   3

#define VIRTIO_SND_D_OUTPUT 0
#define VIRTIO_SND_D_INPUT  1

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
        kprintf("Audio device not found");
        return false;
    }

    uint64_t audio_device_address, audio_device_size;

    virtio_get_capabilities(&audio_dev, addr, &audio_device_address, &audio_device_size);
    pci_register(audio_device_address, audio_device_size);
    
    pci_enable_device(addr);

    if (!virtio_init_device(&audio_dev)){
        kprintf("[VIRTIO_AUDIO] Failed initialization");
        return false;
    }

    select_queue(&audio_dev, EVENT_QUEUE);

    audio_dev.common_cfg->queue_msix_vector = 0;
    if (audio_dev.common_cfg->queue_msix_vector != 0){
        kprintf("[VIRTIO_AUDIO] failed to setup interrupts for event queue");
        return false;
    }
    //TODO: This should (probably) be for input devices only
    // for (uint16_t i = 0; i < 128; i++){
    //     void* buf = kalloc(audio_dev.memory_page, sizeof(virtio_snd_event), ALIGN_64B, MEM_PRIV_KERNEL);
    //     virtio_add_buffer(&audio_dev, i, (uintptr_t)buf, sizeof(virtio_snd_event));
    // }

    select_queue(&audio_dev, CONTROL_QUEUE);

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

typedef struct virtio_snd_pcm_xfer { 
    uint32_t stream_id; 
}__attribute__((packed)) virtio_snd_pcm_xfer; 
 
typedef struct virtio_snd_pcm_status { 
    uint32_t status; 
    uint32_t latency_bytes; 
}__attribute__((packed)) virtio_snd_pcm_status; 

bool VirtioAudioDriver::config_streams(uint32_t streams){
    virtio_snd_query_info* cmd = (virtio_snd_query_info*)kalloc(audio_dev.memory_page, sizeof(virtio_snd_query_info), ALIGN_4KB, MEM_PRIV_KERNEL);
    cmd->hdr.code = VIRTIO_SND_R_PCM_INFO;
    cmd->count = streams;
    cmd->start_id = 0;
    cmd->size = sizeof(virtio_snd_pcm_info);

    size_t resp_size = sizeof(virtio_snd_hdr) + (streams * cmd->size);
    
    uintptr_t resp = (uintptr_t)kalloc(audio_dev.memory_page, resp_size, ALIGN_64B, MEM_PRIV_KERNEL);

    virtio_buf b[2] = {VBUF(cmd, sizeof(virtio_snd_query_info), 0), VBUF((void*)resp, resp_size, VIRTQ_DESC_F_WRITE)};
    if(!virtio_send_nd(&audio_dev, b, 2)){
        kfree(cmd, sizeof(virtio_snd_query_info));
        kfree((void*)resp, resp_size);
        return false;
    }

    uint8_t *streams_bytes = (uint8_t*)(resp + sizeof(virtio_snd_hdr));
    
    virtio_snd_pcm_info *stream_info = (virtio_snd_pcm_info*)streams_bytes;
    for (uint32_t stream = 0; stream < streams; stream++){
        uint64_t format = read_unaligned64(&stream_info[stream].formats);
        uint64_t rate = read_unaligned64(&stream_info[stream].rates);

        kprintf("[VIRTIO_AUDIO] Stream %i (%s): Features %x. Format %x. Sample %x. Channels %i-%i",stream, (uintptr_t)(stream_info[stream].direction ? "IN" : "OUT"), stream_info[stream].features, format, rate, stream_info->channels_min, stream_info->channels_max);

        if (!(format & (1 << SND_SAMPLE_FORMAT))){
            kprintf("[VIRTIO_AUDIO implementation error] stream does not support int16 format");
            return false;
        }

        uint32_t sample_rate = 44100;
        if (!(rate & (1 << SND_SAMPLE_RATE))){
            kprintf("[VIRTIO_AUDIO implementation error] stream does not support 44.1 kHz sample rate");
            return false;
        }

        uint8_t channels = stream_info->channels_max;

        if (!stream_set_params(stream, stream_info[stream].features, SND_SAMPLE_FORMAT, SND_SAMPLE_RATE, channels)){
            kprintf("[VIRTIO_AUDIO error] Failed to configure stream %i",stream);
        }

        if (stream_info[stream].direction == VIRTIO_SND_D_OUTPUT){
            out_dev = new OutputAudioDevice();
            out_dev->stream_id = stream;
            out_dev->rate = sample_rate;
            out_dev->channels = channels;
            out_dev->packet_size = sizeof(virtio_snd_pcm_xfer) + TOTAL_BUF_SIZE;
            out_dev->buf_size = TOTAL_BUF_SIZE/SND_SAMPLE_BYTES;
            out_dev->header_size = sizeof(virtio_snd_pcm_xfer);
            out_dev->populate();
        }
    }
    select_queue(&audio_dev, TRANSMIT_QUEUE);
    return true;
}

void VirtioAudioDriver::send_buffer(sizedptr buf){
    virtio_add_buffer(&audio_dev, cmd_index % audio_dev.common_cfg->queue_size, buf.ptr, buf.size, true);
    volatile virtq_used* u = (virtq_used*)audio_dev.common_cfg->queue_device;
    while (u->idx < cmd_index-2)
        yield();
    cmd_index++;
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

bool VirtioAudioDriver::stream_set_params(uint32_t stream_id, uint32_t features, uint64_t format, uint64_t rate, uint8_t channels){
    virtio_snd_pcm_set_params* cmd = (virtio_snd_pcm_set_params*)kalloc(audio_dev.memory_page, sizeof(virtio_snd_pcm_set_params), ALIGN_4KB, MEM_PRIV_KERNEL);
    cmd->hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS;
    cmd->hdr.stream_id = stream_id;
    cmd->features = features;
    cmd->format = format;
    cmd->rate = rate;

    cmd->channels = channels;
    cmd->period_bytes = TOTAL_PERIOD_SIZE;
    cmd->buffer_bytes = TOTAL_BUF_SIZE;

    virtio_snd_info_hdr *resp = (virtio_snd_info_hdr*)kalloc(audio_dev.memory_page, sizeof(virtio_snd_info_hdr), ALIGN_64B, MEM_PRIV_KERNEL);

    virtio_buf b[2] = {VBUF(cmd, sizeof(virtio_snd_pcm_set_params), 0), VBUF(resp, sizeof(virtio_snd_info_hdr), VIRTQ_DESC_F_WRITE)};
    bool result=virtio_send_nd(&audio_dev, b, 2);    
    kfree(cmd, sizeof(virtio_snd_query_info));
    kfree((void*)resp, sizeof(virtio_snd_info_hdr));

    if (result)
        result = send_simple_stream_cmd(stream_id, VIRTIO_SND_R_PCM_PREPARE);

    if (result)
        result = send_simple_stream_cmd(stream_id, VIRTIO_SND_R_PCM_START);
    
    return result;
}

bool VirtioAudioDriver::send_simple_stream_cmd(uint32_t stream_id, uint32_t command){

    virtio_snd_pcm_hdr* cmd = (virtio_snd_pcm_hdr*)kalloc(audio_dev.memory_page, sizeof(virtio_snd_pcm_hdr), ALIGN_4KB, MEM_PRIV_KERNEL);
    cmd->hdr.code = command;
    cmd->stream_id = stream_id;
    
    virtio_snd_info_hdr *resp = (virtio_snd_info_hdr*)kalloc(audio_dev.memory_page, sizeof(virtio_snd_info_hdr), ALIGN_64B, MEM_PRIV_KERNEL);
    
    virtio_buf b[2] = {VBUF(cmd, sizeof(virtio_snd_pcm_hdr), 0), VBUF(resp, sizeof(virtio_snd_info_hdr), VIRTQ_DESC_F_WRITE)};
    bool result=virtio_send_nd(&audio_dev, b, 2);

    kfree(cmd, sizeof(virtio_snd_query_info));
    kfree((void*)resp, sizeof(virtio_snd_info_hdr));
    
    return result;
}

void VirtioAudioDriver::config_channel_maps(){

}