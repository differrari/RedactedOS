#include "types.h"
#include "files/fs.h"
#include "memory/talloc.h"
#include "std/memory.h"
#include "math/math.h"
#include "console/kio.h"
#include "cuatro.h"
#include "wav.h"
#include "syscalls/syscalls.h"


// TODO: Handle non-trivial wav headers and other sample formats:
// https://web.archive.org/web/20100325183246/http://www-mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/WAVE.html

typedef struct wav_header {
    uint32_t id;
    uint32_t fSize;
    uint32_t wave_id;
    uint32_t format_id;
    uint32_t format_size;
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t idk;
    uint16_t align;
    uint16_t sample_bits;
    uint32_t data_id;
    uint32_t data_size;
}__attribute__((packed)) wav_header;


static void transform_16bit(wav_header* hdr, audio_samples* audio, uint32_t upsample, file* fd){
    int16_t* tbuf = (int16_t*)malloc(hdr->data_size);
    fread(fd, (char*)tbuf, hdr->data_size);
    audio->samples.size = hdr->data_size * upsample;
    audio->samples.ptr = (uintptr_t)malloc(audio->samples.size);
    audio->smpls_per_channel = audio->samples.size / (sizeof(int16_t) * hdr->channels);
    audio->channels = hdr->channels;
    audio->secs = audio->smpls_per_channel / 44100.f;
    uint32_t samples_remaining = hdr->data_size / sizeof(int16_t);
    int16_t* source = tbuf;
    int16_t* dest = (int16_t*)audio->samples.ptr;
    while (samples_remaining-- > 0){
        for (int i = upsample; i > 0; i--){
            *dest++ = *source;  // TODO: interpolate
        }
        ++source;
    }
    free(tbuf, hdr->data_size);
}

static void transform_8bit(wav_header* hdr, audio_samples* audio, uint32_t upsample, file* fd){
    uint8_t* tbuf = (uint8_t*)malloc(hdr->data_size);
    fread(fd, (char*)tbuf, hdr->data_size);
    audio->samples.size = hdr->data_size * upsample * sizeof(int16_t);
    audio->samples.ptr = (uintptr_t)malloc(audio->samples.size);
    audio->smpls_per_channel = audio->samples.size / (sizeof(int16_t) * hdr->channels);
    audio->channels = hdr->channels;
    audio->secs = audio->smpls_per_channel / 44100.f;
    uint32_t samples_remaining = hdr->data_size;
    uint8_t* source = tbuf;
    int16_t* dest = (int16_t*)audio->samples.ptr;
    while (samples_remaining-- > 0){
        int16_t sample = (int16_t)((*source++ - 128) * 256);  // 8-bit source is offset binary 
        for (int i = upsample; i > 0; i--){
            *dest++ = sample;  // TODO: interpolate
        }
    }
    free(tbuf, hdr->data_size);
}

bool wav_load_as_int16(const char* path, audio_samples* audio){
    file fd = {};

    if (FS_RESULT_SUCCESS != fopen(path, &fd))
    {
        //printf("[WAV] File not found: %s", path);
        return false;
    }

    wav_header hdr = {};
    size_t read_size = fread(&fd, (char*)&hdr, sizeof(wav_header));
    if (read_size != sizeof(wav_header) ||
        hdr.id != 0x46464952 ||      // 'RIFF'
        hdr.wave_id != 0x45564157 || // 'WAVE'
        hdr.format != 1 ||
        hdr.channels < 1 || hdr.channels > 2 ||
        hdr.sample_rate > 44100 ||
        (44100 % hdr.sample_rate != 0) ||
        (hdr.sample_bits != 8 && hdr.sample_bits != 16) ||
        hdr.data_id != 0x61746164 || // 'data'
        fd.size < hdr.data_size + sizeof(wav_header) ||
        hdr.data_size == 0
        )
    {
        fclose(&fd);
        printf("[WAV] Unsupported file format %s", path);
        printf("=== Sizes       %i, %i, %i", read_size, fd.size, hdr.data_size);
        printf("=== id          %x", hdr.id);
        printf("=== wave id     %x", hdr.wave_id);
        printf("=== format      %x", hdr.format_id);
        printf("=== channels    %i", hdr.channels);
        printf("=== sample rate %i", hdr.sample_rate);
        printf("=== sample_bits %i", hdr.sample_bits);
        printf("=== data id     %x", hdr.data_id);
        return false;
    }

    uint32_t upsample = 44100 / hdr.sample_rate;
    bool result = true;

    if (hdr.sample_bits == 16 && upsample == 1){
        // simple case: slurp samples direct from file to wav buffer
        audio->samples.size = hdr.data_size;
        audio->samples.ptr = (uintptr_t)malloc(audio->samples.size);
        fread(&fd, (char*)audio->samples.ptr, audio->samples.size);
        audio->smpls_per_channel = hdr.data_size / (sizeof(int16_t) * hdr.channels);
        audio->channels = hdr.channels;
        audio->secs = audio->smpls_per_channel / 44100.f;
    }else if (hdr.sample_bits == 16){
        transform_16bit(&hdr, audio, upsample, &fd);
    }else if (hdr.sample_bits == 8){
        transform_8bit(&hdr, audio, upsample, &fd);
    }else{
        result = false;
    }
    fclose(&fd);
    return result;
}
