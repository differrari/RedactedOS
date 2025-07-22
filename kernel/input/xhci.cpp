#include "xhci.hpp"
#include "USBManager.hpp"
#include "async.h"
#include "usb.hpp"
#include "pci.h"
#include "usb_types.h"
#include "hw/hw.h"
#include "memory/memory_access.h"
#include "std/memfunctions.h"
#include "async.h"
#include "memory/memory_access.h"

uint64_t awaited_addr;
uint32_t awaited_type;
#define AWAIT(addr, action, type) \
    ({ \
        awaited_addr = (uintptr_t)(addr); \
        awaited_type = (type); \
        interrupter->iman &= ~1; \
        action; \
        bool response = await_response((uintptr_t)(addr), (type)); \
        interrupter->iman |= 1; \
        response; \
    })

#define kprintfv(fmt, ...) \
    ({ \
        if (verbose){\
            uint64_t _args[] = { __VA_ARGS__ }; \
            kprintf_args_raw((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
        }\
    })

bool XHCIDriver::check_fatal_error() {
    uint32_t sts = op->usbsts;
    if (sts & (XHCI_USBSTS_HSE | XHCI_USBSTS_CE)) {
        kprintf_raw("[xHCI ERROR] Fatal condition: USBSTS = %x", sts);
        return true;
    }
    return false;
}

#define CHECK_XHCI_FIELD(field) (op->field != 0 ? (kprintf_raw("[xHCI Error] wrong " #field " %x", op->field), false) : (kprintfv("[xHCI] Correct " #field " value"), true))

#define XHCI_EP_TYPE_INT_IN 7
#define XHCI_EP_TYPE_INT_OUT 3
#define XHCI_EP_TYPE_ISO_IN 5
#define XHCI_EP_TYPE_ISO_OUT 1

#define XHCI_SPEED_UNDEFINED            0
#define XHCI_SPEED_FULL_SPEED           1
#define XHCI_SPEED_LOW_SPEED            2
#define XHCI_SPEED_HIGH_SPEED           3
#define XHCI_SPEED_SUPER_SPEED          4
#define XHCI_SPEED_SUPER_SPEED_PLUS     5

#define XHCI_EP_DISABLED 0 
#define XHCI_EP_CONTROL 4

bool XHCIDriver::init(){
    uint64_t addr, mmio, mmio_size;
    bool use_pci = false;
    use_interrupts = true;
    if (XHCI_BASE){
        addr = XHCI_BASE;
        mmio = addr;
        if (BOARD_TYPE == 2 && RPI_BOARD >= 5)
            quirk_simulate_interrupts = !pci_setup_msi_rp1(36, true);
    } else if (PCI_BASE) {
        addr = find_pci_device(0x1B36, 0xD);
        use_pci = true;
    }
    if (!addr){ 
        kprintf_raw("[PCI] xHCI device not found");
        return false;
    }

    kprintfv("[xHCI] init");
    if (use_pci){
        if (!(*(uint16_t*)(addr + 0x06) & (1 << 4))){
            kprintf("[xHCI] Wrong capabilities list");
            return false;
        }
    
        pci_enable_device(addr);
    
        if (!pci_setup_bar(addr, 0, &mmio, &mmio_size)){
            kprintf("[xHCI] BARs not set up");
            return false;
        }
    
        pci_register(mmio, mmio_size);
    
        uint8_t interrupts_ok = pci_setup_interrupts(addr, INPUT_IRQ, 1);
        switch(interrupts_ok){
            case 0:
                kprintf("[xHCI] Failed to setup interrupts");
                quirk_simulate_interrupts = true;
                break;
            case 1:
                kprintfv("[xHCI] Interrupts setup with MSI-X %i",INPUT_IRQ);
                break;
            default:
                kprintfv("[xHCI] Interrupts setup with MSI %i",INPUT_IRQ);
                break;
        }
    
        kprintfv("[xHCI] BARs set up @ %x (%x)",mmio,mmio_size);
    }

    cap = (xhci_cap_regs*)mmio;
    kprintfv("[xHCI] caplength %x",cap->caplength);
    uintptr_t op_base = mmio + cap->caplength;
    op = (xhci_op_regs*)op_base;
    ports = (xhci_port_regs*)(op_base + 0x400);
    db_base = mmio + (cap->dboff & ~0x1F);
    rt_base = mmio + (cap->rtsoff & ~0x1F);

    kprintfv("[xHCI] Resetting controller");
    op->usbcmd &= ~1;
    wait(&op->usbcmd, 1, false, 16);
    kprintfv("[xHCI] Clear complete");

    op->usbcmd |= (1 << 1);
    wait(&op->usbcmd, 1 << 1, false, 1000);
    kprintfv("[xHCI] Reset complete");

    wait(&op->usbsts, 1 << 11, false, 1000);
    kprintfv("[xHCI] Device ready");

    if (!CHECK_XHCI_FIELD(usbcmd)) return false;
    if (!CHECK_XHCI_FIELD(dnctrl)) return false;
    if (!CHECK_XHCI_FIELD(crcr)) return false;
    if (!CHECK_XHCI_FIELD(dcbaap)) return false;
    if (!CHECK_XHCI_FIELD(config)) return false;

    command_ring.cycle_bit = 1;

    max_device_slots = cap->hcsparams1 & 0xFF;
    max_ports = (cap->hcsparams1 >> 24) & 0xFF;

    usb_manager = new USBManager(max_device_slots);

    uint16_t erst_max = ((cap->hcsparams2 >> 4) & 0xF);
    
    kprintfv("[xHCI] ERST Max: 2^%i",erst_max);
    
    op->dnctrl = 0xFFFF;//Enable device notifications

    op->config = max_device_slots;
    kprintfv("[xHCI] %i device slots", max_device_slots);

    mem_page = alloc_page(0x1000, true, true, false);

    uintptr_t dcbaap_addr = (uintptr_t)allocate_in_page(mem_page, (max_device_slots + 1) * sizeof(uintptr_t), ALIGN_64B, true, true);

    op->dcbaap = dcbaap_addr;

    op->pagesize = 0b1;//4KB page size

    uint32_t scratchpad_count = ((cap->hcsparams2 >> 27) & 0x1F);

    dcbaap = (uintptr_t*)dcbaap_addr;

    uint64_t* scratchpad_array = (uint64_t*)allocate_in_page(mem_page, (scratchpad_count == 0 ? 1 : scratchpad_count) * sizeof(uintptr_t), ALIGN_64B, true, true);
    for (uint32_t i = 0; i < scratchpad_count; i++)
        scratchpad_array[i] = (uint64_t)allocate_in_page(mem_page, 0x1000, ALIGN_64B, true, true);
    dcbaap[0] = (uint64_t)scratchpad_array;

    kprintfv("[xHCI] dcbaap assigned at %x with %i scratchpads",dcbaap_addr,scratchpad_count);

    command_ring.ring = (trb*)allocate_in_page(mem_page, MAX_TRB_AMOUNT * sizeof(trb), ALIGN_64B, true, true);

    op->crcr = (uintptr_t)command_ring.ring | command_ring.cycle_bit;

    make_ring_link(command_ring.ring, command_ring.cycle_bit);

    kprintfv("[xHCI] command ring allocated at %x. crcr now %x",(uintptr_t)command_ring.ring, op->crcr);

    if (!enable_events()){
        kprintf("[xHCI error] failed to enable events");
        return false;
    }

    kprintfv("[xHCI] event configuration finished");

    op->usbcmd |= (1 << 2);//Interrupt enable
    op->usbcmd |= 1;//Run
    wait (&op->usbsts, 0x1, false, 1000);

    endpoint_map = IndexMap<xhci_ring>(255 * 5);
    context_map = IndexMap<xhci_input_context*>(255 * 5);

    kprintfv("[xHCI] Init complete with usbcmd %x, usbsts %x",op->usbcmd, op->usbsts);
    
    if (check_fatal_error()) return false;

    for (uint16_t i = 0; i < max_ports; i++)
        if (ports[i].portsc.ccs && ports[i].portsc.csc){
            if (!port_reset(i)){
                kprintf("[xHCI] Failed to reset port %i",i);
                continue;
            }
            if (!setup_device(0,i)){
                kprintf("[xHCI] Failed to configure device at port %i",i);
            }
        }

    return true;

}

bool XHCIDriver::port_reset(uint16_t port){

    xhci_port_regs* port_info = &ports[port];

    if (port_info->portsc.pp == 0){
        port_info->portsc.pp = 1;

        delay(20);

        //Read back after delay to ensure
        if (port_info->portsc.pp == 0){
            kprintf_raw("[xHCI error] failed to power on port %i",port);
            return false;
        }
    }

    port_info->portsc.csc = 1;
    port_info->portsc.pec = 1;
    port_info->portsc.prc = 1;

    //TODO: if usb3
    // port_info->portsc.wpr = 1;
    //else 

    if (!AWAIT(0, { port_info->portsc.pr = 1; },TRB_TYPE_PORT_STATUS_CHANGE)){
        kprintf("[xHCI error] failed port reset");
        return false;
    }

    port_info->portsc.prc = 1;
    port_info->portsc.wrc = 1;
    port_info->portsc.csc = 1;
    port_info->portsc.pec = 1;
    port_info->portsc.ped = 0;

    delay(300);

    return port_info->portsc.ped != 0;
}

bool XHCIDriver::enable_events(){
    kprintfv("[xHCI] Allocating ERST");
    interrupter = (xhci_interrupter*)(rt_base + 0x20);

    uint64_t ev_ring = (uintptr_t)allocate_in_page(mem_page, MAX_TRB_AMOUNT * sizeof(trb), ALIGN_64B, true, true);
    uint64_t erst_addr = (uintptr_t)allocate_in_page(mem_page, MAX_ERST_AMOUNT * sizeof(erst_entry), ALIGN_64B, true, true);
    erst_entry* erst = (erst_entry*)erst_addr;

    erst->ring_base = ev_ring;
    erst->ring_size = MAX_TRB_AMOUNT;

    kprintfv("[xHCI] ERST ring_base: %x", ev_ring);
    kprintfv("[xHCI] ERST ring_size: %x", erst[0].ring_size);
    event_ring.ring = (trb*)ev_ring;
    event_ring.cycle_bit = 1;

    kprintfv("[xHCI] Interrupter register @ %x", rt_base + 0x20);
    
    interrupter->erstsz = 1;
    kprintfv("[xHCI] ERSTSZ set to: %x", interrupter->erstsz);
    
    interrupter->erdp = ev_ring;
    interrupter->erstba = erst_addr;
    kprintfv("[xHCI] ERSTBA set to: %x", interrupter->erstba);
    
    kprintfv("[xHCI] ERDP set to: %x", interrupter->erdp);
    
    interrupter->iman |= 1 << 1;//Enable interrupt

    op->usbsts = 1 << 3;//Enable interrupts
    interrupter->iman |= 1;//Clear pending interrupts

    return !check_fatal_error();
}

void XHCIDriver::make_ring_link_control(trb* ring, bool cycle){
    ring[MAX_TRB_AMOUNT-1].control =
        (TRB_TYPE_LINK << 10)
      | (1 << 1)              // Toggle Cycle
      | cycle;
}

void XHCIDriver::make_ring_link(trb* ring, bool cycle){
    ring[MAX_TRB_AMOUNT-1].parameter = (uintptr_t)ring;
    ring[MAX_TRB_AMOUNT-1].status = 0;
    make_ring_link_control(ring, cycle);
}

void XHCIDriver::ring_doorbell(uint32_t slot, uint32_t endpoint) {
    volatile uint32_t* db = (uint32_t*)(uintptr_t)(db_base + (slot << 2));
    kprintfv("[xHCI] Ringing doorbell at %x with value %x", db_base + (slot << 2),endpoint);
    *db = endpoint;
}

bool XHCIDriver::await_response(uint64_t command, uint32_t type){
    while (1){
        if (check_fatal_error()){
            kprintf_raw("[xHCI error] USBSTS value %x",op->usbsts);
            awaited_type = 0;
            return false;
        }
        for (; event_ring.index < MAX_TRB_AMOUNT; event_ring.index++){
            last_event = &event_ring.ring[event_ring.index];
            if (!wait(&last_event->control, event_ring.cycle_bit, true, 2000)){
                kprintf("[xHCI error] Timeout awaiting response to %x command of type %x", command, type);
                awaited_type = 0;
                return false;
            }
            // kprintf_raw("[xHCI] A response at %i of type %x as a response to %x",event_ring.index, (last_event->control & TRB_TYPE_MASK) >> 10, last_event->parameter);
            // kprintf_raw("[xHCI]  %x vs %x = %i and %x vs %x = %i", (ev->control & TRB_TYPE_MASK) >> 10, type, (ev->control & TRB_TYPE_MASK) >> 10 == type, ev->parameter, command, command == 0 || ev->parameter == command);
            if (event_ring.index == MAX_TRB_AMOUNT - 1){
                event_ring.index = 0;
                event_ring.cycle_bit = !event_ring.cycle_bit;
            }
            if ((((last_event->control & TRB_TYPE_MASK) >> 10) == type) && (command == 0 || last_event->parameter == command)){
                uint8_t completion_code = (last_event->status >> 24) & 0xFF;
                if (completion_code != 1) 
                    kprintf("[xHCI error] wrong status %i on command type %x", completion_code, ((last_event->control & TRB_TYPE_MASK) >> 10) );
                interrupter->erdp = (uintptr_t)&event_ring.ring[event_ring.index+1] | (1 << 3);//Inform of latest processed event
                interrupter->iman |= 1;//Clear interrupts
                op->usbsts |= 1 << 3;//Clear interrupts
                awaited_type = 0;
                event_ring.index++;
                return completion_code == 1;
            }
        }
    }
    awaited_type = 0;
    return false;
}

bool XHCIDriver::issue_command(uint64_t param, uint32_t status, uint32_t control){
    trb* cmd = &command_ring.ring[command_ring.index++];
    cmd->parameter = param;
    cmd->status = status;
    cmd->control = control | command_ring.cycle_bit;

    uint64_t cmd_addr = (uintptr_t)cmd;
    kprintfv("[xHCI] issuing command with control: %x from %x", cmd->control, cmd_addr);
    if (command_ring.index == MAX_TRB_AMOUNT - 1){
        make_ring_link_control(command_ring.ring, command_ring.cycle_bit);
        command_ring.cycle_bit = !command_ring.cycle_bit;
        command_ring.index = 0;
    }
    return AWAIT(cmd_addr, {ring_doorbell(0, 0);}, TRB_TYPE_COMMAND_COMPLETION);
}

bool XHCIDriver::setup_device(uint8_t address, uint16_t port){

    if (!issue_command(0,0,TRB_TYPE_ENABLE_SLOT << 10)){
        kprintf_raw("[xHCI error] failed enable slot command");
        return false;
    }

    address = (last_event->control >> 24) & 0xFF;
    kprintfv("[xHCI] Slot id %x", address);

    if (address == 0){
        kprintf_raw("[xHCI error]: Wrong slot id 0");
        return false;
    }

    xhci_ring *transfer_ring = &endpoint_map[address << 8];

    transfer_ring->cycle_bit = 1;

    xhci_input_context *ctx = (xhci_input_context*)allocate_in_page(mem_page, sizeof(xhci_input_context), ALIGN_64B, true, true);
    kprintfv("[xHCI] Allocating input context at %x", (uintptr_t)ctx);
    context_map[address << 8] = ctx;
    void* output_ctx = (void*)allocate_in_page(mem_page, 0x1000, ALIGN_64B, true, true);
    kprintfv("[xHCI] Allocating output for context at %x", (uintptr_t)output_ctx);
    
    ctx->control_context.add_flags = 0b11;
    
    ctx->device_context.slot_f0.speed = ports[port].portsc.port_speed;
    ctx->device_context.slot_f0.context_entries = 1;
    ctx->device_context.slot_f1.root_hub_port_num = port + 1;
    
    ctx->device_context.endpoints[0].endpoint_f0.endpoint_state = XHCI_EP_DISABLED;
    ctx->device_context.endpoints[0].endpoint_f1.endpoint_type = XHCI_EP_CONTROL;
    ctx->device_context.endpoints[0].endpoint_f0.interval = 0;
    ctx->device_context.endpoints[0].endpoint_f1.error_count = 3;
    ctx->device_context.endpoints[0].endpoint_f1.max_packet_size = packet_size(ctx->device_context.slot_f0.speed);
    
    transfer_ring->ring = (trb*)allocate_in_page(mem_page, MAX_TRB_AMOUNT * sizeof(trb), ALIGN_64B, true, true);
    kprintfv("Transfer ring at %x %i",(uintptr_t)transfer_ring->ring, address << 8);
    make_ring_link(transfer_ring->ring, transfer_ring->cycle_bit);

    ctx->device_context.endpoints[0].endpoint_f23.dcs = transfer_ring->cycle_bit;
    ctx->device_context.endpoints[0].endpoint_f23.ring_ptr = ((uintptr_t)transfer_ring->ring) >> 4;
    ctx->device_context.endpoints[0].endpoint_f4.average_trb_length = sizeof(trb);

    dcbaap[address] = (uintptr_t)output_ctx;

    return USBDriver::setup_device(address, port);
}

bool XHCIDriver::request_sized_descriptor(uint8_t address, uint8_t endpoint, uint8_t rType, uint8_t request, uint8_t type, uint16_t descriptor_index, uint16_t wIndex, uint16_t descriptor_size, void *out_descriptor){
    usb_setup_packet packet = {
        .bmRequestType = rType,
        .bRequest = request,
        .wValue = (type << 8) | descriptor_index,
        .wIndex = wIndex,
        .wLength = descriptor_size
    };

    bool is_in = (rType & 0x80) != 0;

    xhci_ring *transfer_ring = &endpoint_map[address << 8 | endpoint];

    trb* setup_trb = &transfer_ring->ring[transfer_ring->index++];
    memcpy(&setup_trb->parameter, &packet, sizeof(packet));
    setup_trb->status = sizeof(usb_setup_packet);
    //bit 4 = chain. Bit 16 direction (3 = IN, 2 = OUT, 0 = NO DATA)
    setup_trb->control = (3 << 16) | (TRB_TYPE_SETUP_STAGE << 10) | (1 << 6) | (1 << 4) | transfer_ring->cycle_bit;
    
    trb* data = &transfer_ring->ring[transfer_ring->index++];
    data->parameter = (uintptr_t)out_descriptor;
    data->status = packet.wLength;
    //bit 16 = direction
    data->control = (1 << 16) | (TRB_TYPE_DATA_STAGE << 10) | (0 << 4) | transfer_ring->cycle_bit;
    
    trb* status_trb = &transfer_ring->ring[transfer_ring->index++];
    status_trb->parameter = 0;
    status_trb->status = 0;
    //bit 5 = interrupt-on-completion
    status_trb->control = (TRB_TYPE_STATUS_STAGE << 10) | (1 << 5) | transfer_ring->cycle_bit;

    if (transfer_ring->index == MAX_TRB_AMOUNT - 1){
        make_ring_link_control(transfer_ring->ring, transfer_ring->cycle_bit);
        transfer_ring->cycle_bit = !transfer_ring->cycle_bit;
        transfer_ring->index = 0;
    }

    return AWAIT((uintptr_t)status_trb, {ring_doorbell(address, 1);}, TRB_TYPE_TRANSFER);
}

uint8_t XHCIDriver::address_device(uint8_t address){
    xhci_input_context* ctx = context_map[address << 8];
    kprintfv("Addressing device %i with context %x", address, (uintptr_t)ctx);
    if (!issue_command((uintptr_t)ctx, 0, (address << 24) | (TRB_TYPE_ADDRESS_DEV << 10))){
        kprintf_raw("[xHCI error] failed addressing device at slot %x",address);
        return 0;
    }
    xhci_device_context* context = (xhci_device_context*)dcbaap[address];

    kprintfv("[xHCI] ADDRESS_DEVICE %i command issued. dcbaap %x Received packet size %i",address, (uintptr_t)dcbaap, context->endpoints[0].endpoint_f1.max_packet_size);
    return address;
}

uint8_t XHCIDriver::get_ep_type(usb_endpoint_descriptor* descriptor) {
    return (descriptor->bEndpointAddress & 0x80 ? 1 << 2 : 0) | (descriptor->bmAttributes & 0x3);
}

uint32_t XHCIDriver::calculate_interval(uint32_t speed, uint32_t received_interval){
    if (speed >= XHCI_SPEED_HIGH_SPEED)
	{
		if (received_interval < 1)
			received_interval = 1;

		if (received_interval > 16)
			received_interval = 16;

		return received_interval-1;
	}

	uint32_t i;
	for (i = 3; i < 11; i++)
		if (125 * (1 << i) >= 1000 * received_interval) break;

	return i;
}

bool XHCIDriver::configure_endpoint(uint8_t address, usb_endpoint_descriptor *endpoint, uint8_t configuration_value, usb_device_types type){
    kprintfv("[xHCI] endpoint address %x",endpoint->bEndpointAddress);
    uint8_t ep_address = endpoint->bEndpointAddress;
    uint8_t ep_dir = (ep_address & 0x80) ? 1 : 0; // 1 IN, 0 OUT
    uint8_t ep_num = ((ep_address & 0x0F) * 2) + ep_dir;

    uint8_t ep_type = endpoint->bmAttributes & 0x03; // 0 = Control, 1 = Iso, 2 = Bulk, 3 = Interrupt

    if (ep_type != 3){ 
        kprintf("[xHCI implementation warning] Endpoint type %i not supported. Ignored",ep_type);
        return true;
    }
    kprintf_raw("[xHCI] endpoint %i info. Direction %i type %i",ep_num, ep_dir, ep_type);

    xhci_input_context* ctx = context_map[address << 8];
    xhci_device_context* context = (xhci_device_context*)dcbaap[address];

    ctx->control_context.add_flags = (1 << 0) | (1 << ep_num);
    if (ep_num > ctx->device_context.slot_f0.context_entries)
        ctx->device_context.slot_f0.context_entries = ep_num;
    ctx->device_context.slot_f0.speed = context->slot_f0.speed;
    ctx->device_context.endpoints[ep_num-1].endpoint_f0.interval = calculate_interval(context->slot_f0.speed, endpoint->bInterval);
    
    ctx->device_context.endpoints[ep_num-1].endpoint_f0.endpoint_state = XHCI_EP_DISABLED;
    ctx->device_context.endpoints[ep_num-1].endpoint_f1.endpoint_type = get_ep_type(endpoint);
    ctx->device_context.endpoints[ep_num-1].endpoint_f1.max_packet_size = endpoint->wMaxPacketSize;
    ctx->device_context.endpoints[ep_num-1].endpoint_f4.max_esit_payload_lo = endpoint->wMaxPacketSize;
    ctx->device_context.endpoints[ep_num-1].endpoint_f1.error_count = 3;
    
    xhci_ring *ep_ring = &endpoint_map[address << 8 | ep_num];
    
    ep_ring->ring = (trb*)allocate_in_page(mem_page, MAX_TRB_AMOUNT * sizeof(trb), ALIGN_64B, true, true);
    ep_ring->cycle_bit = 1;
    make_ring_link(ep_ring->ring, ep_ring->cycle_bit);
    ctx->device_context.endpoints[ep_num-1].endpoint_f23.dcs = ep_ring->cycle_bit;
    ctx->device_context.endpoints[ep_num-1].endpoint_f23.ring_ptr = ((uintptr_t)ep_ring->ring) >> 4;
    ctx->device_context.endpoints[ep_num-1].endpoint_f4.average_trb_length = sizeof(trb);

    if (!issue_command((uintptr_t)ctx, 0, (address << 24) | (TRB_TYPE_CONFIG_EP << 10))){
        kprintf_raw("[xHCI] Failed to configure endpoint %i for address %i",ep_num,address);
        return false;
    }

    usb_manager->register_endpoint(address, ep_num, type, endpoint->wMaxPacketSize);
    usb_manager->request_data(address, ep_num, this);
    
    return true;
}

void XHCIDriver::handle_hub_routing(uint8_t hub, uint8_t port){
    //TODO: hubs
}

bool XHCIDriver::poll(uint8_t address, uint8_t endpoint, void *out_buf, uint16_t size){
    xhci_ring *ep_ring = &endpoint_map[address << 8 | endpoint];

    trb* ev = &ep_ring->ring[ep_ring->index++];

    ev->parameter = (uintptr_t)out_buf;
    ev->status = size;
    ev->control = (TRB_TYPE_NORMAL << 10) | (1 << 5) | ep_ring->cycle_bit;

    if (ep_ring->index == MAX_TRB_AMOUNT - 1){
        make_ring_link_control(ep_ring->ring, ep_ring->cycle_bit);
        ep_ring->cycle_bit = !ep_ring->cycle_bit;
        ep_ring->index = 0;
    }

    ring_doorbell(address, endpoint);

    return true;
}

void XHCIDriver::handle_interrupt(){
    trb* ev = &event_ring.ring[event_ring.index];
    if (!((ev->control & 1) == event_ring.cycle_bit)) return;
    uint32_t type = (ev->control & TRB_TYPE_MASK) >> 10;
    uint64_t addr = ev->parameter;
    if (type == awaited_type && (awaited_addr == 0 || (awaited_addr & 0xFFFFFFFFFFFFFFFF) == addr))
        return;
    kprintfv("[xHCI] >>> Unhandled interrupt %i %x",event_ring.index,type);
    uint8_t completion_code = (ev->status >> 24) & 0xFF;
    if (completion_code == 1){
        switch (type){
            case TRB_TYPE_TRANSFER: {
                uint8_t slot_id = (ev->control & TRB_SLOT_MASK) >> 24;
                uint8_t endpoint_id = (ev->control & TRB_ENDPOINT_MASK) >> 16;
                kprintfv("Received input from slot %i endpoint %i",slot_id, endpoint_id);
                
                usb_manager->process_data(slot_id,endpoint_id, this);
                break;
            }
            case TRB_TYPE_PORT_STATUS_CHANGE: {
                kprintfv("[xHCI] Port status change. Ignored for now");
                break;
            }
        }
    } else {
        kprintf("[xHCI error] wrong status %i on command type %x", completion_code, ((ev->control & TRB_TYPE_MASK) >> 10));
    }
    event_ring.index++;
    if (event_ring.index == MAX_TRB_AMOUNT - 1){
        event_ring.index = 0;
        event_ring.cycle_bit = !event_ring.cycle_bit;
    }
    interrupter->erdp = (uintptr_t)&event_ring.ring[event_ring.index] | (1 << 3);//Inform of latest processed event
    interrupter->iman |= 1;//Clear interrupts
    op->usbsts |= 1 << 3;//Clear interrupts
}