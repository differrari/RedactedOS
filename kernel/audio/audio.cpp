#include "audio.h"
#include "virtio_audio_pci.hpp"
#include "kernel_processes/kprocess_loader.h"
#include "console/kio.h"
#include "math/math.h"
#include "audio/cuatro.h"
#include "audio/wav.h"
#include "std/memory.h"
#include "theme/theme.h"
#include "syscalls/syscalls.h"

VirtioAudioDriver *audio_driver;

bool init_audio(){
    audio_driver = new VirtioAudioDriver();
    return audio_driver->init();
}

sizedptr audio_request_buffer(uint32_t device){
    return audio_driver->out_dev->request_buffer();
}

void audio_submit_buffer(){
    audio_driver->out_dev->submit_buffer(audio_driver);
}

void audio_get_info(uint32_t* rate, uint8_t* channels) {
    *rate = audio_driver->out_dev->rate;
    *channels = audio_driver->out_dev->channels;
}


#define MIX_LINES 4

typedef struct {
    mixer_input u;  // user process can r/w this - reliant on no memory protection!
    uint8_t     bix;
    uint64_t    id;
} mixer_line;

static mixer_line mixin[MIX_LINES];

static void mixer_reset_line(mixer_line* line){
    line->u.buf[0].samples = 0;
    line->u.buf[0].sample_count = 0;
    line->u.buf[0].left_level = 0;
    line->u.buf[0].right_level = 0;
    line->u.buf[1].samples = 0;
    line->u.buf[1].sample_count = 0;
    line->u.buf[1].left_level = 0;
    line->u.buf[1].right_level = 0;
    line->bix = 0;
    line->id = 0;  // 0 as 'unused' is ok - filesystem allocates id's > 255
}

static void mixer_reset(){
    for (int i = 0; i < MIX_LINES; i++){
        mixer_reset_line(&mixin[i]);
    }
}

static uint32_t master_level   = AUDIO_LEVEL_MAX / 2;
static uint32_t master_premute = 0;
static bool     master_muted   = false;

static inline uint32_t normalise_int64_to_uint32(int64_t input){
    return (uint32_t)(((input / UINT16_MAX) * (int64_t)master_level / (int64_t)AUDIO_LEVEL_MAX) + (int64_t)WAVE_MID_VALUE);
}

static void mixer_run(){
    sizedptr buf = audio_request_buffer(audio_driver->out_dev->stream_id);
    do{
        bool have_audio = false;
        uint32_t* output = (uint32_t*)buf.ptr;
        uint32_t* limit = output + buf.size;
        while (output < limit){
            int64_t left_signal = 0;
            int64_t right_signal = 0;
            mixer_line* input = mixin;
            while (input < mixin+MIX_LINES){
                mixer_buf* buf = &input->u.buf[input->bix];
                if (buf->samples != NULL){
                    left_signal += (int64_t)*buf->samples * buf->left_level;
                    if (input->u.channels == 2){
                        buf->samples++;
                        buf->sample_count--;
                    }
                    right_signal += (int64_t)*buf->samples++ * buf->right_level;
                    if (--buf->sample_count < 1){
                        buf->samples = NULL;
                        input->bix = 1 - input->bix;
                    }
                    have_audio = true;
                }
                ++input;
            }
            *output++ = normalise_int64_to_uint32(left_signal);
            *output++ = normalise_int64_to_uint32(right_signal);
        }
        if (have_audio){
            audio_submit_buffer();
            buf = audio_request_buffer(audio_driver->out_dev->stream_id);
        }else{
            // TODO: yield cpu
        }
    } while (1);
}

static int audio_mixer(int argc, char* argv[]){
    mixer_reset();
    mixer_run();
    return 0;
}

process_t* init_audio_mixer(){
    return create_kernel_process("Audio out", audio_mixer, 0, 0);
}


static FS_RESULT audio_open(const char *path, file *out_fd){
    if (0 == strcmp(path, "/output", true)){
        for (int i = 0; i < MIX_LINES; ++i){
            if (mixin[i].id == 0){
                mixer_reset_line(&mixin[i]);
                out_fd->id = reserve_fd_id();
                mixin[i].id = out_fd->id;
                out_fd->size = sizeof(intptr_t);
                out_fd->cursor = 0; // TODO: check if necessary?  not done by filesystem??
                return FS_RESULT_SUCCESS;
            }
        }
    }
    return FS_RESULT_NOTFOUND;
}

static mixer_line* find_line(uint64_t id){
    for (int i = 0; i < MIX_LINES; ++i){
        if (mixin[i].id == id){
            return &mixin[i];
        }
    }
    return NULL;
}

static size_t audio_read(file *fd, char *out_buf, size_t size, file_offset offset){
    // reads ptr to fd's mixer line
    size = min(size, sizeof(intptr_t));
    mixer_line* line = find_line(fd->id);
    memcpy(out_buf, &line, size);
    return size;
}

static size_t audio_write(file *fd, const char *buf, size_t size, file_offset offset){
    mixer_line* line = find_line(fd->id);
    if (line == NULL || size != sizeof(mixer_command)){
        return 0;
    }
    mixer_command* cmd = (mixer_command*)buf;
    switch (cmd->command){
        case MIXER_SETLEVEL:
            {
                uint32_t value = min(UINT32_MAX, max(0, cmd->value));
                if (master_muted == false){
                    master_level = value;
                }else{
                    master_premute = value;
                }
                break;
            }
        case MIXER_MUTE:
            if (master_muted == false){
                master_premute = master_level;
                master_level = 0;
                master_muted = true;
            }
            break;
        case MIXER_UNMUTE:
            if (master_muted == true){
                master_level = master_premute;
                master_muted = false;
            }
            break;
        case MIXER_CLOSE_LINE:
            mixer_reset_line(line);
            break;
        default:
            return 0;
    }
    return size;
}

// static file_offset audio_seek(file *fd, file_offset offset){
//     return 0;
// }

// static sizedptr audio_readdir(const char* path){
//     return (sizedptr){ 0, 0 };
// }



driver_module audio_module = (driver_module){
    .name = "audio",
    .mount = "/dev/audio",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = init_audio,
    .fini = 0,
    .open = audio_open,
    .read = audio_read,
    .write = audio_write,
    .seek = 0,
    .readdir = 0,
};
