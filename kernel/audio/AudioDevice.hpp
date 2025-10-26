#pragma once

#include "types.h"

class AudioDriver {
public:
    virtual void send_buffer(sizedptr buf) = 0;
};

class AudioDevice {
public:
    virtual void populate() = 0;
    virtual sizedptr request_buffer() = 0;
    virtual void submit_buffer(AudioDriver *driver) = 0;
    uint32_t stream_id;
    uint32_t rate;
    uint8_t channels;
    uintptr_t buffer;
    uintptr_t write_ptr;
    uintptr_t read_ptr;
    size_t buf_size;
    size_t header_size;
    size_t packet_size;
};