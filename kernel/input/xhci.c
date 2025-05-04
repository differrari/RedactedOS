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

typedef struct {
    uint64_t mmio;
    uint64_t mmio_size;
    xhci_cap_regs* cap;
    xhci_op_regs* op;
    uint64_t db_base;
    uint64_t rt_base;
    trb* cmd_ring;
    int cmd_index;
    bool cycle_bit;
    trb* event_ring;
    uint8_t* key_buffer;
} xhci_device;

typedef struct {
    uint32_t iman;
    uint32_t imod;
    uint32_t erstsz;
    uint32_t reserved;
    uint64_t erstba;
    uint64_t erdp;
}__attribute__((packed)) xhci_interrupter;

typedef struct {
    uint64_t ring_base;
    uint32_t ring_size;
    uint32_t reserved;
}__attribute__((packed)) erst_entry;

typedef struct {
    uint32_t drop_flags;
    uint32_t add_flags;
    uint64_t reserved[5];
    uint32_t slot_ctx[8];
    uint32_t ep0_ctx[8];
} xhci_input_context;

typedef struct __attribute__((packed)) {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_packet;

static trb transfer_ring[64]__attribute__((aligned(64)));

static xhci_device global_device;

static bool nec_verbose = false;

void nec_enable_verbose(){
    nec_verbose = true;
}

#define kprintfv(fmt, ...) \
    ({ \
        if (nec_verbose){\
            uint64_t _args[] = { __VA_ARGS__ }; \
            kprintf_args((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
        }\
    })

bool enable_xhci_interrupts(){
    kprintfv("[xHCI] Allocating ERST");
    xhci_interrupter* int0 = (xhci_interrupter*)(uintptr_t)(global_device.rt_base + 0x20);
    int0->iman |= 1;

    uint64_t event_ring = palloc(MAX_TRB_AMOUNT * sizeof(trb));
    uint64_t erst_addr = palloc(sizeof(erst_entry) * MAX_ERST_AMOUNT);
    erst_entry* erst = (erst_entry*)erst_addr;

    erst->ring_base = event_ring;
    erst->ring_size = MAX_TRB_AMOUNT;
    erst->reserved = 0;
    kprintfv("[xHCI] ERST ring_base: %h", event_ring);
    kprintfv("[xHCI] ERST ring_size: %h", erst[0].ring_size);
    global_device.event_ring = (trb*)event_ring;

    kprintfv("[xHCI] Interrupter register @ %h", global_device.rt_base + 0x20);
    
    kprintfv("iman cleared %h",int0->iman);
    int0->erstsz = 1;
    kprintfv("[xHCI] ERSTSZ set to: %h", int0->erstsz);
    
    int0->erdp = event_ring;
    kprintf("USBSTS is %h before",global_device.op->usbsts);
    int0->erstba = erst_addr;
    kprintfv("[xHCI] ERSTBA set to: %h", int0->erstba);
    kprintf("USBSTS is %h after",global_device.op->usbsts);
    
    kprintfv("[xHCI] ERDP set to: %h", int0->erdp);
    
    int0->iman |= 1 << 1;//Enable interrupt
    kprintfv("[xHCI] IMAN after enable: %h", int0->iman);
    kprintfv("[xHCI] ERSTSZ readback: %h", int0->erstsz);
    kprintfv("[xHCI] ERSTBA readback: %h", int0->erstba);
    kprintfv("[xHCI] ERDP readback: %h", int0->erdp);

    global_device.op->usbsts = 1 << 3;
    int0->iman = 1;//Clear pending interrupts

    return true;

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

    xhci->cycle_bit = 1;

    uint32_t max_device_slots = xhci->cap->hcsparams1 & 0xFF;

    uint16_t erst_max = ((xhci->cap->hcsparams2 >> 4) & 0xF);
    
    kprintfv("[xHCI] ERST Max: 2^%i",erst_max);
    
    xhci->op->dnctrl = 0xFFFF;//Enable device notifications

    xhci->op->config = max_device_slots;
    kprintfv("[xHCI] %i device slots", max_device_slots);

    uint64_t dcbaap_addr = alloc_dma_region((max_device_slots + 1) * sizeof(uintptr_t));

    xhci->op->dcbaap = dcbaap_addr;

    xhci->op->pagesize = 0b1;//4KB page size

    uint32_t scratchpad_count = ((xhci->cap->hcsparams2 >> 27) & 0x1F);

    uint64_t* dcbaap = (uint64_t *)dcbaap_addr;

    uint64_t* scratchpad_array = (uint64_t*)alloc_dma_region(scratchpad_count * sizeof(uint64_t));
    for (uint32_t i = 0; i < scratchpad_count; i++)
        scratchpad_array[i] = alloc_dma_region(0x1000);
    dcbaap[0] = (uint64_t)scratchpad_array;

    kprintfv("[xHCI] dcbaap assigned at %h with %i scratchpads",dcbaap_addr,scratchpad_count);

    uint64_t command_ring = alloc_dma_region(MAX_TRB_AMOUNT * sizeof(trb));

    //TODO: link trb as the last entry. Param as command ring base, control = Link (6 << 10) + toggle cycle + current cycle bit 

    xhci->cmd_ring = (trb*)command_ring;
    xhci->cmd_index = 0;
    xhci->op->crcr = command_ring | xhci->cycle_bit;

    kprintfv("[xHCI] command ring allocated at %h. crcr now %h",command_ring, xhci->op->crcr);

    if (!enable_xhci_interrupts())
        return false;

    kprintfv("[xHCI] interrupt configuration finished");

    xhci->op->usbcmd |= (1 << 2);//Interrupt enable
    xhci->op->usbcmd |= 1;
    while ((xhci->op->usbsts & 0x1));

    kprintfv("[xHCI] Init complete with usbcmd %h",xhci->op->usbcmd);

    return true;
}


static void ring_doorbell(uint32_t slot, uint32_t endpoint) {
    volatile uint32_t* db = (uint32_t*)(uintptr_t)(global_device.db_base + (slot << 2));
    kprintf("Ringing doorbell at %h with value %h", global_device.db_base + (slot << 2),endpoint);
    *db = endpoint;
}

void issue_command(uint64_t param, uint32_t status, uint32_t control){
    trb* cmd = &global_device.cmd_ring[global_device.cmd_index++];
    cmd->parameter = param;
    cmd->status = status;
    cmd->control = control | global_device.cycle_bit;
    ring_doorbell(0, 0);
    kprintfv("cmd_ring[0] param: %h", global_device.cmd_ring[0].parameter);
    kprintfv("cmd_ring[0] status: %h", global_device.cmd_ring[0].status);
    kprintfv("cmd_ring[0] control: %h", global_device.cmd_ring[0].control);
    //TODO: check for cycle.
}

void poll_xhci_events() {
    trb* ev = &global_device.event_ring[0];
    if (!(ev->control & global_device.cycle_bit)) return;

    uint32_t type = (ev->control >> 10) & 0x3F;
    if (type == TRB_TYPE_TRANSFER) {
        uint8_t* data = (uint8_t*)(uintptr_t)(ev->parameter);
        kprintf("[xHCI] Key data: %h", data[2]);
        global_device.key_buffer = data;
    }

    // ev->control &= ~global_device.cycle_bit;
}

void submit_interrupt_in_trb(uint64_t ep_ring_addr, void* buf, uint32_t len) {
    trb* tr = (trb*)ep_ring_addr;
    tr[0].parameter = (uint64_t)(uintptr_t)buf;
    tr[0].status = len;
    tr[0].control = (TRB_TYPE_TRANSFER << 10) | (1 << 5) | global_device.cycle_bit;
    ring_doorbell(1 /*slot_id*/, 2 /*EP2 IN*/);
}

bool nec_input_init() {
    uint64_t addr = find_pci_device(0x1B36, 0xD);
    if (!addr){ 
        kprintf("[PCI] Input device not found");
        return false;
    }

    if (!xhci_init(&global_device, addr)){
        kprintf("Device initialization failed");
        return false;
    }

    kprintf("usbcmd %h usbsts %h",global_device.op->usbcmd, global_device.op->usbsts);

    issue_command(0,0,TRB_TYPE_ENABLE_SLOT << 10);

    // while (!(global_device.event_ring[0].control & global_device.cycle_bit)) {
        // kprintf("Waiting for completion %h",global_device.event_ring[0].control);
    // }

    kprintf("Event TRB param: %h", global_device.event_ring[0].parameter);
    kprintf("Event TRB status: %h", global_device.event_ring[0].status);
    kprintf("Event TRB control: %h", global_device.event_ring[0].control);
    uint32_t type = (global_device.event_ring[0].control >> 10) & 0x3F;
    kprintf("Event TRB type: %h", type);

    uint32_t slot_id = (global_device.event_ring[0].status >> 24) & 0xFF;
    kprintfv("Slot id %h", slot_id);

    if (slot_id == 0){
        kprintf("[xHCI error]: Wrong slot id 0");
        return false;
    }

    void* input_ctx = (void*)alloc_dma_region(0x1000);
    kprintfv("input_ctx: %h", (uint64_t)(uintptr_t)input_ctx);

    ((uint64_t*)(uintptr_t)global_device.op->dcbaap)[slot_id] = (uint64_t)(uintptr_t)input_ctx;
    kprintfv("dcbaap[%d] = %h", slot_id, ((uint64_t*)(uintptr_t)global_device.op->dcbaap)[slot_id]);

    issue_command((uint64_t)input_ctx, slot_id << 24, TRB_TYPE_ADDRESS_DEV);
    kprintfv("ADDRESS_DEV command issued");

    xhci_input_context* ctx = (xhci_input_context*)input_ctx;
    ctx->add_flags = 0b11;
    kprintfv("add_flags = %h", ctx->add_flags);

    ctx->slot_ctx[0] = 1;
    ctx->slot_ctx[1] = 0x03 << 27;
    kprintfv("slot_ctx[0] = %h", ctx->slot_ctx[0]);
    kprintfv("slot_ctx[1] = %h", ctx->slot_ctx[1]);

    ctx->ep0_ctx[1] = (8 << 16) | (3 << 3);
    ctx->ep0_ctx[2] = 0x200;
    kprintfv("ep0_ctx[1] = %h", ctx->ep0_ctx[1]);
    kprintfv("ep0_ctx[2] = %h", ctx->ep0_ctx[2]);

    trb* ep0_ring = (trb*)alloc_dma_region(0x1000);
    ctx->ep0_ctx[2] = (uint64_t)(uintptr_t)ep0_ring | 1;
    kprintfv("ep0_ring = %h", (uint64_t)(uintptr_t)ep0_ring);
    kprintfv("ep0_ctx[2] updated to = %h", ctx->ep0_ctx[2]);

    usb_setup_packet* setup = (usb_setup_packet*)alloc_dma_region(0x1000);
    *setup = (usb_setup_packet){
        .bmRequestType = 0x80,
        .bRequest = 6,
        .wValue = 0x0100,
        .wIndex = 0,
        .wLength = 18
    };
    kprintfv("setup TRB = %h", (uint64_t)(uintptr_t)setup);

    ep0_ring[0] = (trb){
        .parameter = (uint64_t)(uintptr_t)setup,
        .status = 8,
        .control = (TRB_TYPE_SETUP << 10) | global_device.cycle_bit
    };
    kprintfv("TRB0 param = %h", ep0_ring[0].parameter);
    kprintfv("TRB0 status = %h", ep0_ring[0].status);
    kprintfv("TRB0 control = %h", ep0_ring[0].control);

    void* data_buf = (void*)alloc_dma_region(0x1000);
    ep0_ring[1] = (trb){
        .parameter = (uint64_t)(uintptr_t)data_buf,
        .status = 18,
        .control = (TRB_TYPE_NORMAL << 10) | (1 << 5) | global_device.cycle_bit
    };
    kprintfv("TRB1 param = %h", ep0_ring[1].parameter);
    kprintfv("TRB1 status = %h", ep0_ring[1].status);
    kprintfv("TRB1 control = %h", ep0_ring[1].control);

    ep0_ring[2] = (trb){
        .parameter = 0,
        .status = 0,
        .control = (TRB_TYPE_STATUS << 10) | global_device.cycle_bit
    };
    kprintfv("TRB2 control = %h", ep0_ring[2].control);

    ring_doorbell(slot_id, 1);
    kprintfv("Doorbell rung for EP0");

    while (!(global_device.event_ring[0].control & global_device.cycle_bit)) {
        kprintf("Waiting for descriptor %h", global_device.event_ring[0].control);
    }
    
    uint32_t completion_code = (global_device.event_ring[0].control >> 24) & 0xFF;
    if (completion_code != 1) { // 1 = Success
        kprintf("Descriptor request failed, code: %h", completion_code);
        return false;
    }
    // issue_command((uint64_t)(uintptr_t)ctx, slot_id << 24, TRB_TYPE_CONFIG_EP);
    
    uint8_t* desc_buf = (uint8_t*)data_buf;
    uint8_t interface_number = 0;
    uint8_t endpoint_address = 0;
    uint16_t max_packet_size = 0;
    uint8_t interval = 0;
    
    kprintf("2");
    
    for (int i = 0; i < 18 + 64; i += desc_buf[i]) {
        kprintf("AB %i",i);
        if (desc_buf[i + 1] == 0x04) interface_number = desc_buf[i + 2];
        if (desc_buf[i + 1] == 0x05 && (desc_buf[i + 3] & 0x03) == 0x03) {
            kprintf("A");
            endpoint_address = desc_buf[i + 2];
            max_packet_size = desc_buf[i + 4] | (desc_buf[i + 5] << 8);
            interval = desc_buf[i + 6];
            break;
        }
    }
    kprintf("1");
    static uint8_t key_buffer[4096] __attribute__((aligned(64)));
    trb* ep_ring = (trb*)alloc_dma_region(0x1000);
    ep_ring[0].parameter = (uint64_t)(uintptr_t)key_buffer;
    ep_ring[0].status = sizeof(key_buffer);
    ep_ring[0].control = (TRB_TYPE_TRANSFER << 10) | (1 << 5) | global_device.cycle_bit;
    ring_doorbell(slot_id, 2);
    
    kprintf("3");

    trb* intr_ring = (trb*)alloc_dma_region(0x1000);
    global_device.event_ring[0].control = 0;

    kprintf("4");

    global_device.cmd_index = 0;

    ctx->add_flags |= 1 << 2;
    ctx->slot_ctx[0] |= (2 << 27); // now two contexts (EP0 + EP1)

    kprintf("5");

    ctx->ep0_ctx[0] = (endpoint_address & 0x7F) << 16;
    ctx->ep0_ctx[1] = (max_packet_size << 16) | (3 << 3);
    ctx->ep0_ctx[2] = (uint64_t)(uintptr_t)intr_ring | 1;

    kprintf("6");


    kprintf("7");

    return true;
}

bool nec_key_ready() {
    return global_device.event_ring[0].control & global_device.cycle_bit;
}

int nec_read_key() {
    // global_device.event_ring[0].control &= ~global_device.cycle_bit;
    return global_device.key_buffer[2];
}

void test_keyboard_input() {
    kprintf("[NEC] Waiting for input");
    while (true) {
        if (nec_key_ready()) {
            int ch = nec_read_key();
            kprintf("Key: %c", ch);
        }
    }
}

