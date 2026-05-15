#pragma once

#include "signals/signal_types.h"

typedef struct process process_t;
extern process_t* get_proc_by_pid(uint16_t pid);

bool register_signal_handler(process_t* proc, signal_types type, signal_handler handler);

bool send_signal_proc_proc(signal_types type, i64 value, process_t *source, process_t *destination);

bool handle_signal_default(process_t *proc, signal_info_t *info);

static inline bool send_signal_proc_id(signal_types type, i64 value, process_t *source, u16 destination){
    return send_signal_proc_proc(type, value, source, get_proc_by_pid(destination));
}

static inline bool send_signal_id_proc(signal_types type, i64 value, u16 source, process_t *destination){
    return send_signal_proc_proc(type, value, get_proc_by_pid(source), destination);
}

static inline bool send_signal_id_id(signal_types type, i64 value,u16 source, u16 destination){
    return send_signal_proc_proc(type, value, get_proc_by_pid(source), get_proc_by_pid(destination));
}

