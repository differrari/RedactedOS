#include "graphics.h"
#include "console/kio.h"

#include "graph/drivers/virtio_gpu_pci/virtio_gpu_pci_driver.h"
#include "graph/drivers/ramfb_driver/ramfb_driver.h"

static size screen_size;
static bool _gpu_ready;

typedef enum {
    NONE,
    VIRTIO_GPU_PCI,
    RAMFB
} SupportedGPU;

SupportedGPU chosen_GPU;

void gpu_init(size preferred_screen_size){
    if (vgp_init(preferred_screen_size.width,preferred_screen_size.height))
        chosen_GPU = VIRTIO_GPU_PCI;
    else if (rfb_init(preferred_screen_size.width,preferred_screen_size.height))
        chosen_GPU = RAMFB;
    screen_size = preferred_screen_size;
    _gpu_ready = true;
    kprintf("Selected and initialized GPU %i",chosen_GPU);
}

bool gpu_ready(){
    return chosen_GPU != NONE && _gpu_ready;
}

void gpu_flush(){
    if (!gpu_ready())
        return;
    switch (chosen_GPU) {
        case VIRTIO_GPU_PCI:
            vgp_flush();
            break;
        case RAMFB:
            rfb_flush();
            break;
        default:
            break;
    }
}
void gpu_clear(color color){
    if (!gpu_ready())
        return;
    switch (chosen_GPU) {
        case VIRTIO_GPU_PCI:
            vgp_clear(color);
            break;
        case RAMFB:
            rfb_clear(color);
        default:
            break;
    }
}

void gpu_draw_pixel(point p, color color){
    if (!gpu_ready())
        return;
    switch (chosen_GPU) {
        case VIRTIO_GPU_PCI:
            vgp_draw_pixel(p.x,p.y,color);
        break;
        case RAMFB:
            rfb_draw_pixel(p.x,p.y,color);
        break;
        default:
        break;
    }
}

void gpu_fill_rect(rect r, color color){
    if (!gpu_ready())
        return;
    switch (chosen_GPU) {
        case VIRTIO_GPU_PCI:
            vgp_fill_rect(r.point.x,r.point.y,r.size.width,r.size.height,color);
        break;
        case RAMFB:
            rfb_fill_rect(r.point.x,r.point.y,r.size.width,r.size.height,color);
        break;
        default:
        break;
    }
}

void gpu_draw_line(point p0, point p1, uint32_t color){
    if (!gpu_ready())
        return;
    switch (chosen_GPU) {
        case VIRTIO_GPU_PCI:
            vgp_draw_line(p0.x,p0.y,p1.x,p1.y,color);
        break;
        case RAMFB:
            rfb_draw_line(p0.x,p0.y,p1.x,p1.y,color);
        break;
        default:
        break;
    }
}
void gpu_draw_char(point p, char c, uint32_t scale, uint32_t color){
    if (!gpu_ready())
        return;
    switch (chosen_GPU) {
        case VIRTIO_GPU_PCI:
            vgp_draw_char(p.x,p.y,c,color);
        break;
        case RAMFB:
            rfb_draw_char(p.x,p.y,c,scale,color);
        break;
        default:
        break;
    }
}

void gpu_draw_string(kstring s, point p, uint32_t scale){
    if (!gpu_ready())
        return;
    switch (chosen_GPU) {
        case RAMFB:
            rfb_draw_string(s, p.x,p.y, scale);
        break;
        default:
        break;
    }
}

uint32_t gpu_get_char_size(uint32_t scale){
    if (!gpu_ready())
        return 0;
    switch (chosen_GPU) {
        case RAMFB:
            rfb_get_char_size(scale);
        break;
        default:
        break;
    }
}

size gpu_get_screen_size(){
    if (!gpu_ready())
        return (size){0,0};
    return screen_size;
}