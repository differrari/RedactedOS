#include "xhci.h"
#include "console/kio.h"
#include "console/serial/uart.h"
#include "pci.h"
#include "ram_e.h"
#include "xhci_kbd.h"

static xhci_device global_device;

static bool xhci_verbose = false;

void xhci_enable_verbose(){
    xhci_verbose = true;
}

#define kprintfv(fmt, ...) \
    ({ \
        if (xhci_verbose){\
            uint64_t _args[] = { __VA_ARGS__ }; \
            kprintf_args((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
        }\
    })

bool xhci_check_fatal_error() {
    uint32_t sts = global_device.op->usbsts;
    if (sts & (XHCI_USBSTS_HSE | XHCI_USBSTS_CE)) {
        kprintf("[xHCI ERROR] Fatal condition: USBSTS = %h", sts);
        return true;
    }
    return false;
}

bool enable_xhci_events(){
    kprintfv("[xHCI] Allocating ERST");
    global_device.interrupter = (xhci_interrupter*)(uintptr_t)(global_device.rt_base + 0x20);

    uint64_t event_ring = alloc_dma_region(MAX_TRB_AMOUNT * sizeof(trb));
    uint64_t erst_addr = alloc_dma_region(sizeof(erst_entry) * MAX_ERST_AMOUNT);
    erst_entry* erst = (erst_entry*)erst_addr;

    erst->ring_base = event_ring;
    erst->ring_size = MAX_TRB_AMOUNT;
    erst->reserved = 0;
    kprintfv("[xHCI] ERST ring_base: %h", event_ring);
    kprintfv("[xHCI] ERST ring_size: %h", erst[0].ring_size);
    global_device.event_ring = (trb*)event_ring;

    kprintfv("[xHCI] Interrupter register @ %h", global_device.rt_base + 0x20);
    
    global_device.interrupter->erstsz = 1;
    kprintfv("[xHCI] ERSTSZ set to: %h", global_device.interrupter->erstsz);
    
    global_device.interrupter->erdp = event_ring;
    global_device.interrupter->erstba = erst_addr;
    kprintfv("[xHCI] ERSTBA set to: %h", global_device.interrupter->erstba);
    
    kprintfv("[xHCI] ERDP set to: %h", global_device.interrupter->erdp);
    
    global_device.interrupter->iman |= 1 << 1;//Enable interrupt

    global_device.op->usbsts = 1 << 3;//Enable interrupts
    global_device.interrupter->iman |= 1;//Clear pending interrupts

    return !xhci_check_fatal_error();
}

void make_ring_link_control(trb* ring, bool cycle){
    ring[MAX_TRB_AMOUNT-1].control =
        (TRB_TYPE_LINK << 10)
      | (1 << 1)              // Toggle Cycle
      | cycle;
}

void make_ring_link(trb* ring, bool cycle){
    ring[MAX_TRB_AMOUNT-1].parameter = (uintptr_t)ring;
    ring[MAX_TRB_AMOUNT-1].status = 0;
    make_ring_link_control(ring, cycle);
}

bool xhci_init(xhci_device *xhci, uint64_t pci_addr) {
    kprintfv("[xHCI] init");
    if (!(read16(pci_addr + 0x06) & (1 << 4))){
        kprintfv("[xHCI] Wrong capabilities list");
        return false;
    }

    pci_setup_msi(pci_addr, XHCI_IRQ);

    if (!pci_setup_bar(pci_addr, 0, &xhci->mmio, &xhci->mmio_size)){
        kprintfv("[xHCI] BARs not set up");
        return false;
    }

    pci_enable_device(pci_addr);

    kprintfv("[xHCI] BARs set up @ %h (%h)",xhci->mmio,xhci->mmio_size);

    xhci->cap = (xhci_cap_regs*)(uintptr_t)xhci->mmio;
    kprintfv("[xHCI] caplength %h",xhci->cap->caplength);
    uint64_t op_base = xhci->mmio + xhci->cap->caplength;
    xhci->op = (xhci_op_regs*)(uintptr_t)op_base;
    xhci->ports = (xhci_port_regs*)((uintptr_t)op_base + 0x400);
    xhci->db_base = xhci->mmio + (xhci->cap->dboff & ~0x1F);
    xhci->rt_base = xhci->mmio + (xhci->cap->rtsoff & ~0x1F);

    kprintfv("[xHCI] Resetting controller");
    xhci->op->usbcmd &= ~1;
    while (xhci->op->usbcmd & 1);
    kprintfv("[xHCI] Clear complete");

    xhci->op->usbcmd |= (1 << 1);
    while (xhci->op->usbcmd & 1);
    kprintfv("[xHCI] Reset complete");

    while (xhci->op->usbsts & (1 << 11)); 
    kprintfv("[xHCI] Device ready");

    if (xhci->op->usbcmd != 0){
        kprintf("[xHCI Error] wrong usbcmd %h",xhci->op->usbcmd);
        return false;
    } else {
        kprintfv("[xHCI] Correct usbcmd value");
    }

    if (xhci->op->dnctrl != 0){
        kprintf("[xHCI Error] wrong dnctrl %h",xhci->op->dnctrl);
        return false;
    } else {
        kprintfv("[xHCI] Correct dnctrl value");
    }

    if (xhci->op->crcr != 0){
        kprintf("[xHCI Error] wrong crcr %h",xhci->op->crcr);
        return false;
    } else {
        kprintfv("[xHCI] Correct crcr value");
    }

    if (xhci->op->dcbaap != 0){
        kprintf("[xHCI Error] wrong dcbaap %h",xhci->op->dcbaap);
        return false;
    } else {
        kprintfv("[xHCI] Correct dcbaap value");
    }

    if (xhci->op->config != 0){
        kprintf("[xHCI Error] wrong config %h",xhci->op->config);
        return false;
    } else {
        kprintfv("[xHCI] Correct config value");
    }

    xhci->command_cycle_bit = 1;
    xhci->event_cycle_bit = 1;

    xhci->max_device_slots = xhci->cap->hcsparams1 & 0xFF;
    xhci->max_ports = (xhci->cap->hcsparams1 >> 24) & 0xFF;

    uint16_t erst_max = ((xhci->cap->hcsparams2 >> 4) & 0xF);
    
    kprintfv("[xHCI] ERST Max: 2^%i",erst_max);
    
    xhci->op->dnctrl = 0xFFFF;//Enable device notifications

    xhci->op->config = xhci->max_device_slots;
    kprintfv("[xHCI] %i device slots", xhci->max_device_slots);

    uint64_t dcbaap_addr = alloc_dma_region((xhci->max_device_slots + 1) * sizeof(uintptr_t));

    xhci->op->dcbaap = dcbaap_addr;

    xhci->op->pagesize = 0b1;//4KB page size

    uint32_t scratchpad_count = ((xhci->cap->hcsparams2 >> 27) & 0x1F);

    xhci->dcbaa = (uint64_t *)dcbaap_addr;

    uint64_t* scratchpad_array = (uint64_t*)alloc_dma_region(scratchpad_count * sizeof(uint64_t));
    for (uint32_t i = 0; i < scratchpad_count; i++)
        scratchpad_array[i] = alloc_dma_region(0x1000);
    xhci->dcbaa[0] = (uint64_t)scratchpad_array;

    kprintfv("[xHCI] dcbaap assigned at %h with %i scratchpads",dcbaap_addr,scratchpad_count);

    uint64_t command_ring = alloc_dma_region(MAX_TRB_AMOUNT * sizeof(trb));

    xhci->cmd_ring = (trb*)command_ring;
    xhci->cmd_index = 0;
    xhci->op->crcr = command_ring | xhci->command_cycle_bit;

    make_ring_link(xhci->cmd_ring, xhci->command_cycle_bit);

    kprintfv("[xHCI] command ring allocated at %h. crcr now %h",command_ring, xhci->op->crcr);

    if (!enable_xhci_events())
        return false;

    kprintfv("[xHCI] event configuration finished");

    xhci->op->usbcmd |= (1 << 2);//Interrupt enable
    xhci->op->usbcmd |= 1;//Run
    while ((xhci->op->usbsts & 0x1));

    kprintfv("[xHCI] Init complete with usbcmd %h, usbsts %h",xhci->op->usbcmd, xhci->op->usbsts);

    return !xhci_check_fatal_error();
}


void ring_doorbell(uint32_t slot, uint32_t endpoint) {
    volatile uint32_t* db = (uint32_t*)(uintptr_t)(global_device.db_base + (slot << 2));
    kprintfv("[xHCI] Ringing doorbell at %h with value %h", global_device.db_base + (slot << 2),endpoint);
    *db = endpoint;
}

bool xhci_await_response(uint64_t command, uint32_t type){
    while (true) {
        if (xhci_check_fatal_error()){
            kprintf("[xHCI error] USBSTS value %h",global_device.op->usbsts);
            return false;
        }
        for (global_device.event_index; global_device.event_index < MAX_TRB_AMOUNT; global_device.event_index++){
            trb* ev = &global_device.event_ring[global_device.event_index];
            if (!((ev->control & 1) == global_device.event_cycle_bit)) //TODO: implement a timeout
                break;
            kprintfv("[xHCI] A response at %i of type %h as a response to %h",global_device.event_index, (ev->control & TRB_TYPE_MASK) >> 10, ev->parameter);
            if (global_device.event_index == MAX_TRB_AMOUNT - 1){
                global_device.event_index = 0;
                global_device.event_cycle_bit = !global_device.event_cycle_bit;
            }
            if ((ev->control & TRB_TYPE_MASK) >> 10 == type && (command == 0 || ev->parameter == (command & 0xFFFFFFFFFFFFFFFF))){
                uint8_t completion_code = (ev->control >> 24) & 0xFF;
                global_device.interrupter->erdp = (uintptr_t)ev | (1 << 3);//Inform of latest processed event
                global_device.interrupter->iman |= 1;//Clear interrupts
                global_device.op->usbsts |= 1 << 3;//Clear interrupts
                return completion_code == 1;
            }
        }
    }
}

bool xhci_sync_events(){
    for (global_device.event_index; global_device.event_index < MAX_TRB_AMOUNT; global_device.event_index++){
        trb* ev = &global_device.event_ring[global_device.event_index];
        if (!((ev->control & 1) == global_device.event_cycle_bit)){
            kprintf("Found end of event ring");
            global_device.interrupter->erdp = (uintptr_t)ev | (1 << 3);
            global_device.interrupter->iman |= 1;
            global_device.op->usbsts |= 1 << 3;
            return true;
        }
        kprintf("[xHCI] Found unhandled response %h",(ev->control & TRB_TYPE_MASK) >> 10);
        global_device.interrupter->erdp = (uintptr_t)ev | (1 << 3);
        global_device.interrupter->iman |= 1;
        global_device.op->usbsts |= 1 << 3;
    }
}

bool issue_command(uint64_t param, uint32_t status, uint32_t control){
    trb* cmd = &global_device.cmd_ring[global_device.cmd_index++];
    cmd->parameter = param;
    cmd->status = status;
    cmd->control = control | global_device.command_cycle_bit;

    uint64_t cmd_addr = (uintptr_t)cmd;
    kprintfv("[xHCI] issuing command with control: %h", cmd->control);
    if (global_device.cmd_index == MAX_TRB_AMOUNT - 1){
        make_ring_link_control(global_device.cmd_ring, global_device.command_cycle_bit);
        global_device.command_cycle_bit = !global_device.command_cycle_bit;
        global_device.cmd_index = 0;
    }
    ring_doorbell(0, 0);
    return xhci_await_response(cmd_addr, TRB_TYPE_COMMAND_COMPLETION);
}

uint16_t packet_size(uint16_t port_speed){
    switch (port_speed) {
        case 2: return 8;//Low
        case 1:
        case 3: return 64;//High & full
        case 4:
        case 5:
        default: return 512;//Super & Super Plus & Default
    }
}

bool reset_port(uint16_t port){
    if (port == -1) return false;

    xhci_port_regs* port_info = &global_device.ports[port];

    if (port_info->portsc.pp == 0){
        port_info->portsc.pp = 1;

        //Read back after delay to ensure
        // if (port_info->portsc.pp == 0){
        //     kprintf("[xHCI error] failed to power on port %i",port);
        //     return false;
        // }
    }

    port_info->portsc.csc = 1;
    port_info->portsc.pec = 1;
    port_info->portsc.prc = 1;

    //TODO: if usb3
    //portsc.wpr = 1;
    //else 
    port_info->portsc.pr = 1;

    //TODO: read back after delay 

    return xhci_await_response(0, TRB_TYPE_PORT_STATUS_CHANGE);
}

bool xhci_request_sized_descriptor(xhci_usb_device *device, bool interface, uint8_t type, uint16_t descriptor_index, uint16_t wIndex, uint16_t descriptor_size, void *out_descriptor){
    usb_setup_packet packet = {
        .bmRequestType = 0x80 | interface,
        .bRequest = 6,
        .wValue = (type << 8) | descriptor_index,
        .wIndex = wIndex,
        .wLength = descriptor_size
    };
    
    trb* setup_trb = &device->transfer_ring[device->transfer_index++];
    memcpy(&setup_trb->parameter, &packet, sizeof(packet));
    setup_trb->status = sizeof(usb_setup_packet);
    //bit 4 = chain. Bit 16 direction (3 = IN, 2 = OUT, 0 = NO DATA)
    setup_trb->control = (3 << 16) | (TRB_TYPE_SETUP_STAGE << 10) | (1 << 6) | (1 << 4) | device->transfer_cycle_bit;
    
    trb* data = &device->transfer_ring[device->transfer_index++];
    data->parameter = (uintptr_t)out_descriptor;
    data->status = packet.wLength;
    //bit 16 = direction
    data->control = (1 << 16) | (TRB_TYPE_DATA_STAGE << 10) | (0 << 4) | device->transfer_cycle_bit;
    
    trb* status_trb = &device->transfer_ring[device->transfer_index++];
    status_trb->parameter = 0;
    status_trb->status = 0;
    //bit 5 = interrupt-on-completion
    status_trb->control = (TRB_TYPE_STATUS_STAGE << 10) | (1 << 5) | device->transfer_cycle_bit;

    if (device->transfer_index == MAX_TRB_AMOUNT - 1){
        make_ring_link_control(device->transfer_ring, device->transfer_cycle_bit);
        device->transfer_cycle_bit = !device->transfer_cycle_bit;
        device->transfer_index = 0;
    }
    
    ring_doorbell(device->slot_id, 1);

    kprintfv("Awaiting response of type %h at %h",TRB_TYPE_TRANSFER, (uintptr_t)status_trb);
    if (!xhci_await_response((uintptr_t)status_trb, TRB_TYPE_TRANSFER)){
        kprintf("[xHCI error] error fetching descriptor");
    }
    return true;
}

bool clear_halt(xhci_usb_device *device, uint16_t endpoint_num){
    kprintfv("Clearing halt");
    usb_setup_packet packet = {
        .bmRequestType = 0x2,
        .bRequest = 1,
        .wValue = 0,
        .wIndex = endpoint_num,
        .wLength = 0
    };

    trb* setup_trb = &device->transfer_ring[device->transfer_index++];
    memcpy(&setup_trb->parameter, &packet, sizeof(packet));
    setup_trb->status = sizeof(usb_setup_packet);
    //Bit 16 direction (3 = IN, 2 = OUT, 0 = NO DATA)
    setup_trb->control = (0 << 16) | (TRB_TYPE_SETUP_STAGE << 10) | (1 << 6) | device->transfer_cycle_bit;

    trb* status_trb = &device->transfer_ring[device->transfer_index++];
    status_trb->parameter = 0;
    status_trb->status = 0;
    //bit 16 = direction. 1 = handshake. bit 5 = interrupt-on-completion
    status_trb->control = (1 << 16) | (TRB_TYPE_STATUS_STAGE << 10) | (1 << 5) | device->transfer_cycle_bit;

    if (device->transfer_index == MAX_TRB_AMOUNT - 1){
        make_ring_link_control(device->transfer_ring, device->transfer_cycle_bit);
        device->transfer_cycle_bit = !device->transfer_cycle_bit;
        device->transfer_index = 0;
    }
    
    ring_doorbell(device->slot_id, 1);

    kprintfv("Awaiting response of type %h at %h",TRB_TYPE_TRANSFER, (uintptr_t)status_trb);
    if (!xhci_await_response((uintptr_t)status_trb, TRB_TYPE_TRANSFER)){
        kprintf("[xHCI error] could not clear stall");
    }
}

bool xhci_request_descriptor(xhci_usb_device *device, bool interface, uint8_t type, uint16_t index, uint16_t wIndex, void *out_descriptor){
    if (!xhci_request_sized_descriptor(device, interface, type, index, wIndex, sizeof(usb_descriptor_header), out_descriptor)){
        kprintf("[xHCI error] Failed to get descriptor header. Size %i", sizeof(usb_descriptor_header));
        return false;
    }
    usb_descriptor_header* descriptor = (usb_descriptor_header*)out_descriptor;
    if (descriptor->bLength == 0){
        kprintf("[xHCI error] wrong descriptor size %i",descriptor->bLength);
        return false;
    }
    return xhci_request_sized_descriptor(device, interface, type, index, wIndex, descriptor->bLength, out_descriptor);
}

bool parse_string_descriptor_utf16le(const uint16_t* str_in, char* out_str, size_t max_len) {
    size_t out_i = 0;
    while (str_in[out_i] != 0 && out_i + 1 < max_len) {
        uint16_t wc = str_in[out_i];
        out_str[out_i] = (wc <= 0x7F) ? (char)wc : '?';
        out_i++;
    }
    out_str[out_i] = '\0';
    return true;
}

uint8_t get_ep_type(usb_endpoint_descriptor* descriptor) {
    return (descriptor->bEndpointAddress & 0x80 ? 1 << 2 : 0) | (descriptor->bmAttributes & 0x3);
}

bool xhci_get_configuration(usb_configuration_descriptor *config, xhci_usb_device *device){
    uint16_t total_length = config->wTotalLength - config->header.bLength;

    kprintfv("[xHCI] Config length %i (%i - %i)",total_length,config->wTotalLength,config->header.bLength);

    uint16_t interface_index = 0;

    uint8_t device_type;

    for (uint16_t i = 0; i < total_length;){
        usb_descriptor_header* header = (usb_descriptor_header*)&config->data[i];
        if (header->bLength == 0){
            kprintf("Failed to get descriptor. Header size 0");
            return false;
        }
        switch (header->bDescriptorType)
        {
        case 0x4: //Interface
            usb_interface_descriptor *interface = (usb_interface_descriptor *)&config->data[i];
            if (interface->bInterfaceClass != 0x3){
                kprintf("[xHCI implementation error] non-hid devices not supported yet");
                return false;
            }
            kprintfv("[xHCI] interface protocol %h",interface->bInterfaceProtocol);
            switch (interface->bInterfaceProtocol)
            {
            case 0x1:
                set_default_kbd(device);
                device->type = KEYBOARD;
                break;
            
            default:
                break;
            }
            device->interface_protocol = interface->bInterfaceProtocol;
            interface_index++;
        break;
        case 0x21://HID
            usb_hid_descriptor *hid = (usb_hid_descriptor *)&config->data[i];
            for (uint8_t j = 0; j < hid->bNumDescriptors; j++){
                if (hid->descriptors[j].bDescriptorType == 0x22){//REPORT HID
                    device->report_length = hid->descriptors[j].wDescriptorLength;
                    device->report_descriptor = (uint8_t*)alloc_dma_region(device->report_length);
                    xhci_request_descriptor(device, true, 0x22, 0, interface_index-1, device->report_descriptor);
                    kprintfv("[xHCI] retrieved report descriptor of length %i at %h", device->report_length, (uintptr_t)device->report_descriptor);
                }
            }
        break;
        case 0x5: //Endpoint
            usb_endpoint_descriptor *endpoint = (usb_endpoint_descriptor*)&config->data[i];
            kprintfv("[xHCI] endpoint address %h",endpoint->bEndpointAddress);
            uint8_t ep_address = endpoint->bEndpointAddress;
            uint8_t ep_dir = (ep_address & 0x80) ? 1 : 0; // 1 IN, 0 OUT
            uint8_t ep_num = ((ep_address & 0x0F) * 2) + ep_dir;

            uint8_t ep_type = endpoint->bmAttributes & 0x03; // 0 = Control, 1 = Iso, 2 = Bulk, 3 = Interrupt
            kprintfv("[xHCI] endpoint %i info. Direction %i type %i",ep_num, ep_dir, ep_type);

            xhci_input_context* ctx = device->ctx;

            ctx->control_context.add_flags = (1 << 0) | (1 << ep_num);
            ctx->device_context.slot_f0.context_entries = 2; // 2 entries: EP0 + EP1
            ctx->device_context.endpoints[ep_num-1].endpoint_f0.interval = endpoint->bInterval;
            
            ctx->device_context.endpoints[ep_num-1].endpoint_f0.endpoint_state = 0;
            ctx->device_context.endpoints[ep_num-1].endpoint_f1.endpoint_type = get_ep_type(endpoint);
            ctx->device_context.endpoints[ep_num-1].endpoint_f1.max_packet_size = endpoint->wMaxPacketSize;
            ctx->device_context.endpoints[ep_num-1].endpoint_f4.max_esit_payload_lo = endpoint->wMaxPacketSize;
            ctx->device_context.endpoints[ep_num-1].endpoint_f1.error_count = 3;

            device->endpoint_transfer_ring = (trb*)alloc_dma_region(0x1000);
            device->endpoint_transfer_cycle_bit = 1;
            make_ring_link(device->endpoint_transfer_ring, device->endpoint_transfer_cycle_bit);
            ctx->device_context.endpoints[ep_num-1].endpoint_f23.dcs = device->endpoint_transfer_cycle_bit;
            ctx->device_context.endpoints[ep_num-1].endpoint_f23.ring_ptr = ((uintptr_t)device->endpoint_transfer_ring) >> 4;
            ctx->device_context.endpoints[ep_num-1].endpoint_f4.average_trb_length = 8;

            device->poll_endpoint = ep_num;
            device->poll_packetSize = endpoint->wMaxPacketSize;

            kprintf("A");

            if (!issue_command((uintptr_t)ctx, 0, (device->slot_id << 24) | (TRB_TYPE_CONFIG_EP << 10))){
                kprintf("[xHCI] Failed to configure endpoint %i",ep_num);
                return false;
            }

            kprintf("Finished");

        break;
        }
        i += header->bLength;
    }

    xhci_sync_events();

    if (device->type == KEYBOARD)
        xhci_kbd_request_data();

    return true;
    
}

bool xhci_setup_device(uint16_t port){

    kprintfv("[xHCI] detecting and activating the default device at %i port. Resetting port...",port);
    reset_port(port);
    kprintfv("[xHCI] Port speed %i", (uint32_t)global_device.ports[port].portsc.port_speed);

    if (!issue_command(0,0,TRB_TYPE_ENABLE_SLOT << 10)){
        kprintf("[xHCI error] failed enable slot command");
        return false;
    }

    xhci_usb_device *device = (xhci_usb_device*)palloc(sizeof(xhci_usb_device));

    device->slot_id = (global_device.event_ring[0].status >> 24) & 0xFF;
    kprintfv("[xHCI] Slot id %h", device->slot_id);

    if (device->slot_id == 0){
        kprintf("[xHCI error]: Wrong slot id 0");
        return false;
    }

    device->transfer_cycle_bit = 1;

    xhci_input_context *ctx = (xhci_input_context*)alloc_dma_region(0x1000);
    device->ctx = ctx;
    void* output_ctx = (void*)alloc_dma_region(4096);
    
    ctx->control_context.add_flags = 0b11;
    
    ctx->device_context.slot_f0.speed = global_device.ports[port].portsc.port_speed;
    ctx->device_context.slot_f0.context_entries = 1;
    ctx->device_context.slot_f1.root_hub_port_num = port + 1;
    
    ctx->device_context.endpoints[0].endpoint_f0.endpoint_state = 0;//Disabled
    ctx->device_context.endpoints[0].endpoint_f1.endpoint_type = 4;//Type = control
    ctx->device_context.endpoints[0].endpoint_f0.interval = 0;
    ctx->device_context.endpoints[0].endpoint_f1.error_count = 3;//3 errors allowed
    ctx->device_context.endpoints[0].endpoint_f1.max_packet_size = packet_size(ctx->device_context.slot_f0.speed);//Packet size. Guessed from port speed
    
    device->transfer_ring = (trb*)alloc_dma_region(0x1000);
    make_ring_link(device->transfer_ring, device->transfer_cycle_bit);

    ctx->device_context.endpoints[0].endpoint_f23.dcs = device->transfer_cycle_bit;
    ctx->device_context.endpoints[0].endpoint_f23.ring_ptr = ((uintptr_t)device->transfer_ring) >> 4;
    ctx->device_context.endpoints[0].endpoint_f4.average_trb_length = 8;

    ((uint64_t*)(uintptr_t)global_device.op->dcbaap)[device->slot_id] = (uintptr_t)output_ctx;
    if (!issue_command((uintptr_t)device->ctx, 0, (device->slot_id << 24) | (TRB_TYPE_ADDRESS_DEV << 10))){
        kprintf("[xHCI error] failed addressing device at slot %h",device->slot_id);
        return false;
    }

    xhci_device_context* context = (xhci_device_context*)(uintptr_t)(global_device.dcbaa[device->slot_id]);

    kprintfv("[xHCI] ADDRESS_DEVICE command issued. Received package size %i",context->endpoints[0].endpoint_f1.max_packet_size);

    usb_device_descriptor* descriptor = (usb_device_descriptor*)alloc_dma_region(sizeof(usb_device_descriptor));
    
    if (!xhci_request_descriptor(device, false, USB_DEVICE_DESCRIPTOR, 0, 0, descriptor)){
        kprintf("[xHCI error] failed to get device descriptor");
        return false;
    }

    usb_string_language_descriptor* lang_desc = (usb_string_language_descriptor*)alloc_dma_region(sizeof(usb_string_language_descriptor));

    bool use_lang_desc = true;

    if (!xhci_request_descriptor(device, false, USB_STRING_DESCRIPTOR, 0, 0, lang_desc)){
        kprintf("[xHCI warning] failed to get language descriptor");
        use_lang_desc = false;
    }

    kprintfv("[xHCI] Vendor %h",descriptor->idVendor);
    kprintfv("[xHCI] Product %h",descriptor->idProduct);
    kprintfv("[xHCI] USB version %h",descriptor->bcdUSB);
    kprintfv("[xHCI] EP0 Max Packet Size: %h", descriptor->bMaxPacketSize0);
    kprintfv("[xHCI] Configurations: %h", descriptor->bNumConfigurations);
    if (use_lang_desc){
        uint16_t langid = lang_desc->lang_ids[0];
        usb_string_descriptor* prod_name = (usb_string_descriptor*)alloc_dma_region(sizeof(usb_string_descriptor));
        if (xhci_request_descriptor(device, false, USB_STRING_DESCRIPTOR, descriptor->iProduct, langid, prod_name)){
            char name[128];
            if (parse_string_descriptor_utf16le(prod_name->unicode_string, name, sizeof(name))) {
                kprintfv("[xHCI device] Product name: %s", (uint64_t)name);
            }
        }
        usb_string_descriptor* man_name = (usb_string_descriptor*)alloc_dma_region(sizeof(usb_string_descriptor));
        if (xhci_request_descriptor(device, false, USB_STRING_DESCRIPTOR, descriptor->iManufacturer, langid, man_name)){
            char name[128];
            if (parse_string_descriptor_utf16le(man_name->unicode_string, name, sizeof(name))) {
                kprintfv("[xHCI device] Manufacturer name: %s", (uint64_t)name);
            }
        }
        usb_string_descriptor* ser_name = (usb_string_descriptor*)alloc_dma_region(sizeof(usb_string_descriptor));
        if (xhci_request_descriptor(device, false, USB_STRING_DESCRIPTOR, descriptor->iSerialNumber, langid, ser_name)){
            char name[128];
            if (parse_string_descriptor_utf16le(ser_name->unicode_string, name, sizeof(name))) {
                kprintfv("[xHCI device] Serial: %s", (uint64_t)name);
            }
        }
    }

    usb_configuration_descriptor* config = (usb_configuration_descriptor*)alloc_dma_region(sizeof(usb_configuration_descriptor));
    if (!xhci_request_sized_descriptor(device, false, USB_CONFIGURATION_DESCRIPTOR, 0, 0, 8, config)){
        kprintf("[xHCI error] could not get config descriptor header");
        return false;
    }

    if (!xhci_request_sized_descriptor(device, false, USB_CONFIGURATION_DESCRIPTOR, 0, 0, config->header.bLength, config)){
        kprintf("[xHCI error] could not get full config descriptor");
        return false;
    }

    if (!xhci_request_sized_descriptor(device, false, USB_CONFIGURATION_DESCRIPTOR, 0, 0, config->wTotalLength, config)){
        kprintf("[xHCI error] could not get full config descriptor");
        return false;
    }

    if (!xhci_get_configuration(config, device)){
        kprintf("[xHCI error] failed to parse device configuration");
        return false;
    }

    return true;
}

bool xhci_input_init() {
    uint64_t addr = find_pci_device(0x1B36, 0xD);
    if (!addr){ 
        kprintf("[PCI] xHCI device not found");
        return false;
    }

    if (!xhci_init(&global_device, addr)){
        kprintf("xHCI device initialization failed");
        return false;
    }

    kprintfv("[xHCI] device initialized");

    //QEMU only device finder

    for (uint16_t i = 0; i < global_device.max_ports; i++)
        if (global_device.ports[i].portsc.ccs && global_device.ports[i].portsc.csc){
            if (!xhci_setup_device(i)){
                kprintf("Failed to configure device at port %i",i);
                return false;
            }
            break;
        }

    return true;
}

void xhci_handle_interrupt(){
    xhci_read_key();
}