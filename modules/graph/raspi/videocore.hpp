#pragma once 

#include "common/framebuffer/fbgpu.hpp"
#include "exceptions/exception_handler.h"

class VideoCoreGPUDriver : public FBGPUDriver {
public:
    static VideoCoreGPUDriver* try_init(gpu_size preferred_screen_size);
    VideoCoreGPUDriver(){}
    bool init(gpu_size preferred_screen_size) override;

    gpu_size get_screen_size() override;
    void update_gpu_fb() override;
    
    ~VideoCoreGPUDriver() = default;
    
protected:
    uint32_t last_offset = 0;
    bool mailbox_fallback = false;//Used if swapping framebuffers fails. Pi 5
};