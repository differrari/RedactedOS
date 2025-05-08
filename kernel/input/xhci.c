#include "xhci.h"
#include "console/kio.h"
#include "console/serial/uart.h"
#include "pci.h"
#include "ram_e.h"

#define TRB_TYPE_NORMAL      1
#define TRB_TYPE_LINK        6
#define TRB_TYPE_EVENT_DATA  3
#define TRB_TYPE_TRANSFER    32
#define TRB_TYPE_ENABLE_SLOT 9
#define TRB_TYPE_ADDRESS_DEV 11
#define TRB_TYPE_CONFIG_EP   12
#define TRB_TYPE_SETUP       2
#define TRB_TYPE_STATUS      4
#define TRB_TYPE_INPUT       8

#define MAX_TRB_AMOUNT 256
#define MAX_ERST_AMOUNT 1

#define TRB_TYPE_MASK 0xFC00

#define TRB_TYPE_COMMAND_COMPLETION 0x21
#define TRB_TYPE_PORT_STATUS_CHANGE 0x22

#define TRB_TYPE_SETUP_STAGE 0x2
#define TRB_TYPE_DATA_STAGE 0x3
#define TRB_TYPE_STATUS_STAGE 0x4

#define XHCI_USBSTS_HSE (1 << 2)
#define XHCI_USBSTS_CE  (1 << 12)

typedef struct {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
}__attribute__((packed)) trb;

typedef struct {
    uint8_t caplength;
    uint8_t reserved;
    uint16_t hciversion;
    uint32_t hcsparams1;
    uint32_t hcsparams2;
    uint32_t hcsparams3;
    uint32_t hccparams1;
    uint32_t dboff;
    uint32_t rtsoff;
    uint32_t hccparams2;
}__attribute__((packed)) xhci_cap_regs;

typedef struct {
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t pagesize;
    uint64_t reserved0;
    uint32_t dnctrl;
    uint64_t crcr;
    uint32_t reserved1[4];
    uint64_t dcbaap;
    uint32_t config;
}__attribute__((packed)) xhci_op_regs;

typedef union {
    struct {
        uint32_t    ccs         : 1;
        uint32_t    ped         : 1;
        uint32_t    rsvd0       : 1;
        uint32_t    oca         : 1;
        uint32_t    pr          : 1;
        uint32_t    pls         : 4;
        uint32_t    pp          : 1;
        uint32_t    port_speed  : 4;
        uint32_t    pic         : 2;
        uint32_t    lws         : 1;
        uint32_t    csc         : 1;
        uint32_t    pec         : 1;
        uint32_t    wrc         : 1;
        uint32_t    occ         : 1;
        uint32_t    prc         : 1;
        uint32_t    plc         : 1;
        uint32_t    cec         : 1;
        uint32_t    cas         : 1;
        uint32_t    wce         : 1;
        uint32_t    wde         : 1;
        uint32_t    woe         : 1;
        uint32_t    rsvd1        : 2;
        uint32_t    dr          : 1;
        uint32_t    wpr         : 1;
    };
    uint32_t value;
} portsc;

typedef struct {
    portsc portsc;
    uint32_t portpmsc;
    uint32_t portli;
    uint32_t rsvd;
}__attribute__((packed, aligned(4))) xhci_port_regs;

typedef struct {
    uint32_t iman;
    uint32_t imod;
    uint32_t erstsz;
    uint32_t reserved;
    uint64_t erstba;
    uint64_t erdp;
}__attribute__((packed)) xhci_interrupter;

typedef struct {
    uint64_t mmio;
    uint64_t mmio_size;
    xhci_cap_regs* cap;
    xhci_op_regs* op;
    xhci_port_regs* ports;
    uint64_t db_base;
    uint64_t rt_base;
    trb* cmd_ring;
    uint32_t cmd_index;
    uint32_t event_index;
    bool command_cycle_bit;
    bool event_cycle_bit;
    trb* event_ring;
    uint8_t* key_buffer;
    xhci_interrupter* interrupter;
    uint64_t* dcbaa;
    uint16_t max_device_slots;
    uint16_t max_ports;
} xhci_device;

typedef struct {
    bool transfer_cycle_bit;
    uint32_t transfer_index;
    trb* transfer_ring;
    uint32_t slot_id;
} xhci_usb_device;

typedef struct {
    uint64_t ring_base;
    uint32_t ring_size;
    uint32_t reserved;
}__attribute__((packed)) erst_entry;

typedef struct {
    uint32_t drop_flags;
    uint32_t add_flags;
    uint64_t reserved[3];
}__attribute__((packed)) xhci_input_control_context;

typedef union
{
    struct 
    {
        uint32_t route_string: 20;
        uint32_t speed: 4;
        uint32_t rsvd : 1;
        uint32_t mtt : 1;
        uint32_t hub : 1;
        uint32_t context_entries : 5;
    };
    uint32_t value;
} slot_field0;

typedef union
{
    struct 
    {
        uint16_t    max_exit_latency;
        uint8_t     root_hub_port_num;
        uint8_t     port_count;
    };
    uint32_t value;
} slot_field1;

typedef union
{
    struct 
    {
        uint32_t parent_hub_slot_id : 8;
        uint32_t parent_port_number : 8;
        uint32_t think_time : 2;
        uint32_t rsvd : 4;
        uint32_t interrupt_target : 10;
    };
    uint32_t value;
} slot_field2;

typedef union
{
    struct 
    {
        uint32_t device_address : 8;
        uint32_t rsvd : 19;
        uint32_t state : 5;//0 disabled, 1 default, 2 addressed, 3 configured
    };
    uint32_t value;
} slot_field3;

typedef union 
{
    struct {
        uint32_t endpoint_state        : 3;
        uint32_t rsvd0                 : 5;
        uint32_t mult                  : 2;
        uint32_t max_primary_streams   : 5;
        uint32_t linear_stream_array   : 1;
        uint32_t interval              : 8;
        uint32_t max_esit_payload_hi   : 8;
    };
    uint32_t value;
} endpoint_field0;

typedef union 
{
    struct {
        uint32_t rsvd1                 : 1;
        uint32_t error_count             : 2;
        uint32_t endpoint_type           : 3;
        uint32_t rsvd2                   : 1;
        uint32_t host_initiate_disable   : 1;
        uint32_t max_burst_size          : 8;
        uint32_t max_packet_size         : 16;
    };
    uint32_t value;
} endpoint_field1;

typedef union {
    struct {
        uint64_t dcs : 1;
        uint64_t rsvd0 : 3;
        uint64_t ring_ptr : 60;
    };
    uint64_t value;
} endpoint_field23;

typedef union 
{
    struct {
        uint16_t average_trb_length;
        uint16_t max_esit_payload_lo;
    };
    uint32_t value;
} endpoint_field4;

typedef struct {
    slot_field0 slot_f0;
    slot_field1 slot_f1;
    slot_field2 slot_f2;
    slot_field3 slot_f3;
    uint32_t slot_rsvd[4];
    endpoint_field0 endpoint_f0;
    endpoint_field1 endpoint_f1;
    endpoint_field23 endpoint_f23;
    endpoint_field4 endpoint_f4;
    uint32_t ep0_rsvd[3];
} xhci_device_context;

typedef struct {
    xhci_input_control_context control_context;
    xhci_device_context device_context;
} xhci_input_context;

typedef struct __attribute__((packed)) {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_packet;

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
} usb_descriptor_header ;

typedef struct __attribute__((packed)) {
    usb_descriptor_header header;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_descriptor;

typedef struct __attribute__((packed)) {
    usb_descriptor_header header;
    uint16_t lang_ids[126];
} usb_string_language_descriptor;

typedef struct __attribute__((packed)){
    usb_descriptor_header header;
    uint16_t unicode_string[126];
} usb_string_descriptor;

#define USB_DEVICE_DESCRIPTOR 1
#define USB_CONFIGURATION_DESCRIPTOR 2
#define USB_STRING_DESCRIPTOR 3

static trb transfer_ring[64]__attribute__((aligned(64)));

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

bool xhci_init(xhci_device *xhci, uint64_t pci_addr) {
    kprintfv("[xHCI] init");
    if (!(read16(pci_addr + 0x06) & (1 << 4))){
        kprintfv("[xHCI] Wrong capabilities list");
        return false;
    }

    uint8_t cap_ptr = read8(pci_addr + 0x34);
    while (cap_ptr) {
        uint8_t cap_id = read8(pci_addr + cap_ptr);
        if (cap_id == 0x11) { // MSI-X
            uint16_t msg_ctrl = read16(pci_addr + cap_ptr + 0x2);
            msg_ctrl &= ~(1 << 15); // Clear MSI-X Enable bit
            write16(pci_addr + cap_ptr + 0x2, msg_ctrl);
            kprintf("MSI-X disabled");
            break;
        }
        if (cap_id == 0x05) { // MSI
            uint16_t msg_ctrl = read16(pci_addr + cap_ptr + 0x2);
            msg_ctrl &= ~(1 << 0); // Clear MSI Enable bit
            write16(pci_addr + cap_ptr + 0x2, msg_ctrl);
            kprintf("MSI disabled");
            break;
        }
        cap_ptr = read8(pci_addr + cap_ptr + 1); // Next capability
    }

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

    //TODO: link trb as the last entry. Param as command ring base, control = Link (6 << 10) + toggle cycle + current cycle bit 

    xhci->cmd_ring = (trb*)command_ring;
    xhci->cmd_index = 0;
    xhci->op->crcr = command_ring | xhci->command_cycle_bit;

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


static void ring_doorbell(uint32_t slot, uint32_t endpoint) {
    volatile uint32_t* db = (uint32_t*)(uintptr_t)(global_device.db_base + (slot << 2));
    kprintfv("[xHCI] Ringing doorbell at %h with value %h", global_device.db_base + (slot << 2),endpoint);
    *db = endpoint;
}

bool await_response(uint64_t command, uint32_t type){
    while (true) {
        if (xhci_check_fatal_error()){
            kprintf("[xHCI error] USBSTS value %h",global_device.op->usbsts);
            return false;
        }
        //We could optimize this by looking at which events we've already processed, but maybe we risk skipping some?
        for (global_device.event_index; global_device.event_index < MAX_TRB_AMOUNT; global_device.event_index++){
            trb* ev = &global_device.event_ring[global_device.event_index];
            if (!(ev->control & global_device.event_cycle_bit)) //TODO: implement a timeout
                break;
            kprintfv("[xHCI] A response at %i of type %h as a response to %h",global_device.event_index, (ev->control & TRB_TYPE_MASK) >> 10, ev->parameter);
            if (global_device.event_index == MAX_TRB_AMOUNT - 1){
                global_device.event_index = 0;
                global_device.event_cycle_bit = !global_device.event_cycle_bit;
            }
            if ((ev->control & TRB_TYPE_MASK) >> 10 == type && (command == 0 || ev->parameter == (command & 0xFFFFFFFFFFFFFFFF))){
                kprintfv("[xHCI] Received response %h", (ev->control >> 24) & 0xFF);
                global_device.interrupter->erdp = (uintptr_t)ev | (1 << 3);//Inform of latest processed event
                global_device.interrupter->iman |= 1;//Clear interrupts
                global_device.op->usbsts |= 1 << 3;//Clear interrupts
                return (ev->control >> 24) & 0xFF;
            }
        }
    }
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
    return await_response(cmd_addr, TRB_TYPE_COMMAND_COMPLETION);
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

    return await_response(0, TRB_TYPE_PORT_STATUS_CHANGE);
}

bool xhci_request_sized_descriptor(xhci_usb_device *device, uint8_t type, uint16_t descriptor_index, uint16_t wIndex, uint16_t descriptor_size, void *out_descriptor){
    usb_setup_packet packet = {
        .bmRequestType = 0x80,
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
    if (!await_response((uintptr_t)status_trb, TRB_TYPE_TRANSFER)){
        kprintf("[xHCI error] error fetching descriptor");
    }
    return true;
}

bool xhci_request_descriptor(xhci_usb_device *device, uint8_t type, uint16_t index, uint16_t wIndex, void *out_descriptor){
    if (!xhci_request_sized_descriptor(device,type, index, wIndex, sizeof(usb_descriptor_header), out_descriptor)){
        kprintf("[xHCI error] Failed to get descriptor header. Size %i", sizeof(usb_descriptor_header));
        return false;
    }
    usb_descriptor_header* descriptor = (usb_descriptor_header*)out_descriptor;
    if (descriptor->bLength == 0){
        kprintf("[xHCI error] wrong descriptor size %i",descriptor->bLength);
        return false;
    }
    return xhci_request_sized_descriptor(device, type, index, wIndex, descriptor->bLength, out_descriptor);
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

bool xhci_setup_device(uint16_t port){

    kprintfv("[xHCI] detecting and activating the default device at %i port. Resetting port...",port);
    reset_port(port);
    kprintfv("[xHCI] Port speed %i", (uint32_t)global_device.ports[port].portsc.port_speed);

    if (!issue_command(0,0,TRB_TYPE_ENABLE_SLOT << 10)){
        kprintf("[xHCI error] failed enable slot command");
        return false;
    }

    xhci_usb_device device;

    device.slot_id = (global_device.event_ring[0].status >> 24) & 0xFF;
    kprintfv("[xHCI] Slot id %h", device.slot_id);

    if (device.slot_id == 0){
        kprintf("[xHCI error]: Wrong slot id 0");
        return false;
    }

    device.transfer_cycle_bit = 1;

    xhci_input_context* ctx = (xhci_input_context*)alloc_dma_region(0x1000);
    void* output_ctx = (void*)alloc_dma_region(4096);
    
    ctx->control_context.add_flags = 0b11;
    
    ctx->device_context.slot_f0.speed = global_device.ports[port].portsc.port_speed;
    ctx->device_context.slot_f0.context_entries = 1;
    ctx->device_context.slot_f1.root_hub_port_num = port + 1;
    
    ctx->device_context.endpoint_f0.endpoint_state = 0;//Disabled
    ctx->device_context.endpoint_f1.endpoint_type = 4;//Type = control
    ctx->device_context.endpoint_f0.interval = 0;
    ctx->device_context.endpoint_f1.error_count = 3;//3 errors allowed
    ctx->device_context.endpoint_f1.max_packet_size = packet_size(ctx->device_context.slot_f0.speed);//Packet size. Guessed from port speed
    
    device.transfer_ring = (trb*)alloc_dma_region(0x1000);

    ctx->device_context.endpoint_f23.dcs = device.transfer_cycle_bit;
    ctx->device_context.endpoint_f23.ring_ptr = ((uintptr_t)device.transfer_ring) >> 4;
    ctx->device_context.endpoint_f4.average_trb_length = 8;

    ((uint64_t*)(uintptr_t)global_device.op->dcbaap)[device.slot_id] = (uintptr_t)output_ctx;
    if (!issue_command((uintptr_t)ctx, 0, (device.slot_id << 24) | (TRB_TYPE_ADDRESS_DEV << 10))){
        kprintf("[xHCI error] failed addressing device at slot %h",device.slot_id);
        return false;
    }

    xhci_device_context* context = (xhci_device_context*)(uintptr_t)(global_device.dcbaa[device.slot_id]);

    kprintfv("[xHCI] ADDRESS_DEVICE command issued. Received package size %i",context->endpoint_f1.max_packet_size);

    usb_device_descriptor* descriptor = (usb_device_descriptor*)alloc_dma_region(sizeof(usb_device_descriptor));
    
    if (!xhci_request_descriptor(&device, USB_DEVICE_DESCRIPTOR, 0, 0, descriptor)){
        kprintf("[xHCI error] failed to get device descriptor");
        return false;
    }

    usb_string_language_descriptor* lang_desc = (usb_string_language_descriptor*)alloc_dma_region(sizeof(usb_string_language_descriptor));

    bool use_lang_desc = true;

    if (!xhci_request_descriptor(&device, USB_STRING_DESCRIPTOR, 0, 0, lang_desc)){
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
        if (xhci_request_descriptor(&device, USB_STRING_DESCRIPTOR, descriptor->iProduct, langid, prod_name)){
            char name[128];
            if (parse_string_descriptor_utf16le(prod_name->unicode_string, name, sizeof(name))) {
                kprintf("[xHCI device] Product name: %s", (uint64_t)name);
            }
        }
        usb_string_descriptor* man_name = (usb_string_descriptor*)alloc_dma_region(sizeof(usb_string_descriptor));
        if (xhci_request_descriptor(&device, USB_STRING_DESCRIPTOR, descriptor->iManufacturer, langid, man_name)){
            char name[128];
            if (parse_string_descriptor_utf16le(man_name->unicode_string, name, sizeof(name))) {
                kprintf("[xHCI device] Manufacturer name: %s", (uint64_t)name);
            }
        }
        usb_string_descriptor* ser_name = (usb_string_descriptor*)alloc_dma_region(sizeof(usb_string_descriptor));
        if (xhci_request_descriptor(&device, USB_STRING_DESCRIPTOR, descriptor->iSerialNumber, langid, ser_name)){
            char name[128];
            if (parse_string_descriptor_utf16le(ser_name->unicode_string, name, sizeof(name))) {
                kprintf("[xHCI device] Serial: %s", (uint64_t)name);
            }
        }
    }

    return false;

    //Manually set the rest
    // ctx->control_context.drop_flags = 0;
    // ctx->control_context.add_flags = 0b01;//1 << 1;
    // ctx->device_context.slot_f0.context_entries = 1;

    // ctx->device_context.endpoint_f1.max_packet_size = context->endpoint_f1.max_packet_size;

    // if (!issue_command((uintptr_t)ctx, 0, (slot_id << 24) | (TRB_TYPE_CONFIG_EP << 10))){
    //     kprintf("[xHCI error] failed configure endpoint on slot %h",slot_id);
    //     return false;
    // }

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

    int port = -1;
    for (uint16_t i = 0; i < global_device.max_ports; i++)//qemu only
        if (global_device.ports[i].portsc.ccs && global_device.ports[i].portsc.csc){
            port = i;
            break;
        }

    if (port == -1){
        kprintf("[xHCI error] no device connected. If we're in QEMU this is an error, if not, we'll need to revise this code");
        return false;
    }

    return xhci_setup_device(port);//Default device

}

bool xhci_key_ready() {
    return false;//global_device.event_ring[0].control & global_device.cycle_bit;
}

int xhci_read_key() {
    // global_device.event_ring[0].control &= ~global_device.cycle_bit;
    return 0;//global_device.key_buffer[2];
}

void test_keyboard_input() {
    kprintf("[NEC] Waiting for input");
    while (true) {
        if (xhci_key_ready()) {
            int ch = xhci_read_key();
            kprintf("Key: %c", ch);
        }
    }
}

