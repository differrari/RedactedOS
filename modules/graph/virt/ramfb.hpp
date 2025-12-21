#pragma once 

#include "common/framebuffer/fbgpu.hpp"
#include "exceptions/exception_handler.h"
#include "fw/fw_cfg.h"

class RamFBGPUDriver : public FBGPUDriver {
public:
    static RamFBGPUDriver* try_init(gpu_size preferred_screen_size);
    RamFBGPUDriver(){}
    bool init(gpu_size preferred_screen_size) override;

    gpu_size get_screen_size() override;
    void update_gpu_fb()  override;

    ~RamFBGPUDriver() = default;
    
private: 
    fw_cfg_file file;
};