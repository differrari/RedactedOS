#include "usb.hpp"
#include "console/kio.h"
#include "async.h"
#include "std/string.h"
#include "memory/page_allocator.h"
#include "xhci_types.h"

uint16_t USBDriver::packet_size(uint16_t speed){
    switch (speed) {
        case 2: return 8;//Low
        case 1:
        case 3: return 64;//High & full
        case 4:
        case 5:
        default: return 512;//Super & Super Plus & Default
    }
}

bool USBDriver::setup_device(uint8_t address, uint16_t port){

    address = address_device(address);
    usb_device_descriptor* descriptor = (usb_device_descriptor*)allocate_in_page(mem_page, sizeof(usb_device_descriptor), ALIGN_64B, true, true);
    
    if (!request_descriptor(address, 0, 0x80, 6, USB_DEVICE_DESCRIPTOR, 0, 0, descriptor)){
        kprintf_raw("[USB error] failed to get device descriptor");
        return false;
    }


    usb_string_language_descriptor* lang_desc = (usb_string_language_descriptor*)allocate_in_page(mem_page, sizeof(usb_string_language_descriptor), ALIGN_64B, true, true);

    bool use_lang_desc = true;

    if (!request_descriptor(address, 0, 0x80, 6, USB_STRING_DESCRIPTOR, 0, 0, lang_desc)){
        kprintf_raw("[USB warning] failed to get language descriptor");
        use_lang_desc = false;
    }

    kprintf("[USB] Vendor %x",descriptor->idVendor);
    kprintf("[USB] Product %x",descriptor->idProduct);
    kprintf("[USB] USB version %x",descriptor->bcdUSB);
    kprintf("[USB] EP0 Max Packet Size: %x", descriptor->bMaxPacketSize0);
    kprintf("[USB] Configurations: %x", descriptor->bNumConfigurations);
    if (use_lang_desc){
        //TODO: we want to maintain the strings so we can have USB device information, and rework it to silece the alignment warning
        uint16_t langid = lang_desc->lang_ids[0];
        usb_string_descriptor* prod_name = (usb_string_descriptor*)allocate_in_page(mem_page, sizeof(usb_string_descriptor), ALIGN_64B, true, true);
        if (request_descriptor(address, 0, 0x80, 6, USB_STRING_DESCRIPTOR, descriptor->iProduct, langid, prod_name)){
            char name[128];
            if (utf16tochar(prod_name->unicode_string, name, sizeof(name))) {
                kprintf("[USB device] Product name: %s", (uint64_t)name);
            }
        }
        usb_string_descriptor* man_name = (usb_string_descriptor*)allocate_in_page(mem_page, sizeof(usb_string_descriptor), ALIGN_64B, true, true);
        if (request_descriptor(address, 0, 0x80, 6, USB_STRING_DESCRIPTOR, descriptor->iManufacturer, langid, man_name)){
            char name[128];
            if (utf16tochar(man_name->unicode_string, name, sizeof(name))) {
                kprintf("[USB device] Manufacturer name: %s", (uint64_t)name);
            }
        }
        usb_string_descriptor* ser_name = (usb_string_descriptor*)allocate_in_page(mem_page, sizeof(usb_string_descriptor), ALIGN_64B, true, true);
        if (request_descriptor(address, 0, 0x80, 6, USB_STRING_DESCRIPTOR, descriptor->iSerialNumber, langid, ser_name)){
            char name[128];
            if (utf16tochar(ser_name->unicode_string, name, sizeof(name))) {
                kprintf("[USB device] Serial: %s", (uint64_t)name);
            }
        }
    }

    return get_configuration(address);
}

bool USBDriver::get_configuration(uint8_t address){
    
    uint16_t ep_num = 0;

    usb_manager->register_device(address);

    usb_configuration_descriptor* config = (usb_configuration_descriptor*)allocate_in_page(mem_page, sizeof(usb_configuration_descriptor), ALIGN_64B, true, true);
    if (!request_sized_descriptor(address, 0, 0x80, 6, USB_CONFIGURATION_DESCRIPTOR, 0, 0, 8, config)){
        kprintf("[USB error] could not get config descriptor header");
        return false;
    }

    if (!request_sized_descriptor(address, 0, 0x80, 6, USB_CONFIGURATION_DESCRIPTOR, 0, 0, config->header.bLength, config)){
        kprintf("[USB error] could not get full config descriptor");
        return false;
    }

    if (!request_sized_descriptor(address, 0, 0x80, 6, USB_CONFIGURATION_DESCRIPTOR, 0, 0, config->wTotalLength, config)){
        kprintf("[USB error] could not get full config descriptor");
        return false;
    }

    uint16_t total_length = config->wTotalLength - config->header.bLength;

    kprintf("[USB] Config length %i (%i - %i)",total_length,config->wTotalLength,config->header.bLength);

    uint16_t interface_index = 0;

    bool need_new_endpoint = true;

    uint8_t* report_descriptor;
    uint16_t report_length;

    xhci_device_types dev_type;

    for (uint16_t i = 0; i < total_length;){
        usb_descriptor_header* header = (usb_descriptor_header*)&config->data[i];
        if (header->bLength == 0){
            kprintf("[USB] Failed to get descriptor. Header size 0");
            return false;
        }
        if (need_new_endpoint){
            need_new_endpoint = false;
        }
        switch (header->bDescriptorType)
        {
        case 0x4: { //Interface
            usb_interface_descriptor *interface = (usb_interface_descriptor *)&config->data[i];
            if (interface->bInterfaceClass == 0x9){
                kprintf("[USB] Hub detected %i", interface->bNumEndpoints);
                hub_enumerate(address);
                return true;
            } else if (interface->bInterfaceClass != 0x3){
                kprintf("[USB implementation error] non-hid devices not supported yet %x",interface->bInterfaceClass);
                return false;
            }
            kprintf("[USB] interface protocol %x",interface->bInterfaceProtocol);
            switch (interface->bInterfaceProtocol)
            {
            case 0x1:
                dev_type = KEYBOARD;
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
                    request_descriptor(address, 0, 0x81, 6, 0x22, 0, interface_index-1, report_descriptor);
                    kprintf("[USB] retrieved report descriptor of length %i at %x", report_length, (uintptr_t)report_descriptor);
                }
            }
        }
        break;
        case 0x5: {//Endpoint
            usb_endpoint_descriptor *endpoint = (usb_endpoint_descriptor*)&config->data[i];
            
            if (!configure_endpoint(address, endpoint, config->bConfigurationValue, dev_type)) return false;

            need_new_endpoint = true;
        }
        break;
        default: { kprintf("[USB error] Unknown type %x", header->bDescriptorType); return false; }
        }
        i += header->bLength;
    }

    return true;
    
}

bool USBDriver::request_descriptor(uint8_t address, uint8_t endpoint, uint8_t rType, uint8_t request, uint8_t type, uint16_t index, uint16_t wIndex, void *out_descriptor){
    if (!request_sized_descriptor(address, endpoint, rType, request, type, index, wIndex, sizeof(usb_descriptor_header), out_descriptor)){
        kprintf_raw("[USB error] Failed to get descriptor header. Size %i", sizeof(usb_descriptor_header));
        return false;
    }
    usb_descriptor_header* descriptor = (usb_descriptor_header*)out_descriptor;
    if (descriptor->bLength == 0){
        kprintf_raw("[USB error] wrong descriptor size %i",descriptor->bLength);
        return false;
    }
    return request_sized_descriptor(address, endpoint, rType, request, type, index, wIndex, descriptor->bLength, out_descriptor);
}

void USBDriver::hub_enumerate(uint8_t address){
    //TODO: actually support multiple devices
    uint8_t port = 1;
    uint32_t port_status;
    request_sized_descriptor(address, 0, 0xA3, 0, 0, 0, port, sizeof(uint32_t), (void*)&port_status);
    if (port_status & 1){
        kprintf("Port %i status %b",port, port_status);
        request_sized_descriptor(address, 0, 0x23, 3, 0, 4, port, 0, 0);//Port Reset
        delay(50);
        request_sized_descriptor(address, 0, 0x23, 1, 0, 4, port, 0, 0);//Port Reset Clear
        delay(10);
        request_sized_descriptor(address, 0, 0xA3, 0, 0, 0, port, sizeof(uint32_t), (void*)&port_status);
        if (!(port_status & 0b11)){
            kprintf("Port not enabled or device not connected");
            return;
        }
        handle_hub_routing(address,port);
        setup_device(0,1);
    }
}

void USBDriver::poll_inputs(){
    usb_manager->poll_inputs(this);
}