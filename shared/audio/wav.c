#include "types.h"
#include "filesystem/filesystem.h"
#include "memory/talloc.h"
#include "std/memory.h"
#include "math/math.h"
#include "console/kio.h"
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


void transform_16bit(wav_header *hdr, wav_data *wav, uint32_t upsample, file *fd){
    wav->samples.size = hdr->data_size * upsample;
    wav->samples.ptr = (uintptr_t)malloc(wav->samples.size);
    int16_t* tbuf = (int16_t*)malloc(hdr->data_size);
    read_file(fd, (char*)tbuf, hdr->data_size);
    wav->smpls_per_channel = (wav->samples.size) / (sizeof(int16_t) * hdr->channels);
    wav->channels = hdr->channels;
    uint32_t samples_remaining = hdr->data_size / sizeof(int16_t);
    int16_t* source = tbuf;
    int16_t* dest = (int16_t*)wav->samples.ptr;
    while (samples_remaining-- > 0){
        for (int i = upsample; i > 0; i--){
            *dest++ = *source;  // TODO: interpolate
        }
        ++source;
    }
    free(tbuf, hdr->data_size);
}

void transform_8bit(wav_header *hdr, wav_data *wav, uint32_t upsample, file *fd){
    uint8_t* tbuf = (int8_t*)malloc(hdr->data_size);
    read_file(fd, (char*)tbuf, hdr->data_size);
    wav->samples.size = hdr->data_size * upsample * 2;
    wav->samples.ptr = (uintptr_t)malloc(wav->samples.size);
    wav->smpls_per_channel = wav->samples.size / (sizeof(int16_t) * hdr->channels);
    wav->channels = hdr->channels;
    uint32_t samples_remaining = hdr->data_size;
    uint8_t* source = tbuf;
    int16_t* dest = (int16_t*)wav->samples.ptr;
    while (samples_remaining-- > 0){
        int16_t sample = (int16_t)((*source++ - 128) * 256);  // offset binary to signed
        for (int i = upsample; i > 0; i--){
            *dest++ = sample;  // TODO: interpolate
        }
    }
    free(tbuf, hdr->data_size);
}

bool wav_load_as_int16(const char*path, wav_data *wav){
    file fd = {};
    FS_RESULT result = open_file(path, &fd);

    if (result != FS_RESULT_SUCCESS)
    {
        kprintf("[WAV] File not found: %s", path);
        return false;
    }

    wav_header hdr = {};
    size_t read_size = read_file(&fd, (char*)&hdr, sizeof(wav_header));
    if (read_size != sizeof(wav_header) ||
        hdr.id != (uint32_t)'FFIR' ||
        hdr.wave_id != (uint32_t)'EVAW' ||
        hdr.format != 1 ||
        hdr.channels < 1 || hdr.channels > 2 ||
        hdr.sample_rate > 44100 ||
        (44100 % hdr.sample_rate != 0) ||
        (hdr.sample_bits != 8 && hdr.sample_bits != 16) ||
        hdr.data_id != (uint32_t)'atad' ||
        fd.size < hdr.data_size + sizeof(wav_header) ||
        hdr.data_size == 0
        )
    {
        // close_file(&fd)
        kprintf("[WAV] Unsupported file format %s", path);
        kprintf("=== Sizes       %i, %i, %i", read_size, fd.size, hdr.data_size);
        kprintf("=== id          %x", hdr.id);
        kprintf("=== wave id     %x", hdr.wave_id);
        kprintf("=== format      %x", hdr.format_id);
        kprintf("=== channels    %i", hdr.channels);
        kprintf("=== sample rate %i", hdr.sample_rate);
        kprintf("=== sample_bits %i", hdr.sample_bits);
        kprintf("=== data id     %x", hdr.data_id);
        return false;
    }

    uint32_t upsample = 44100 / hdr.sample_rate; // for up-sampling

    if (hdr.sample_bits == 16 && upsample == 1){
        // simple case: slurp samples direct from file to wav buffer
        wav->samples.size = hdr.data_size;
        wav->samples.ptr = (uintptr_t)malloc(wav->samples.size);
        read_file(&fd, (char*)wav->samples.ptr, wav->samples.size);
        wav->smpls_per_channel = hdr.data_size / (sizeof(int16_t) * hdr.channels);
        wav->channels = hdr.channels;
    }else if (hdr.sample_bits == 16){
        transform_16bit(&hdr, wav, upsample, &fd);
    }else if (hdr.sample_bits == 8){
        transform_8bit(&hdr, wav, upsample, &fd);
    }else{
        //close_file(&fd);
        return false;
    }
    //close_file(&fd);
kprintf("===Samples size %i", wav->samples.size);
kprintf("===Per channel  %i", wav->smpls_per_channel);
kprintf("===Channels     %i", wav->channels);
    return true;
}

