#include "../net_driver.hpp"
#include "virtio/virtio_pci.h"
#include "net/network_types.h"

class VirtioNetDriver : public NetDriver {
public:
    static VirtioNetDriver* try_init();

    VirtioNetDriver(){}

    bool init() override;

    sizedptr handle_receive_packet() override;

    void enable_verbose() override;

    void send_packet(NetProtocol protocol, uint16_t port, network_connection_ctx *destination, void* payload, uint16_t payload_len) override;

    ~VirtioNetDriver() = default;

private:
    bool verbose = false;
    uint16_t last_used_receive_idx = 0;
    virtio_device vnp_net_dev;
    network_connection_ctx connection_context;
};