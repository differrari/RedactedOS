#include "signals.h"
#include "process/process.h"
#include "process/scheduler.h"
#include "process/debug.h"
#include "console/kio.h"
#include "permissions/authorize.h"

#define SIGNAL_DEBUG
#ifdef SIGNAL_DEBUG
#define sig_print(...) print(__VA_ARGS__)
#else
#define sig_print(...)
#endif

bool register_signal_handler(process_t *proc, signal_types type, signal_handler handler){
    if (proc->signal_handlers[type].pc){
        kprint("[SIGNAL error] already exists");
        return false;  
    } 
    if (!can_signal_be_handled(type)){
        kprint("[SIGNAL error] cannot be handled");
        return false;  
    } 
    sig_print("[SIGNAL debug] handler added");
    new_thread(proc, &proc->signal_handlers[type], proc->main_thread.spsr, (uptr)handler);
    return true;
}

bool send_signal_proc_proc(signal_types type, i64 value, process_t *source, process_t *destination){
    if (!source || !destination || !type) return false;

    if (!source->permissions.auth_id || !auth_valid(source->permissions.auth_id, auth_process_send_signals, auth_map_process(destination))) return false;
    
    if (signal_is_immediate(type)){
        handle_signal_default(destination, &(signal_info_t){
            .sender = source->id,
            .type = type,
            .value = value,
        });
        sig_print("[SIGNAL] sent immediate signal by %i to %i",source->id, destination->id);
        return true;
    }

    thread_t *t = &destination->signal_handlers[type];
    if (t->pc)
        schedule_thread(destination,t);
    else 
        handle_signal_default(destination, &(signal_info_t){
            .sender = source->id,
            .type = type,
            .value = value,
        });

    switch_proc(YIELD);
    sig_print("[SIGNAL] sent signal by %i to %i",source->id, destination->id);
    return true;
}

bool handle_signal_default(process_t *proc, signal_info_t *info){
    if (!info || !info->type) return false;
    switch (info->type) {
        case SIG_KILL:
        case SIG_QUIT:
            stop_process(proc->id, -SIG_KILL);
            return true;
        case SIG_STOP:
            sig_print("[SIGNAL debug] Stop %s",proc->name);
            block_process(proc);
            debug_snapshot(proc, &proc->main_thread);
            return true;
        case SIG_CONT:
            sig_print("[SIGNAL debug] Ready proc %s",proc->name);
            resume_blocked_process(proc);
            return true;
        default: return false;
    }
}