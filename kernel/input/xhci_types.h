#pragma once

#ifdef __cplusplus
extern "C" {
#endif 

#include "types.h"

#define TRB_TYPE_NORMAL      1
#define TRB_TYPE_LINK        6
#define TRB_TYPE_EVENT_DATA  3
#define TRB_TYPE_ENABLE_SLOT 9
#define TRB_TYPE_ADDRESS_DEV 11
#define TRB_TYPE_CONFIG_EP   12
#define TRB_TYPE_SETUP       2
#define TRB_TYPE_STATUS      4
#define TRB_TYPE_INPUT       8

#define MAX_TRB_AMOUNT 256
#define MAX_ERST_AMOUNT 1

#define TRB_TYPE_MASK 0xFC00
#define TRB_ENDPOINT_MASK 0xF0000
#define TRB_SLOT_MASK 0xF000000

#define TRB_TYPE_TRANSFER           0x20
#define TRB_TYPE_COMMAND_COMPLETION 0x21
#define TRB_TYPE_PORT_STATUS_CHANGE 0x22

#define TRB_TYPE_SETUP_STAGE 0x2
#define TRB_TYPE_DATA_STAGE 0x3
#define TRB_TYPE_STATUS_STAGE 0x4

#define XHCI_USBSTS_HSE (1 << 2)
#define XHCI_USBSTS_CE  (1 << 12)

#define XHCI_IRQ 31

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
} portstatuscontrol;

typedef struct {
    portstatuscontrol portsc;
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
    struct {
        endpoint_field0 endpoint_f0;
        endpoint_field1 endpoint_f1;
        endpoint_field23 endpoint_f23;
        endpoint_field4 endpoint_f4;
        uint32_t ep_rsvd[3];
    } endpoints[31];
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
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
    uint8_t data[255];
} usb_configuration_descriptor;

typedef struct __attribute__((packed)) {
    usb_descriptor_header header;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} usb_interface_descriptor;

typedef struct __attribute__((packed)) {
    usb_descriptor_header header;
    uint16_t bcdHID;
    uint8_t  bCountryCode;
    uint8_t  bNumDescriptors;
    struct {
        uint8_t  bDescriptorType;
        uint8_t wDescriptorLength;
    }__attribute__((packed)) descriptors[1];
#warning wDescriptorLength is supposed to be 16, but for some reason the descriptor from usb-kbd is 8. Will need to fix this once we do a real device
} usb_hid_descriptor;

typedef struct __attribute__((packed)) {
    usb_descriptor_header header;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint8_t wMaxPacketSize;
    uint8_t bInterval;
#warning wMaxPacketSize is supposed to be 16, but for some reason the descriptor from usb-kbd is 8. Will need to fix this once we do a real device
} usb_endpoint_descriptor;

typedef struct __attribute__((packed)) {
    usb_descriptor_header header;
    uint16_t lang_ids[126];
} usb_string_language_descriptor;

typedef struct __attribute__((packed)){
    usb_descriptor_header header;
    uint16_t unicode_string[126];
} usb_string_descriptor;

typedef enum {
    NONE,
    KEYBOARD,
    MOUSE
} xhci_device_types;

typedef struct {
    uint8_t transfer_cycle_bit;
    uint32_t transfer_index;
    trb* transfer_ring;
    uint32_t slot_id;
    uint8_t interface_protocol;//TODO: support multiple
    xhci_input_context* ctx;
} xhci_usb_device;

typedef struct {
    xhci_device_types type;
    trb* endpoint_transfer_ring;
    uint32_t endpoint_transfer_index;
    uint8_t endpoint_transfer_cycle_bit;
    uint8_t poll_packetSize;
    uint8_t *input_buffer;
    uint8_t poll_endpoint;
    uint16_t report_length;
    uint8_t *report_descriptor;
 } xhci_usb_device_endpoint;

#define USB_DEVICE_DESCRIPTOR 1
#define USB_CONFIGURATION_DESCRIPTOR 2
#define USB_STRING_DESCRIPTOR 3

#ifdef __cplusplus
}
#endif 