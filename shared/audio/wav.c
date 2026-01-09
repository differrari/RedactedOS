#include "types.h"
#include "files/fs.h"
#include "std/memory.h"
#include "math/math.h"
#include "cuatro.h"
#include "wav.h"
#include "syscalls/syscalls.h"


// TODO: Handle non-trivial wav headers and other sample formats:
// https://web.archive.org/web/20100325183246/http://www-mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/WAVE.html

typedef struct wav_header {
    uint32_t id;
    uint32_t fSize;
    uint32_t wave_id;
}__attribute__((packed)) wav_header;

typedef struct wav_chunk_hdr {
    uint32_t ck_id;
    uint32_t ck_size;
}__attribute__((packed)) wav_chunk_hdr;

typedef struct wav_format_chunk {
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t idk;
    uint16_t align;
    uint16_t sample_bits;
}__attribute__((packed)) wav_format_chunk;

static void transform_16bit(wav_format_chunk *fmt_chunk, uint32_t data_size, audio_samples* audio, uint32_t upsample, file* fd){
    int16_t* tbuf = (int16_t*)malloc(data_size);
    readf(fd, (char*)tbuf, data_size);
    audio->samples.size = data_size * upsample;
    audio->samples.ptr = (uintptr_t)malloc(audio->samples.size);
    audio->smpls_per_channel = audio->samples.size / (sizeof(int16_t) * fmt_chunk->channels);
    audio->channels = fmt_chunk->channels;
    uint32_t samples_remaining = data_size / sizeof(int16_t);
    int16_t* source = tbuf;
    int16_t* dest = (int16_t*)audio->samples.ptr;
    while (samples_remaining-- > 0){
        for (int i = upsample; i > 0; i--){
            *dest++ = *source;  // TODO: interpolate
        }
        ++source;
    }
    free_sized(tbuf, data_size);
}

static void transform_8bit(wav_format_chunk *fmt_chunk, uint32_t data_size, audio_samples* audio, uint32_t upsample, file* fd){
    uint8_t* tbuf = (uint8_t*)malloc(data_size);
    readf(fd, (char*)tbuf, data_size);
    audio->samples.size = data_size * upsample * sizeof(int16_t);
    audio->samples.ptr = (uintptr_t)malloc(audio->samples.size);
    audio->smpls_per_channel = audio->samples.size / (sizeof(int16_t) * fmt_chunk->channels);
    audio->channels = fmt_chunk->channels;
    uint32_t samples_remaining = data_size;
    uint8_t* source = tbuf;
    int16_t* dest = (int16_t*)audio->samples.ptr;
    while (samples_remaining-- > 0){
        int16_t sample = (int16_t)((*source++ - 128) * 256);  // 8-bit source is offset binary 
        for (int i = upsample; i > 0; i--){
            *dest++ = sample;  // TODO: interpolate
        }
    }
    free_sized(tbuf, data_size);
}

bool wav_load_as_int16(const char* path, audio_samples* audio){
    file fd = {};

    if (FS_RESULT_SUCCESS != openf(path, &fd))
    {
        printf("[WAV] Could not open file: %s", path);
        return false;
    }

    wav_header hdr = {};
    size_t read_size = readf(&fd, (char*)&hdr, sizeof(wav_header));
    if (read_size != sizeof(wav_header) ||
        hdr.id != 0x46464952 ||      // 'RIFF'
        hdr.wave_id != 0x45564157){ // 'WAVE'
        printf("[WAV] Non-WAV file format %s", path);
        return false;
    }

    wav_format_chunk fmt_chunk = {};
    while (fd.cursor < fd.size){
        wav_chunk_hdr ch_hdr = {};
        read_size = readf(&fd, (char*)&ch_hdr, sizeof(wav_chunk_hdr));
        switch (ch_hdr.ck_id){
            case 0x20746D66://fmt format
                {
                    read_size = readf(&fd, (char*)&fmt_chunk, ch_hdr.ck_size);
                    if (fmt_chunk.channels < 1 || fmt_chunk.channels > 2 ||
                        fmt_chunk.sample_rate > 44100 ||
                        (44100 % fmt_chunk.sample_rate != 0) ||
                        (fmt_chunk.sample_bits != 8 && fmt_chunk.sample_bits != 16)
                        )
                    {
                        closef(&fd);
                        printf("[WAV] Unsupported file format %s", path);
                        printf("=== Sizes       %i, %i", read_size, fd.size);
                        printf("=== id          %x", hdr.id);
                        printf("=== wave id     %x", hdr.wave_id);
                        printf("=== format      %x", ch_hdr.ck_id);
                        printf("=== channels    %i", fmt_chunk.channels);
                        printf("=== sample rate %i", fmt_chunk.sample_rate);
                        printf("=== sample_bits %i", fmt_chunk.sample_bits);
                        return false;
                    }
                    break;
                }
            case 0x61746164://data
            {
                uint32_t upsample = 44100 / fmt_chunk.sample_rate;
                bool result = true;
                if (fmt_chunk.sample_bits == 16 && upsample == 1){
                    // simple case: slurp samples direct from file to wav buffer
                    audio->samples.size = ch_hdr.ck_size;
                    audio->samples.ptr = (uintptr_t)malloc(audio->samples.size);
                    readf(&fd, (char*)audio->samples.ptr, audio->samples.size);
                    audio->smpls_per_channel = ch_hdr.ck_size / (sizeof(int16_t) * fmt_chunk.channels);
                    audio->channels = fmt_chunk.channels;
                } else if (fmt_chunk.sample_bits == 16){
                    transform_16bit(&fmt_chunk, ch_hdr.ck_size, audio, upsample, &fd);
                } else if (fmt_chunk.sample_bits == 8){
                    transform_8bit(&fmt_chunk, ch_hdr.ck_size, audio, upsample, &fd);
                } else{
                    result = false;
                }
                
                closef(&fd);
                return result;
            }
            default:
                fd.cursor += ch_hdr.ck_size;
                break;
        }
    }
    
    return false;
}
