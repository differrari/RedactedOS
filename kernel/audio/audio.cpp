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


typedef struct mixer_buf {
    int16_t* samples;
    size_t   sample_count;
    uint16_t left_level;
    uint16_t right_level;
} mixer_buf;

typedef struct mixer_input {
    mixer_buf buf[2];
    uint8_t   channels;
} mixer_input;

typedef struct mixer_line {
    mixer_input u;
    uint8_t     bix;
    bool        in_use;
} mixer_line;

static mixer_line mixin[MIXER_INPUTS];

static void mixer_reset_line(int8_t lineId){
    if (lineId < 0 || lineId >= MIXER_INPUTS) return;
    mixer_line* line = &mixin[lineId];
    line->u.buf[0].samples = 0;
    line->u.buf[0].sample_count = 0;
    line->u.buf[0].left_level = 0;
    line->u.buf[0].right_level = 0;
    line->u.buf[1].samples = 0;
    line->u.buf[1].sample_count = 0;
    line->u.buf[1].left_level = 0;
    line->u.buf[1].right_level = 0;
    line->bix = 0;
    line->in_use = false;
}

static void mixer_reset(){
    for (int i = 0; i < MIXER_INPUTS; i++){
        mixer_reset_line(i);
    }
}

static int16_t master_level    = AUDIO_LEVEL_MAX / 2;
static int16_t master_premute  = 0;
static bool    master_muted    = false;

static inline int16_t normalise_int64_to_int16(int64_t input){
    int signal = input * master_level / (AUDIO_LEVEL_MAX * AUDIO_LEVEL_MAX);
    return (int16_t)max(min(signal, AUDIO_LEVEL_MAX), -AUDIO_LEVEL_MAX);
}

#define SUBMIT_SEPARATION_ACTUAL_MSECS (AUDIO_DRIVER_BUFFER_SIZE * 1000 / 44100)    // Ideally no remainder from division!
#define SUBMIT_SEPARATION_SAFE_MSECS   (SUBMIT_SEPARATION_ACTUAL_MSECS - 5)

static void mixer_run(){
    uint64_t buffer_run_start_time = 0;
    uint32_t buffer_run_count = 0;
    sizedptr buf = audio_request_buffer(audio_driver->out_dev->stream_id);
    do{
        bool have_audio = false;
        int16_t* output = (int16_t*)buf.ptr;
        int16_t* limit = output + buf.size;
        while (output < limit){
            int64_t left_signal = 0;
            int64_t right_signal = 0;
            mixer_line* line = mixin;
            while (line < mixin + MIXER_INPUTS){
                mixer_buf* buf = &line->u.buf[line->bix];
                if (buf->samples != NULL){
                    left_signal += *buf->samples * buf->left_level;
                    if (line->u.channels == 2){
                        buf->samples++;
                        buf->sample_count--;
                    }
                    right_signal += *buf->samples++ * buf->right_level;
                    if (--buf->sample_count < 1){
                        buf->samples = NULL;
                        // line->bix = 1 - line->bix;   // TODO: double-buffering for streaming support
                    }
                    have_audio = true;
                }
                ++line;
            }
            *output++ = normalise_int64_to_int16(left_signal);
            *output++ = normalise_int64_to_int16(right_signal);
        }
        if (have_audio){
            if (buffer_run_count == 0){
                buffer_run_start_time = get_time();
            }else{
                uint64_t this_buffer_time = buffer_run_start_time + 
                                            (buffer_run_count * SUBMIT_SEPARATION_ACTUAL_MSECS) + SUBMIT_SEPARATION_SAFE_MSECS;
                while (get_time() < this_buffer_time)
                    // TODO: yield cpu?
                    ;
            }
            audio_submit_buffer();
            ++buffer_run_count;
            buf = audio_request_buffer(audio_driver->out_dev->stream_id);
        }else{
            buffer_run_count = 0;
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


static FS_RESULT audio_open(const char *path, file *fd){
    if (0 == strcmp(path, "/output", true)){
        fd->id = reserve_fd_id();
        fd->size = UINT16_MAX;  // dummy value
        fd->cursor = 0;
        return FS_RESULT_SUCCESS;
    }
    return FS_RESULT_NOTFOUND;
}

static int audio_get_free_line(){
    // TODO: protect against multi-processing
    for (int i = 0; i < MIXER_INPUTS; ++i){
        if (mixin[i].in_use == false){
            mixer_reset_line(i);
            mixin[i].in_use = true;
            return i;
        }
    }
    return -1;
}

static size_t audio_read(file *fd, char *out_buf, size_t size, file_offset offset){
    if (size != sizeof(mixer_line_data)) return 0;
    mixer_line_data* data = (mixer_line_data*)out_buf;  // passed buffer has 'line' set
    int8_t lineId = data->lineId;
    fd->cursor = 0; // never moves
    if (lineId < 0){
        // request is for a free input line
        data->lineId = audio_get_free_line();
        data->count[0] = 0;
        data->count[1] = 0;
    }else{
        // request is for line data
        if (lineId < 0 || lineId >=  MIXER_INPUTS) return 0;
        mixer_line* line = &mixin[data->lineId];
        data->count[0] = (intptr_t)line->u.buf[0].sample_count;
        data->count[1] = (intptr_t)line->u.buf[1].sample_count;
    }
    return size;
}

static size_t audio_write(file *fd, const char *buf, size_t size, file_offset offset){
    mixer_command* cmd = (mixer_command*)buf;
    int8_t lineId = cmd->lineId;
    fd->cursor = 0;  // never moves
    switch (cmd->command){
        case MIXER_SETLEVEL: {
            int16_t value = min(INT16_MAX, max(0, (int)cmd->value));
            if (master_muted == false){
                master_level = value;
            }else{
                master_premute = value;  // not being able to change volume without un-muting is a UI/UX failure
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
        case MIXER_PLAY: {
            if (lineId < 0 || lineId >=  MIXER_INPUTS) return 0;
            mixer_line* line = &mixin[cmd->lineId];
            line->u.channels = cmd->audio->channels;
            line->u.buf[0].left_level = cmd->audio->amplitude;
            line->u.buf[0].right_level = cmd->audio->amplitude;
            line->u.buf[0].sample_count = cmd->audio->smpls_per_channel * cmd->audio->channels;
            line->u.buf[0].samples = (int16_t*)cmd->audio->samples.ptr;  // this must be last mutation of 'line'.
            break;
        }
        case MIXER_CLOSE_LINE:
            mixer_reset_line(lineId);
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
    .mount = "/audio",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = init_audio,
    .fini = 0,
    .open = audio_open,
    .read = audio_read,
    .write = audio_write,
    .sread = 0,
    .swrite = 0,
    .readdir = 0,
};
