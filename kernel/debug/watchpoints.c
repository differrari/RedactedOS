#include "watchpoints.h"
#include "syscalls/syscalls.h"

#define MAX_WATCHPOINTS 4

uptr watchpoints[MAX_WATCHPOINTS] = {};
size_t watchpoint_count = 0;

#define STORE_VAL(i) case i: asm volatile("msr DBGWVR"#i"_EL1, %0" : "=r"(address)); break;
#define STORE_CONFIG(i) case i: asm volatile("msr DBGWCR"#i"_EL1, %0" : "=r"(config)); break;

typedef union {
    struct {
        u32 enabled: 1;
        u32 priv_access_control: 2;
        u32 load_store: 2;
        u32 byte_address_select: 8;
        u32 higher_mode_control: 1;
        u32 security_state_control: 1;
        u32 linked_breakpoint_number: 4;
        u32 watchpoint_type: 2;//linked or unlinked. TODO: add linked once breakpoints exist
        u32 rsvd: 3;
        u32 mask: 5;
        u32 rsvd2: 3;
    };
    u32 entry;
}
watchpoint_entry_t;

void debug_store_watch_value(u64 index, uptr address){
    switch (index) {
        STORE_VAL(0)
        STORE_VAL(1)
        STORE_VAL(2)
        STORE_VAL(3)
    }
}

bool debug_store_watch_config(u64 index, debug_watchpoint_type type, u8 bit_mask, bool active){
    if (active && !(type & watchpoint_type_all)) return false;
    watchpoint_entry_t entry = {
        .enabled = active & 1,
        .priv_access_control = active ? 0b11 : 0,//01 = EL1, 10 = EL0, 11 = both
        .load_store = type,
        .byte_address_select = bit_mask,
        .higher_mode_control = 0,
        .security_state_control = 0,
        .linked_breakpoint_number = 0,
        .watchpoint_type = 0,
        .mask = 0,
    };
    u32 config = entry.entry; 
    print("Config %b",config);
    switch (index) {
        STORE_CONFIG(0)
        STORE_CONFIG(1)
        STORE_CONFIG(2)
        STORE_CONFIG(3)
    }
    return true;
}

int debug_watch(uptr address, debug_watchpoint_type type, u8 bit_mask){
    asm volatile ("msr daifclr, #8");
    u64 mdscr = 0;
    asm volatile (
        "msr oslar_el1, xzr\n"
        "mrs %0, mdscr_el1\n"
        "orr %0, %0, #(1 << 15)\n"
        "msr mdscr_el1, %0\n" : "=r"(mdscr)
    );
    if (watchpoint_count + 1 >= MAX_WATCHPOINTS) return -1;
    for (u64 i = 0; i < MAX_WATCHPOINTS; i++){
        if (!watchpoints[i]){
            watchpoints[i] = address;
            debug_store_watch_value(i, address);
            if (!debug_store_watch_config(i, type, bit_mask, true)) return -1;
            asm volatile("isb" ::: "memory");
            watchpoint_count++;
            print("[DEBUG] Set debug watchpoint at %llx",address);
            return i;
        }
    }
    return -1;
}

bool debug_unwatch(int index){
    if (!watchpoint_count) return false;
    //TODO actually (un)configure registers
    if (!watchpoints[index]) return false;
    watchpoint_count--;
    print("[DEBUG] Unset debug watchpoint for %llx",watchpoints[index]);
    watchpoints[index] = 0;
    debug_store_watch_config(index, 0, 0, false);
    asm volatile("isb" ::: "memory");
    return true;
}