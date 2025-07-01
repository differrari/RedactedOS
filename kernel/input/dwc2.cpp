#include "dwc2.hpp"
#include "async.h"
#include "console/kio.h"
#include "memory/page_allocator.h"
#include "std/string.h"
#include "memory/mmu.h"

#define DWC2_BASE 0xFE980000

dwc2_host_channel* DWC2Driver::get_channel(uint16_t channel){
    return (dwc2_host_channel *)(DWC2_BASE + 0x500 + (channel * 0x20));
}

uint8_t DWC2Driver::assign_channel(uint8_t device, uint8_t endpoint, uint8_t ep_type){
    uint8_t new_chan = ++next_channel;
    dwc2_host_channel *channel = get_channel(new_chan);
    channel->cchar = (device << 22) | (endpoint << 11) | (ep_type << 18);
    channel_map[device << 8 | endpoint] = new_chan;
    return new_chan;
}

bool DWC2Driver::init() {

    use_interrupts = false;

    dwc2 = (dwc2_regs*)DWC2_BASE;
    host = (dwc2_host*)(DWC2_BASE + 0x400);

    *(uint32_t*)(DWC2_BASE + 0xE00) = 0;//Power reset

    dwc2->grstctl |= 1;
    if (!wait(&dwc2->grstctl, 1, false, 2000)){
        kprintf("[DWC2] Failed to reset");
        return false;
    }

    dwc2->gusbcfg &= ~(1 << 30);//Device mode disable
    dwc2->gusbcfg |= (1 << 29);//Host mode enable

    dwc2->gahbcfg |= (1 << 5);//DMA

    dwc2->gintmsk = 0xffffffff;

    //Device setup0

    host->port |= (1 << 12);
    
    if (!wait(&host->port, 1, true, 2000)){
        kprintf("[DWC2] No device connected %x",host->port);
        return false;
    }

    if (!port_reset(&host->port)){
        kprintf("[DWC2] failed port reset %b",host->port);
        return false;
    }

    channel_map = IndexMap<uint16_t>(127);
    usb_manager = new USBManager(127); 

    kprintf("Port reset %x",host->port);

    port_speed = (host->port >> 17) & 0x3;
    kprintf("Port speed %i",port_speed);

    mem_page = alloc_page(0x1000, true, true, false);

    setup_device();

    register_device_memory(DWC2_BASE, DWC2_BASE);

    return true;
}

uint8_t DWC2Driver::address_device(){
    uint8_t new_address = ++next_address;
    request_sized_descriptor(0, 0, 0x0, 0x5, 0, new_address, 0, 0, 0);
    channel_map[new_address << 8] = assign_channel(new_address, 0, 0);
    get_channel(0)->splt = 0;
    return new_address;
}

bool DWC2Driver::make_transfer(dwc2_host_channel *channel, bool in, uint8_t pid, sizedptr data){
    channel->dma = data.ptr;
    uint16_t max_size = packet_size(port_speed);
    uint32_t pkt_count = (data.size + max_size - 1)/max_size;
    channel->xfer_size = (pkt_count << 19) | (pid << 29) | data.size;

    channel->cchar &= ~0x7FF;
    channel->cchar |= max_size;

    channel->intmask = 0xFFFFFFFF;
    channel->interrupt = 0;

    channel->cchar &= ~(1 << 15);
    channel->cchar |= ((in ? 1 : 0) << 15);
    
    channel->cchar &= ~(1 << 30);
    channel->cchar &= ~(1 << 31);
    channel->cchar |= (1 << 31); 

    if (!wait(&channel->interrupt, 1, true, 2000)){
        kprintf("[DWC2 error] Transfer timed out.");
        return false;
    }

    return true;
}

bool DWC2Driver::request_sized_descriptor(uint8_t address, uint8_t endpoint, uint8_t rType, uint8_t request, uint8_t type, uint16_t descriptor_index, uint16_t wIndex, uint16_t descriptor_size, void *out_descriptor){
    
    usb_setup_packet packet = {
        .bmRequestType = rType,
        .bRequest = request,
        .wValue = (type << 8) | descriptor_index,
        .wIndex = wIndex,
        .wLength = descriptor_size
    };

    bool is_in = (rType & 0x80) != 0;

    dwc2_host_channel *channel = address == 0 ? get_channel(0) : get_channel(channel_map[address << 8 | endpoint]);
    // kprintf("RT: %x R: %x V: %x I: %x L: %x",packet.bmRequestType,packet.bRequest,packet.wValue,packet.wIndex,packet.wLength);

    if (!make_transfer(channel, false, 0x3, (sizedptr){(uintptr_t)&packet, sizeof(uintptr_t)})){
        kprintf("[DWC2 error] Descriptor transfer failed setup stage");
        return false;
    }

    if (descriptor_size > 0){
        if (!make_transfer(channel, is_in, 0x2, (sizedptr){(uintptr_t)out_descriptor, descriptor_size})){
            kprintf("[DWC2 error] Descriptor transfer failed data stage");
            return false;
        }
    }

    if (!make_transfer(channel, !is_in, 0x2, (sizedptr){0, 0})){
        kprintf("[DWC2 error] Descriptor transfer failed status stage");
        return false;
    }

    return true;
}

bool DWC2Driver::configure_endpoint(uint8_t address, usb_endpoint_descriptor *endpoint, uint8_t configuration_value, xhci_device_types type){
    
    uint8_t ep_address = endpoint->bEndpointAddress;
    uint8_t ep_num = ep_address & 0x0F;
    uint8_t ep_dir = (ep_address & 0x80) >> 7;
    
    uint8_t ep_type = endpoint->bmAttributes & 0x03; // 0 = Control, 1 = Iso, 2 = Bulk, 3 = Interrupt
    
    kprintf("[DWC2] endpoint %i info. Direction %i type %i",ep_num, ep_dir, ep_type);
    
    //Configure endpoint
    request_sized_descriptor(address, 0, 0x00, 0x09, 0, configuration_value, 0, 0, 0);
    
    uint8_t conf;
    request_sized_descriptor(address, 0, 0x80, 0x08, 0, 0, 0, 1, &conf);
    
    if (!conf){
        kprintf("Failed to set configuration for device");
        return false;
    }
    
    dwc2_host_channel *channel = get_channel(channel_map[address << 8]);
    endpoint_channel = get_channel(assign_channel(address, ep_num, ep_type));
    if (channel->splt)
        endpoint_channel->splt = channel->splt;

    endpoint_channel->cchar &= ~(1 << 15);
    endpoint_channel->cchar |= (ep_dir << 15);

    endpoint_channel->cchar &= ~0x7FF;
    endpoint_channel->cchar |= endpoint->wMaxPacketSize;

    endpoint_channel->xfer_size = (1 << 19) | (0x3 << 29) | endpoint->wMaxPacketSize;

    uint8_t mc = endpoint->bInterval & 0x3;
    endpoint_channel->cchar &= ~(0b11 << 20);
    endpoint_channel->cchar |= (mc << 20);

    usb_manager->register_endpoint(address, ep_num, type, endpoint->wMaxPacketSize);

    return true;
}

bool DWC2Driver::poll(uint8_t address, uint8_t endpoint, void *out_buf, uint16_t size){
    dwc2_host_channel *endpoint_channel = get_channel(channel_map[(address << 8) | endpoint]);
    if (endpoint_channel->cchar & 1)
        return false;

    endpoint_channel->dma = (uintptr_t)out_buf;

    uint16_t max_size = endpoint_channel->cchar & 0x7FF;
    uint32_t pkt_count = (size + max_size - 1)/max_size;
    endpoint_channel->xfer_size = (pkt_count << 19) | (0x3 << 29) | size;

    endpoint_channel->intmask = 0xFFFFFFFF;

    endpoint_channel->interrupt = 0xFFFFFFFF;
    
    endpoint_channel->cchar &= ~(1 << 30);
    endpoint_channel->cchar &= ~(1 << 31);

    endpoint_channel->cchar |= (1 << 31); 

    if (!wait(&endpoint_channel->interrupt, 1, true, 10)){
        return false;
    }

    endpoint_channel->interrupt = 0xFFFFFFFF;
    endpoint_channel->cchar &= ~(1 << 31);

    return true;
}

void DWC2Driver::handle_hub_routing(uint8_t hub, uint8_t port){
    dwc2_host_channel *dev_channel = get_channel(0);
    dev_channel->splt = (1 << 31) | (1 << 16) | (hub << 7) | (port << 0);
}