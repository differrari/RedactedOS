#include "framebuffer.h"
#include "virtio-gpu-pci/virtio_gpu_pci_driver.h"
#include "uart.h"

typedef enum {
    NONE,
    VIRTIO_GPU_PCI,
    RAMFB
} SupportedGPU;

SupportedGPU chosen_GPU;

void gpu_init(){
    if (vgp_init())
        chosen_GPU = VIRTIO_GPU_PCI;
    uart_puts("Selected and initialized GPU ");
    uart_puthex(chosen_GPU);
    uart_putc('\n');
}

void gpu_flush(){
    switch (chosen_GPU) {
        case VIRTIO_GPU_PCI:
            vgp_transfer_to_host();
            vgp_flush();
            break;
        default:
            break;
    }
}
void gpu_clear(uint32_t color){
    switch (chosen_GPU) {
        case VIRTIO_GPU_PCI:
            vgp_clear(color);
            break;
        default:
            break;
    }
}