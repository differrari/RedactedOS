#include "graphics.h"
#include "console/kio.h"

#include "drivers/virtio_gpu_pci/virtio_gpu_pci.hpp"
#include "drivers/ramfb_driver/ramfb.hpp"
#include "drivers/videocore/videocore.hpp"

#include "std/std.hpp"
#include "hw/hw.h"

#include "tres.h"

static gpu_size screen_size;
static bool _gpu_ready;

GPUDriver *gpu_driver;

bool gpu_init(){
    kprint("Initializing GPU");
    gpu_size preferred_screen_size = {1920,1080};
    if (BOARD_TYPE == 1){
        if (VirtioGPUDriver *vgd = VirtioGPUDriver::try_init(preferred_screen_size)){
            gpu_driver = vgd;
        } else if (RamFBGPUDriver *rfb = RamFBGPUDriver::try_init(preferred_screen_size)){
            gpu_driver = rfb;
        }
    } else if (BOARD_TYPE == 2){
        gpu_driver = VideoCoreGPUDriver::try_init(preferred_screen_size);
    } else return false;
    screen_size = preferred_screen_size;
    _gpu_ready = true;
    kprintf("Selected and initialized GPU %x", (uintptr_t)gpu_driver);

    //TODO: make window manager its own module and access the driver by exposing it through /dev/gpu
    init_window_manager((uintptr_t)gpu_driver);

    return true;
}

bool gpu_ready(){
    return gpu_driver != nullptr && _gpu_ready;
}

void gpu_flush(){
    if (!gpu_ready())
        return;
    gpu_driver->flush();
}
void gpu_clear(color color){
    if (!gpu_ready())
        return;
    gpu_driver->clear(color);
}

void gpu_draw_pixel(gpu_point p, color color){
    if (!gpu_ready())
        return;
    gpu_driver->draw_pixel(p.x,p.y,color);
}

void gpu_fill_rect(gpu_rect r, color color){
    if (!gpu_ready())
        return;
   gpu_driver->fill_rect(r.point.x,r.point.y,r.size.width,r.size.height,color);
}

void gpu_draw_line(gpu_point p0, gpu_point p1, uint32_t color){
    if (!gpu_ready())
        return;
    gpu_driver->draw_line(p0.x,p0.y,p1.x,p1.y,color);
}
void gpu_draw_char(gpu_point p, char c, uint32_t scale, uint32_t color){
    if (!gpu_ready())
        return;
    gpu_driver->draw_char(p.x,p.y,c,scale,color);
}

void gpu_draw_string(string s, gpu_point p, uint32_t scale, uint32_t color){
    if (!gpu_ready())
        return;
    gpu_driver->draw_string(s, p.x, p.y, scale, color);
}

uint32_t gpu_get_char_size(uint32_t scale){
    if (!gpu_ready())
        return 0;
    return gpu_driver->get_char_size(scale);
}

gpu_size gpu_get_screen_size(){
    if (!gpu_ready())
        return (gpu_size){0,0};
    return gpu_driver->get_screen_size();
}

draw_ctx* gpu_get_ctx(){
    if (!gpu_ready()) return 0;
    return gpu_driver->get_ctx();
}

void gpu_setup_cursor(gpu_point initial_loc){
    gpu_driver->setup_cursor();
    gpu_driver->update_cursor(initial_loc.x, initial_loc.y, true);
}

void gpu_update_cursor(gpu_point new_loc, bool full){
    gpu_driver->update_cursor(new_loc.x, new_loc.y, full);
}

void gpu_set_cursor_pressed(bool pressed){
    gpu_driver->set_cursor_pressed(pressed);
}

driver_module graphics_module = {
    .name = "graphics",
    .mount = "/dev/graph",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = gpu_init,
    .fini = 0,
    .open = 0,
    .read = 0,
    .write = 0,
    .seek = 0,
    .readdir = 0
};