#include "signals.h"
#include "process/process.h"
#include "process/scheduler.h"

#include "console/kio.h"

bool register_signal_handler(process_t *proc, signal_types type, signal_handler handler){
    if (proc->signal_handlers[type].pc){
        kprint("Signal already exists");
        return false;  
    } 
    if (!can_signal_be_handled(type)){
        kprint("Signal cannot be handled");
        return false;  
    } 
    kprint("Signal handler added");
    proc->signal_handlers[type] = new_thread(proc, (uptr)handler);
    return true;
}

bool send_signal_proc_proc(signal_types type, i64 value, process_t *source, process_t *destination){
    if (!source || !destination || !type) return false;

    if (signal_is_immediate(type)){
        handle_signal_default(destination, &(signal_info_t){
            .sender = source->id,
            .type = type,
            .value = value,
        });
        return true;
    }

    thread_t *t = zalloc(sizeof(thread_t));
    *t = new_thread(destination, (uptr)destination->signal_handlers[type].pc);//&destination->signal_handlers[type];
    if (t->pc)
        run_thread_oneshot(t, destination);

    switch_proc(YIELD);
    
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
            kprintf("Stop %s",proc->name);
            block_process(proc);
            return true;
        case SIG_CONT:
            kprintf("Ready proc %s",proc->name);
            resume_blocked_process(proc);
            return true;
        default: return false;
    }
}