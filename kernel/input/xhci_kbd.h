#include "xhci.h"
#include "xhci_types.h"

typedef struct {
    uint8_t modifier;
    uint8_t rsvd;
    char keys[6];
} keypress;

void set_default_kbd(xhci_usb_device* kbd);
void xhci_kbd_request_data();
keypress xhci_read_key();