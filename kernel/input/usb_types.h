#pragma once

#ifdef __cplusplus
extern "C" {
#endif 

#include "types.h"

#define INPUT_IRQ 36

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
//TODO: wDescriptorLength is supposed to be 16, but for some reason the descriptor from usb-kbd is 8. Will need to fix this once we do a real device
} usb_hid_descriptor;

typedef struct __attribute__((packed)) {
    usb_descriptor_header header;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint8_t wMaxPacketSize;
    uint8_t bInterval;
//TODO: wMaxPacketSize is supposed to be 16, but for some reason the descriptor from usb-kbd is 8. Will need to fix this once we do a real device
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
    UNKNOWN,
    KEYBOARD,
    MOUSE
} usb_device_types;

#define USB_DEVICE_DESCRIPTOR 1
#define USB_CONFIGURATION_DESCRIPTOR 2
#define USB_STRING_DESCRIPTOR 3

#ifdef __cplusplus
}
#endif 