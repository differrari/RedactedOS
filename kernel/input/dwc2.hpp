#include "types.h"
#include "keypress.h"
#include "xhci_types.h"
#include "std/indexmap.hpp"

typedef struct {
    uint32_t gotgctl;
    uint32_t gotgint;
    uint32_t gahbcfg;
    uint32_t gusbcfg;
    uint32_t grstctl;
    uint32_t gintsts;
    uint32_t gintmsk;
    uint32_t grxstsr;
    uint32_t grxstsp;
    uint32_t grxfsiz;
    uint32_t gnptxfsiz;
    uint32_t gnptxsts;
} dwc2_regs;

typedef struct {
    uint32_t hstcfg;
    uint32_t frminterval;
    uint32_t frmnum;
    uint32_t rsvd;
    uint32_t ptx_fifo;
    uint32_t allchan_int;
    uint32_t allchan_int_mask;
    uint32_t frmlst_base;
    uint32_t rsvd2[8];
    uint32_t port;
} dwc2_host;

typedef struct {
    uint32_t cchar;
    uint32_t splt;
    uint32_t interrupt;
    uint32_t intmask;
    uint32_t xfer_size;
    uint32_t dma;
    uint32_t reserved[2];
    uint32_t buf;
} dwc2_host_channel;

class DWC2Driver {
public:
    bool init();
    bool request_descriptor(uint8_t address, uint8_t endpoint, uint8_t rType, uint8_t request, uint8_t type, uint16_t index, uint16_t wIndex, void *out_descriptor);
    bool request_sized_descriptor(uint8_t address, uint8_t endpoint, uint8_t rType, uint8_t request, uint8_t type, uint16_t descriptor_index, uint16_t wIndex, uint16_t descriptor_size, void *out_descriptor);
    uint16_t packet_size(uint16_t speed);
    bool get_configuration(uint8_t address);
    void hub_enumerate(uint8_t address);
    bool port_reset(uint32_t *port);
    bool setup_device();
    uint8_t address_device();
    bool poll_interrupt_in();
    bool configure_endpoint(uint8_t address, usb_endpoint_descriptor *endpoint, uint8_t configuration_value);
    // bool configure_endpoint(uint8_t address, usb_endpoint_descriptor *endpoint, uint8_t configuration_value);
private:
    dwc2_host_channel* get_channel(uint16_t channel);
    uint8_t assign_channel(uint8_t device, uint8_t endpoint, uint8_t ep_type);
    bool make_transfer(dwc2_host_channel *channel, bool in, uint8_t pid, sizedptr data);
    dwc2_host_channel* get_channel(uint8_t device, uint8_t endpoint);
    void *mem_page;
    uint16_t port_speed;
    void *TEMP_input_buffer;
    dwc2_regs *dwc2;
    dwc2_host *host;
    uint8_t next_channel;
    uint8_t next_address;
    dwc2_host_channel *endpoint_channel;
    keypress last_keypress;
    int repeated_keypresses = 0;
    IndexMap<uint8_t> channel_map;
};