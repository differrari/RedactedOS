#include "OutputAudioDevice.hpp"
#include "memory/page_allocator.h"

#define BUF_SIZE PAGE_SIZE * 2

void OutputAudioDevice::populate(){
    buffer = (uintptr_t)palloc(BUF_SIZE, MEM_PRIV_KERNEL, MEM_RW, true);
}

sizedptr OutputAudioDevice::request_buffer(){
    sizedptr ptr = (sizedptr){buffer + write_ptr + header_size, buf_size};
    write_ptr += packet_size;
    if (write_ptr + packet_size >= BUF_SIZE) write_ptr = 0;
    return ptr;
}

void OutputAudioDevice::submit_buffer(AudioDriver *driver){
    if (read_ptr == write_ptr) return;
    driver->send_buffer((sizedptr){buffer + read_ptr, packet_size});
    read_ptr += packet_size;
    if (read_ptr + packet_size >= BUF_SIZE) read_ptr = 0;
}
