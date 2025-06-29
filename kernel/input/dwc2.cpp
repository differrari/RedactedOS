#include "dwc2.hpp"
#include "async.h"
#include "console/kio.h"
#include "memory/page_allocator.h"
#include "xhci_types.h"//TODO: split into xhci and USB types
#include "std/string.h"

#define DWC2_BASE 0xFE980000

dwc2_host_channel* DWC2Driver::get_channel(uint16_t channel){
    return (dwc2_host_channel *)(DWC2_BASE + 0x500 + (channel * 0x20));
}

void DWC2Driver::assign_channel(dwc2_host_channel* channel, uint8_t device, uint8_t endpoint, uint8_t ep_type){
    channel->cchar = (device << 22) | (endpoint << 11) | (ep_type << 18);
}

uint16_t DWC2Driver::packet_size(uint16_t speed){
    switch (speed) {
        case 2: return 8;//Low
        case 1:
        case 3: return 64;//High & full
        case 4:
        case 5:
        default: return 512;//Super & Super Plus & Default
    }
}

bool DWC2Driver::init() {

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

    delay(200);

    dwc2->gahbcfg |= (1 << 5);//DMA

    dwc2->gintmsk = 0xffffffff;

    host->port |= (1 << 12);
    
    if (!wait(&host->port, 1, true, 2000)){
        kprintf("[DWC2] No device connected %x",host->port);
        return false;
    }

    delay(100);

    host->port &= ~0b101110;
    host->port |= (1 << 8);

    delay(50);

    host->port &= ~0b101110;
    host->port &= ~(1 << 8);

    wait(&host->port, (1 << 8), false, 2000);

    kprintf("Port reset %x",host->port);

    port_speed = (host->port >> 17) & 0x3;
    kprintf("Port speed %i",port_speed);

    dwc2_host_channel *channel = get_channel(0);

    assign_channel(channel, 0, 0, 0);

    mem_page = alloc_page(0x1000, true, true, false);
    usb_device_descriptor* descriptor = (usb_device_descriptor*)allocate_in_page(mem_page, sizeof(usb_device_descriptor), ALIGN_64B, true, true);
    
    if (!request_descriptor(0x80, USB_DEVICE_DESCRIPTOR, 0, 0, descriptor)){
        kprintf_raw("[DWC2 error] failed to get device descriptor");
        return false;
    }

    usb_string_language_descriptor* lang_desc = (usb_string_language_descriptor*)allocate_in_page(mem_page, sizeof(usb_string_language_descriptor), ALIGN_64B, true, true);

    bool use_lang_desc = true;

    if (!request_descriptor(0x80, USB_STRING_DESCRIPTOR, 0, 0, lang_desc)){
        kprintf_raw("[DWC2 warning] failed to get language descriptor");
        use_lang_desc = false;
    }

    kprintf("[DWC2] Vendor %x",descriptor->idVendor);
    kprintf("[DWC2] Product %x",descriptor->idProduct);
    kprintf("[DWC2] USB version %x",descriptor->bcdUSB);
    kprintf("[DWC2] EP0 Max Packet Size: %x", descriptor->bMaxPacketSize0);
    kprintf("[DWC2] Configurations: %x", descriptor->bNumConfigurations);
    if (use_lang_desc){
        //TODO: we want to maintain the strings so we can have USB device information, and rework it to silece the alignment warning
        uint16_t langid = lang_desc->lang_ids[0];
        usb_string_descriptor* prod_name = (usb_string_descriptor*)allocate_in_page(mem_page, sizeof(usb_string_descriptor), ALIGN_64B, true, true);
        if (request_descriptor(0x80, USB_STRING_DESCRIPTOR, descriptor->iProduct, langid, prod_name)){
            char name[128];
            if (utf16tochar(prod_name->unicode_string, name, sizeof(name))) {
                kprintf("[DWC2 device] Product name: %s", (uint64_t)name);
            }
        }
        usb_string_descriptor* man_name = (usb_string_descriptor*)allocate_in_page(mem_page, sizeof(usb_string_descriptor), ALIGN_64B, true, true);
        if (request_descriptor(0x80, USB_STRING_DESCRIPTOR, descriptor->iManufacturer, langid, man_name)){
            char name[128];
            if (utf16tochar(man_name->unicode_string, name, sizeof(name))) {
                kprintf("[DWC2 device] Manufacturer name: %s", (uint64_t)name);
            }
        }
        usb_string_descriptor* ser_name = (usb_string_descriptor*)allocate_in_page(mem_page, sizeof(usb_string_descriptor), ALIGN_64B, true, true);
        if (request_descriptor(0x80, USB_STRING_DESCRIPTOR, descriptor->iSerialNumber, langid, ser_name)){
            char name[128];
            if (utf16tochar(ser_name->unicode_string, name, sizeof(name))) {
                kprintf("[DWC2 device] Serial: %s", (uint64_t)name);
            }
        }
    }

    get_configuration();

    return true;
}

bool DWC2Driver::make_transfer(dwc2_host_channel *channel, bool in, uint8_t pid, sizedptr data){
    channel->dma = data.ptr;
    uint16_t max_size = packet_size(port_speed);
    uint32_t pkt_count = (data.size + max_size - 1)/max_size;
    channel->xfer_size = (pkt_count << 19) | (pid << 29) | data.size;

    channel->cchar &= ~0x7FF;
    channel->cchar |= max_size;

    channel->intmask = 0xFFFFFFFF;

    delay(10);

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

bool DWC2Driver::request_sized_descriptor(uint8_t rType, uint8_t type, uint16_t descriptor_index, uint16_t wIndex, uint16_t descriptor_size, void *out_descriptor){
    dwc2_host_channel *channel = get_channel(0);
    
    usb_setup_packet packet = {
        .bmRequestType = rType,
        .bRequest = 6,
        .wValue = (type << 8) | descriptor_index,
        .wIndex = wIndex,
        .wLength = descriptor_size
    };

    if (!make_transfer(channel, false, 0x3, (sizedptr){(uintptr_t)&packet, sizeof(uintptr_t)})){
        kprintf("[DWC2 error] Descriptor transfer failed setup stage");
        return false;
    }

    if (!make_transfer(channel, true, 0x2, (sizedptr){(uintptr_t)out_descriptor, descriptor_size})){
        kprintf("[DWC2 error] Descriptor transfer failed data stage");
        return false;
    }

    if (!make_transfer(channel, false, 0x2, (sizedptr){0, 0})){
        kprintf("[DWC2 error] Descriptor transfer failed status stage");
        return false;
    }

    return true;
}

bool DWC2Driver::request_descriptor(uint8_t rType, uint8_t type, uint16_t index, uint16_t wIndex, void *out_descriptor){
    if (!request_sized_descriptor(rType, type, index, wIndex, sizeof(usb_descriptor_header), out_descriptor)){
        kprintf_raw("[DWC2 error] Failed to get descriptor header. Size %i", sizeof(usb_descriptor_header));
        return false;
    }
    usb_descriptor_header* descriptor = (usb_descriptor_header*)out_descriptor;
    if (descriptor->bLength == 0){
        kprintf_raw("[DWC2 error] wrong descriptor size %i. Size %x",descriptor->bLength, (uintptr_t)out_descriptor);
        return false;
    }
    return request_sized_descriptor(rType, type, index, wIndex, descriptor->bLength, out_descriptor);
}

bool DWC2Driver::get_configuration(){

    uint16_t ep_num = 0;

    usb_configuration_descriptor* config = (usb_configuration_descriptor*)allocate_in_page(mem_page, sizeof(usb_configuration_descriptor), ALIGN_64B, true, true);
    if (!request_sized_descriptor(0x80, USB_CONFIGURATION_DESCRIPTOR, 0, 0, 8, config)){
        kprintf("[DWC2 error] could not get config descriptor header");
        return false;
    }

    if (!request_sized_descriptor(0x80, USB_CONFIGURATION_DESCRIPTOR, 0, 0, config->header.bLength, config)){
        kprintf("[DWC2 error] could not get full config descriptor");
        return false;
    }

    if (!request_sized_descriptor(0x80, USB_CONFIGURATION_DESCRIPTOR, 0, 0, config->wTotalLength, config)){
        kprintf("[DWC2 error] could not get full config descriptor");
        return false;
    }

    uint16_t total_length = config->wTotalLength - config->header.bLength;

    kprintf("[DWC2] Config length %i (%i - %i)",total_length,config->wTotalLength,config->header.bLength);

    uint16_t interface_index = 0;

    bool need_new_endpoint = true;

    uint8_t* report_descriptor;
    uint16_t report_length;

    for (uint16_t i = 0; i < total_length;){
        usb_descriptor_header* header = (usb_descriptor_header*)&config->data[i];
        if (header->bLength == 0){
            kprintf("Failed to get descriptor. Header size 0");
            return false;
        }
        if (need_new_endpoint){
            need_new_endpoint = false;
            // device_endpoint = (xhci_usb_device_endpoint*)allocate_in_page(xhci_mem_page, sizeof(xhci_usb_device_endpoint), ALIGN_64B, true, true);
        }
        switch (header->bDescriptorType)
        {
        case 0x4: { //Interface
            usb_interface_descriptor *interface = (usb_interface_descriptor *)&config->data[i];
            if (interface->bInterfaceClass == 0x9){
                kprintf("USB Hub detected %i", interface->bNumEndpoints);
                
            } else if (interface->bInterfaceClass != 0x3){
                kprintf("[DWC2 implementation error] non-hid devices not supported yet %x",interface->bInterfaceClass);
                return false;
            }
            kprintf("[DWC2] interface protocol %x",interface->bInterfaceProtocol);
            switch (interface->bInterfaceProtocol)
            {
            case 0x1:
                // device_endpoint->type = KEYBOARD;
                break;
            
            default:
                break;
            }
            interface_index++;
        }
        break;
        case 0x21: { //HID
            usb_hid_descriptor *hid = (usb_hid_descriptor *)&config->data[i];
            for (uint8_t j = 0; j < hid->bNumDescriptors; j++){
                if (hid->descriptors[j].bDescriptorType == 0x22){//REPORT HID
                    report_length = hid->descriptors[j].wDescriptorLength;
                    report_descriptor = (uint8_t*)allocate_in_page(mem_page, report_length, ALIGN_64B, true, true);
                    request_descriptor(0x81, 0x22, 0, interface_index-1, report_descriptor);
                    kprintf("[DWC2] retrieved report descriptor of length %i at %x", report_length, (uintptr_t)report_descriptor);
                }
            }
        }
        break;
        case 0x5: {//Endpoint
        
            usb_endpoint_descriptor *endpoint = (usb_endpoint_descriptor*)&config->data[i];
            kprintf("[DWC2] endpoint address %x",endpoint->bEndpointAddress);
            uint8_t ep_address = endpoint->bEndpointAddress;
            uint8_t ep_dir = (ep_address & 0x80) ? 1 : 0; // 1 IN, 0 OUT
            uint8_t ep_num = ((ep_address & 0x0F) * 2) + ep_dir;

            uint8_t ep_type = endpoint->bmAttributes & 0x03; // 0 = Control, 1 = Iso, 2 = Bulk, 3 = Interrupt
            kprintf("[DWC2] endpoint %i info. Direction %i type %i",ep_num, ep_dir, ep_type);

            // device_endpoint->poll_endpoint = ep_num;
            // device_endpoint->poll_packetSize = endpoint->wMaxPacketSize;

            // if (!issue_command((uintptr_t)ctx, 0, (device->slot_id << 24) | (TRB_TYPE_CONFIG_EP << 10))){
            //     kprintf_raw("[xHCI] Failed to configure endpoint %i",ep_num);
            //     return false;
            // }

            need_new_endpoint = true;
        }
        break;
        default: kprintf("Unknown type %x", header->bDescriptorType);
        }
        i += header->bLength;
    }

    return true;
    
}