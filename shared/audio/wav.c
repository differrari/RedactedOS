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
    int16_t  first_sample;
}__attribute__((packed)) wav_header;


static uint32_t inline int16_to_uint32(int16_t sample) {
    return (INT16_MAX + sample) << 16;
}

bool wav_load(const char*path, wav_data *wav){
    file fd = {};
    FS_RESULT result = open_file(path, &fd);

    if (result != FS_RESULT_SUCCESS)
    {
        kprintf("[WAV] File not found: %s", path);
        return false;
    }

    wav->file_content.ptr = (uintptr_t)malloc(fd.size);
    wav->file_content.size = fd.size;
    wav_header* hdr = (wav_header*)wav->file_content.ptr;
    size_t read_size = read_file(&fd, (char*)hdr, fd.size);
    if (read_size != fd.size ||
        hdr->id != (uint32_t)'FFIR' ||
        hdr->wave_id != (uint32_t)'EVAW' ||
        hdr->format != 1 ||
        hdr->channels < 1 || hdr->channels > 2 ||
        hdr->sample_rate != 44100 ||
        hdr->sample_bits != 16 ||
        hdr->data_id != (uint32_t)'atad'
        )
    {
        // close_file(&fd)
        kprintf("[WAV] Incorrect file format %s", path);
        kprintf("=== Sizes       %i, %i", read_size, fd.size);
        kprintf("=== id          %x", hdr->id);
        kprintf("=== wave id     %x", hdr->wave_id);
        kprintf("=== format      %i", hdr->format_id);
        kprintf("=== channels    %i", hdr->channels);
        kprintf("=== sample rate %i", hdr->sample_rate);
        kprintf("=== sample_bits %i", hdr->sample_bits);
        kprintf("=== data id     %x", hdr->data_id);
        free((void*)wav->file_content.ptr, wav->file_content.size);
        return false;
    }

    wav->smpls_per_channel = hdr->data_size / (sizeof(int16_t) * hdr->channels);
    wav->channels = hdr->channels;
    wav->samples = &hdr->first_sample;

    //fclose(&fd);
    return true;
}
