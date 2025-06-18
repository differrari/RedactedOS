#include "../net_driver.hpp"
#include "virtio/virtio_pci.h"

class VirtioNetDriver : public NetDriver {
public:
    static VirtioNetDriver* try_init();

    VirtioNetDriver(){}

    bool init() override;

    void handle_interrupt() override;

    void send_packet(void* packet, size_t size) override;

    void enable_verbose() override;

    ~VirtioNetDriver() = default;

private:
    bool verbose = false;
    uint16_t last_used_receive_idx = 0;
    virtio_device vnp_net_dev;
};