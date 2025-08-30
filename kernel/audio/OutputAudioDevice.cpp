#include "OutputAudioDevice.hpp"
#include "memory/page_allocator.h"

void OutputAudioDevice::populate(){
    buffer = (uintptr_t)palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, true);
}

sizedptr OutputAudioDevice::get_buffer(){
    sizedptr ptr = (sizedptr){buffer + write_ptr + header_size, buf_size};
    write_ptr += packet_size;
    if (write_ptr + packet_size >= PAGE_SIZE) write_ptr = 0;
    return ptr;
}

void OutputAudioDevice::submit_buffer(AudioDriver *driver){
    driver->send_buffer((sizedptr){buffer + read_ptr, packet_size});
    read_ptr += packet_size;
    if (read_ptr + packet_size >= PAGE_SIZE) read_ptr = 0;
}