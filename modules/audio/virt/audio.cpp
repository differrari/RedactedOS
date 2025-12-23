#include "audio/audio.h"
#include "virtio_audio_pci.hpp"
#include "kernel_processes/kprocess_loader.h"
#include "syscalls/syscalls.h"
#include "exceptions/timer.h"
#include "math/math.h"
#include "audio/cuatro.h"
#include "audio/mixer.h"
#include "std/memory.h"


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

typedef struct mixer_line {
    mixer_dblbuf dbl;
    sizedptr source;
    uint64_t start_time;
    uint32_t delay_ms;
    AUDIO_LIFETIME life;
    int16_t level;
    int16_t pan;
    int16_t left_lvl;
    int16_t right_lvl;
    bool in_use;
    uint8_t channels;
} mixer_line;

static mixer_line mixin[MIXER_INPUTS];

static void mixer_reset_line(int8_t lineId){
    if (lineId < 0 || lineId >= MIXER_INPUTS) return;
    memset(mixin + lineId, 0, sizeof(mixer_line));
    mixin[lineId].life = AUDIO_OFF;
}

static void mixer_reset(){
    for (int i = 0; i < MIXER_INPUTS; i++) mixer_reset_line(i);
}

static int16_t master_level    = AUDIO_LEVEL_MAX / 2;
static int16_t master_premute  = 0;
static bool    master_muted    = false;

#define BUFFER_SEPARATION_MSECS (AUDIO_DRIVER_BUFFER_SIZE * 1000 / 44100)    // Ideally no remainder from division!
#define BUFFER_PREROLL_MSECS   (BUFFER_SEPARATION_MSECS - 5)
static_assert(BUFFER_PREROLL_MSECS > 0, "Audio buffer size too small");

static int32_t bhaskara_sin_int32(int deg){
    // https://en.wikipedia.org/wiki/Bh%C4%81skara_I%27s_sine_approximation_formula
    return (INT16_MAX * 4 * deg * (180 - deg)) / (40500 - (deg * (180 - deg)));
}

static void calc_channel_levels(int8_t lineId){
    // !! approximation of 3db pan law
    mixer_line* line = &mixin[lineId];
    int pan = ((int)line->pan + INT16_MAX) / 2;  // 0 == hard left, INT16_MAX = hard right
    int degrees = ((90 * pan)+(INT16_MAX/2)) / INT16_MAX;
    line->left_lvl = line->level * bhaskara_sin_int32(90-degrees) / INT16_MAX;
    line->right_lvl = line->level * bhaskara_sin_int32(degrees) / INT16_MAX;
}

static inline audio_sample_t normalise_int64_sample(int64_t input){
    int64_t signal = input * master_level / (SIGNAL_LEVEL_MAX * SIGNAL_LEVEL_MAX);
    return (audio_sample_t)max(min(signal, SIGNAL_LEVEL_MAX), SIGNAL_LEVEL_MIN);
}

static inline void buffer_exhausted(mixer_line* line, sizedptr* inbuf){
    switch (line->life) {
        case AUDIO_OFF:
            break;
        case AUDIO_ONESHOT:
            inbuf->ptr = NULL;
            mixer_reset_line(line - mixin);
            break;
        case AUDIO_ONESHOT_FREE:
            inbuf->ptr = NULL;
            free_sized((void*)line->source.ptr, line->source.size);
            mixer_reset_line(line - mixin);
            break;
        case AUDIO_LOOP:
            line->start_time = timer_now_msec() + line->delay_ms;
            inbuf->size = line->source.size / sizeof(audio_sample_t);
            inbuf->ptr = line->source.ptr;
            break;
        case AUDIO_STREAM:
            inbuf->ptr = NULL;
            line->dbl.buf_idx = 1 - line->dbl.buf_idx;
            break;
    }
}

static void mixer_run(){
    uint64_t buffers_start_time = 0;
    uint64_t buffers_output = 0;
    sizedptr outbuf = audio_request_buffer(audio_driver->out_dev->stream_id);
    do{
        audio_sample_t* output = (audio_sample_t*)outbuf.ptr;
        audio_sample_t* limit = output + outbuf.size;
        uint64_t now = timer_now_msec();
        // TODO: consider inverting these nested ifs - maybe more cache-friendly.
        while (output < limit){
            int64_t left_signal = 0;
            int64_t right_signal = 0;
            mixer_line* line = mixin;
            while (line < mixin + MIXER_INPUTS){
                sizedptr* inbuf = &line->dbl.buf[line->dbl.buf_idx];
                if (line->in_use && inbuf->ptr != NULL && line->start_time < now){
                    left_signal += *(audio_sample_t*)inbuf->ptr * line->level; // line->left_lvl;
                    if (line->channels == 2){
                        inbuf->ptr += sizeof(audio_sample_t);
                        --inbuf->size;
                    }
                    right_signal += *(audio_sample_t*)inbuf->ptr * line->level; // line->right_lvl;
                    inbuf->ptr += sizeof(audio_sample_t);
                    if (--inbuf->size < 1) buffer_exhausted(line, inbuf);
                }
                ++line;
            }
            *output++ = normalise_int64_sample(left_signal);
            *output++ = normalise_int64_sample(right_signal);
        }
        if (buffers_output == 0){
            buffers_start_time = timer_now_msec();
        }else{
            uint64_t this_buffer_due = buffers_start_time + 
                                        (buffers_output * BUFFER_SEPARATION_MSECS) + BUFFER_PREROLL_MSECS;
            while (timer_now_msec() < this_buffer_due)
                yield();
        }
        audio_submit_buffer();
        ++buffers_output;
        outbuf = audio_request_buffer(audio_driver->out_dev->stream_id);
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
    if (0 == strcmp_case(path, "/output",true)){
        fd->id = reserve_fd_gid("/audio/output");//TODO: Review
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
    mixer_line_data* data = (mixer_line_data*)out_buf;  // !! passed buffer has 'line' set
    fd->cursor = 0; // never moves
    if (data->lineId < 0){
        // request is for a free input line
        data->lineId = audio_get_free_line();
    }
    if (data->lineId < 0 || data->lineId >=  MIXER_INPUTS) return 0;
    data->dbl = mixin[data->lineId].dbl;
    return size;
}

static size_t audio_write(file *fd, const char *buf, size_t size, file_offset offset){
    if (size != sizeof(mixer_command)) return 0;
    mixer_command* cmd = (mixer_command*)buf;
    int8_t lineId = cmd->lineId;
    fd->cursor = 0;  // never moves
    switch (cmd->command){
        case MIXER_SETLEVEL: {
            int16_t level = min(AUDIO_LEVEL_MAX, max(0, (int)cmd->level));
            if (lineId == -1){
                if (master_muted == false){
                    master_level = level;
                }else{
                    master_premute = level;  // not being able to change volume without un-muting is a UI/UX failure
                }
            }else{
                mixin[lineId].level = level;
                mixin[lineId].pan = cmd->pan;
                calc_channel_levels(lineId);
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
            // AUDIO_ONESHOT, AUDIO_ONESHOT_FREE, AUDIO_LOOP: set & forget
            // AUDIO_STREAM: initialise
            if (lineId < 0 || lineId >=  MIXER_INPUTS) return 0;
            mixer_line* line = &mixin[cmd->lineId];
            line->channels = cmd->audio->channels;
            line->level = cmd->level;
            line->pan = cmd->pan;
            calc_channel_levels(lineId);
            line->delay_ms = cmd->delay_ms;
            line->start_time = timer_now_msec() + line->delay_ms;
            line->life = cmd->life;
            line->source = cmd->audio->samples;
            line->dbl.buf_idx = 0;
            line->dbl.buf[0].size = line->source.size / sizeof(audio_sample_t);
            line->dbl.buf[0].ptr = line->source.ptr;  // this must be last mutation of 'line'
            break;
        }
        case MIXER_SETBUFFER: {
            // AUDIO_STREAM: populate next buffer
            if (lineId < 0 || lineId >=  MIXER_INPUTS) return 0;
            mixer_line* line = &mixin[lineId];
            uint8_t nxt_buf = (line->dbl.buf[0].ptr == NULL) ? 0 : 1;
            line->dbl.buf[nxt_buf].size = cmd->samples.size / sizeof(audio_sample_t);
            line->dbl.buf[nxt_buf].ptr = cmd->samples.ptr; // this must be last mutation of 'line'
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



system_module audio_module = (system_module){
    .name = "audio",
    .mount = "/audio",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = init_audio,
    .fini = 0,
    .open = audio_open,
    .read = audio_read,
    .write = audio_write,
    .close = 0,
    .sread = 0,
    .swrite = 0,
    .readdir = 0,
};
