#include "power.h"
#include "mailbox/mailbox.h"
#include "console/kio.h"
#include "graph/graphics.h"  // For GPU functions
#include "types.h"          // For uint32_t, bool, etc.

// Forward declarations for graphics functions
void gpu_clear(uint32_t color);
void gpu_flush(void);
bool gpu_ready(void);

// ARM-specific instructions
#define WFI() asm volatile("wfi")
#define HVC() asm volatile("hvc #0")

// Semihosting operations
#define SYS_EXIT 0x18
#define ADP_STOPPED_APPLICATIONEXIT 0x20026
#define PSCI_SYSTEM_RESET 0x84000009

// Power management mailbox channels
#define MBOX_CH_PROP 8

// Power management tags
#define TAG_SET_POWER_STATE 0x00028001
#define TAG_GET_POWER_STATE 0x00020001

// Power device IDs
#define POWER_DEVICE_SD_CARD 0x00000000
#define POWER_DEVICE_UART0   0x00000001
#define POWER_DEVICE_UART1   0x00000002
#define POWER_DEVICE_USB_HCD 0x00000003
#define POWER_DEVICE_I2C0    0x00000004
#define POWER_DEVICE_I2C1    0x00000005
#define POWER_DEVICE_I2C2    0x00000006
#define POWER_DEVICE_SPI     0x00000007
#define POWER_DEVICE_CCP2TX  0x00000008

// Power state
#define POWER_STATE_OFF      0x00000000
#define POWER_STATE_ON       0x00000001
#define POWER_STATE_WAIT     0x00000002
#define POWER_STATE_NO_DEVICE 0x00000003

// Tag structure for power state
struct __attribute__((aligned(16))) power_state_tag {
    uint32_t tag_id;           // Tag identifier (TAG_SET_POWER_STATE or TAG_GET_POWER_STATE)
    uint32_t buffer_size;      // Size of request/response buffer (in bytes)
    uint32_t request_code;     // Request/response code
    
    // Device ID and state
    uint32_t device_id;
    uint32_t state;
    
    // End tag
    uint32_t end_tag;
};

// Mailbox message buffer for power management
struct __attribute__((aligned(16))) mbox_message {
    uint32_t size;                  // Buffer size in bytes
    uint32_t request_code;          // Request/response code (0 for request)
    
    // Tag for power state
    struct power_state_tag power_state;
    
    // End tag
    uint32_t end_tag;
};

// QEMU ARM poweroff via semihosting
// QEMU ARM poweroff via semihosting
static void qemu_poweroff() {
    // Clear screen if GPU is available
    if (gpu_ready()) {
        gpu_clear(0x00000000);  // Black color
        gpu_flush();
        for (volatile int i = 0; i < 500000; i++);
    }
    
    // Semihosting exit
    register uint64_t x0 __asm__("x0") = 0x18;  // SYS_EXIT
    register uint64_t x1 __asm__("x1") = 0x20026; // ADP_Stopped_ApplicationExit
    
    __asm__ volatile(
        "hlt #0xf000"
        : 
        : "r" (x0), "r" (x1)
        : "memory"
    );
}

void power_off() {
    // Clear screen if GPU is available
    if (gpu_ready()) {
        gpu_clear(0x00000000);  // Black color
        gpu_flush();
        for (volatile int i = 0; i < 500000; i++);
    }
    
    kprint("Shutting down the system...\n");
    
    // Try QEMU poweroff first (will only return if semihosting is not available)
    qemu_poweroff();
    
    // If we're still here, try standard poweroff methods
    
    // Mailbox poweroff (Raspberry Pi)
    volatile unsigned int* MAILBOX = (volatile unsigned int*)0x3F00B880;
    
    // Wait until mailbox is not full
    while (MAILBOX[6] & 0x80000000);
    
    // Send poweroff request
    MAILBOX[8] = 0x48000 | 0x10;  // Channel 8 (power management) with request
    
    // Wait for response
    while (1) {
        // Wait until there's mail
        while (MAILBOX[6] & 0x40000000);
        
        // Check if this is the response to our request
        if ((MAILBOX[0] & 0xF) == 0x8) {
            break;
        }
    }
    
    // If we're still here, try direct hardware power-off
    volatile unsigned int* PM_PWR = (volatile unsigned int*)0x3F10001C;
    unsigned int PM_PASSWORD = 0x5A000000;
    *PM_PWR = PM_PASSWORD | 0x20000;  // Power off
    
    // Final fallback - just hang
    while(1) {
        WFI();  // Wait for interrupt (low power)
    }
}

// System reset using PSCI (Power State Coordination Interface)
static void system_reset() {
    // PSCI SYSTEM_RESET function ID for ARM
    register uint64_t x0 __asm__("x0") = PSCI_SYSTEM_RESET;
    
    // Make the HVC (Hypervisor Call) to PSCI
    (void)x0; // Mark as used to suppress warning
    HVC();
    
    // If we get here, PSCI call failed
    kprint("PSCI reset failed, trying other methods...\n");
}

void reboot() {
    kprint("Rebooting the system...\n");
    
    // Clear screen if GPU is available
    if (gpu_ready()) {
        gpu_clear(0x00000000);  // Black color
        gpu_flush();
        for (volatile int i = 0; i < 500000; i++);
    }
    
    // Try PSCI reset first
    system_reset();
    
    // If we're still here, try QEMU semihosting exit
    register uint64_t x0 __asm__("x0") = 0x18;  // SYS_EXIT
    register uint64_t x1 __asm__("x1") = 0x20026; // ADP_Stopped_ApplicationExit
    __asm__ volatile("hlt #0xf000" : : "r"(x0), "r"(x1) : "memory");
    
    // If we're still here, try direct hardware reset
    volatile unsigned int* PM_RSTC = (volatile unsigned int*)0x3F10001C;
    volatile unsigned int* PM_WDOG = (volatile unsigned int*)0x3F100024;
    unsigned int PM_PASSWORD = 0x5A000000;
    
    // Set the watchdog to reset immediately
    *PM_RSTC = PM_PASSWORD | 0x20;  // Full reset
    *PM_WDOG = PM_PASSWORD | 0x8000; // Minimum count
    
    // Wait for the system to reset
    while(1) {
        asm volatile("wfi");  // Wait for interrupt (low power)
    }
}
