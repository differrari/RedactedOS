#include "../net_driver.hpp"
#include "virtio/virtio_pci.h"
#include "net/network_types.h"

#define VIRTIO_NET_ID 0x1000

class VirtioNetDriver : public NetDriver {
public:
    static VirtioNetDriver* try_init();

    VirtioNetDriver(){}

    bool init() override;

    sizedptr allocate_packet(size_t size) override;

    sizedptr handle_receive_packet(void* buffer) override;
    void handle_sent_packet() override;

    void enable_verbose() override;

    void send_packet(sizedptr packet) override;

    void get_mac(network_connection_ctx *context) override;

    ~VirtioNetDriver() = default;

private:
    bool verbose = false;
    uint16_t last_used_receive_idx = 0;
    uint16_t last_used_sent_idx = 0;
    virtio_device vnp_net_dev;
};