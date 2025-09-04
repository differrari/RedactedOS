#include "scheduler.h"
#include "console/kio.h"
#include "memory/talloc.h"
#include "memory/page_allocator.h"
#include "exceptions/irq.h"
#include "console/serial/uart.h"
#include "input/input_dispatch.h"
#include "exceptions/exception_handler.h"
#include "exceptions/timer.h"
#include "console/kconsole/kconsole.h"
#include "syscalls/syscalls.h"
#include "std/string.h"
#include "data_struct/linked_list.h"
#include "std/memory.h"
#include "math/math.h"

extern void save_context(process_t* proc);
extern void save_pc_interrupt(process_t* proc);
extern void restore_context(process_t* proc);

//TODO: use queues, eliminate the max procs limitation
process_t processes[MAX_PROCS];
uint16_t current_proc = 0;
uint16_t proc_count = 0;
uint16_t next_proc_index = 1;

typedef struct sleep_tracker {
    uint16_t pid;
    uint64_t timestamp;
    uint64_t sleep_time;
    bool valid;
} sleep_tracker;

sleep_tracker sleeping[MAX_PROCS];
uint16_t sleep_count;

clinkedlist_t *proc_opened_files;

typedef struct proc_open_file {
    uint64_t fid;
    size_t file_size;
    uintptr_t buffer;
    uint16_t pid;
} proc_open_file;

void* proc_page;

void save_context_registers(){
    save_context(&processes[current_proc]);
}

void save_return_address_interrupt(){
    save_pc_interrupt(&processes[current_proc]);
}

void switch_proc(ProcSwitchReason reason) {
    // kprintf("Stopping execution of process %i at %x",current_proc, processes[current_proc].spsr);
    if (proc_count == 0)
        panic_with_info("No processes active", 0);
    int next_proc = (current_proc + 1) % MAX_PROCS;
    while (processes[next_proc].state != READY) {
        next_proc = (next_proc + 1) % MAX_PROCS;
    }

    current_proc = next_proc;
    timer_reset();
    process_restore();
}

void save_syscall_return(uint64_t value){
    processes[current_proc].PROC_X0 = value;
}

void process_restore(){
    restore_context(&processes[current_proc]);
}

bool start_scheduler(){
    kprint("Starting scheduler");
    kconsole_clear();
    disable_interrupt();
    timer_init(1);
    switch_proc(YIELD);
    return true;
}

uintptr_t get_current_heap(){
    return processes[current_proc].heap;
}

bool get_current_privilege(){
    return (processes[current_proc].spsr & 0b1111) != 0;
}

process_t* get_current_proc(){
    return &processes[current_proc];
}

process_t* get_proc_by_pid(uint16_t pid){
    for (int i = 0; i < MAX_PROCS; i++)
        if (processes[i].id == pid)
            return &processes[i];
    return NULL;
}

uint16_t get_current_proc_pid(){
    return processes[current_proc].id;
}

void reset_process(process_t *proc){
    proc->sp = 0;
    pfree((void*)proc->stack-proc->stack_size,proc->stack_size);
    proc->pc = 0;
    proc->spsr = 0;
    proc->exit_code = 0;
    for (int j = 0; j < 31; j++)
        proc->regs[j] = 0;
    for (int k = 0; k < MAX_PROC_NAME_LENGTH; k++)
        proc->name[k] = 0;
    proc->input_buffer.read_index = 0;
    proc->input_buffer.write_index = 0;
    for (int k = 0; k < INPUT_BUFFER_CAPACITY; k++){
        proc->input_buffer.entries[k] = (keypress){0};
    }
    proc->packet_buffer.read_index = 0;
    proc->packet_buffer.write_index = 0;
    for (int k = 0; k < PACKET_BUFFER_CAPACITY; k++){
        sizedptr p = proc->packet_buffer.entries[k];
        if (p.ptr)
            free_sized(p);
        proc->packet_buffer.entries[k] = (sizedptr){0};
    }
}

void init_main_process(){
    proc_page = palloc(0x1000, MEM_PRIV_KERNEL, MEM_RW, false);
    process_t* proc = &processes[0];
    reset_process(proc);
    proc->id = next_proc_index++;
    proc->state = BLOCKED;
    proc->heap = (uintptr_t)palloc(0x1000, MEM_PRIV_KERNEL, MEM_RW, false);
    proc->stack_size = 0x1000;
    proc->stack = (uintptr_t)palloc(proc->stack_size,MEM_PRIV_KERNEL, MEM_RW,true);
    proc->sp = ksp;
    proc->output = (uintptr_t)palloc(0x1000, MEM_PRIV_KERNEL, MEM_RW, true);
    name_process(proc, "kernel");
    proc_count++;
}

process_t* init_process(){
    process_t* proc;
    if (next_proc_index >= MAX_PROCS){
        for (uint16_t i = 0; i < MAX_PROCS; i++){
            if (processes[i].state == STOPPED){
                proc = &processes[i];
                reset_process(proc);
                proc->state = READY;
                proc->id = next_proc_index++;
                proc_count++;
                return proc;
            }
        }
        panic_with_info("Out of process memory", 0);
    }

    proc = &processes[next_proc_index];
    reset_process(proc);
    proc->id = next_proc_index++;
    proc->state = READY;
    proc_count++;
    return proc;
}

void name_process(process_t *proc, const char *name){
    uint32_t len = 0;
    while (len < MAX_PROC_NAME_LENGTH && name[len] != '\0') len++;
    for (uint32_t i = 0; i < len; i++)
        proc->name[i] = name[i];
}

void stop_process(uint16_t pid, uint32_t exit_code){
    disable_interrupt();
    process_t *proc = get_proc_by_pid(pid);
    if (proc->state != READY) return;
    proc->state = STOPPED;
    proc->exit_code = exit_code;
    if (proc->focused)
        sys_unset_focus();
    reset_process(proc);
    proc_count--;
    // kprintf("Stopped %i process %i",pid,proc_count);
    switch_proc(HALT);
}

void stop_current_process(uint32_t exit_code){
    disable_interrupt();
    stop_process(processes[current_proc].id, exit_code);
}

uint16_t process_count(){
    return proc_count;
}

process_t *get_all_processes(){
    return processes;
}

void sleep_process(uint64_t msec){
    if (sleep_count < MAX_PROCS){
        processes[current_proc].state = BLOCKED;
        sleeping[sleep_count++] = (sleep_tracker){
            .pid = processes[current_proc].id,
            .timestamp = timer_now_msec(),
            .sleep_time = msec, 
            .valid = true
        };
    }
    if (virtual_timer_remaining_msec() > msec || virtual_timer_remaining_msec() == 0){
        virtual_timer_reset(msec);
        virtual_timer_enable();
    }
    switch_proc(YIELD);
}

void wake_processes(){
    uint64_t now = timer_now_msec();
    uint64_t next = UINT64_MAX;
    uint16_t w = 0;
    for(uint16_t i=0;i<sleep_count;i++){
        if(!sleeping[i].valid) continue;
        uint64_t wake = sleeping[i].timestamp + sleeping[i].sleep_time;
        if(wake <= now){
            process_t *p = get_proc_by_pid(sleeping[i].pid);
            if(p) p->state = READY;
        }else{
            if(wake < next) next = wake;
            sleeping[w++] = sleeping[i];
        }
    }
    sleep_count = w;

    if(next != UINT64_MAX){
        virtual_timer_reset(next - now);
        virtual_timer_enable();
    }
}

sizedptr list_processes(const char *path){
    size_t size = 0x1000;
    void *list_buffer = (char*)malloc(size);
    if (strlen(path, 100) == 0){
        uint32_t count = 0;
    
        char *write_ptr = (char*)list_buffer + 4;
        process_t *processes = get_all_processes();
        for (int i = 0; i < MAX_PROCS; i++){
            process_t *proc = &processes[i];
            if (proc->id != 0 && proc->state != STOPPED){
                count++;
                char* name = proc->name;
                while (*name) {
                    *write_ptr++ = *name;
                    name++;
                }
                *write_ptr++ = 0;
            }
        }
        *(uint32_t*)list_buffer = count;
    }
    //TODO:
    //else advance to / and get the pid
        //if that's it print that
        //else open the file (out, in, etc)
    return (sizedptr){(uintptr_t)list_buffer,size};
}

#define PROC_OUT_BUF 0x1000

FS_RESULT open_proc(const char *path, file *descriptor){
    const char *pid_s = seek_to(path, '/');
    path = seek_to(pid_s, '/');
    uint64_t pid = parse_int_u64(pid_s, path - pid_s);
    process_t *proc = get_proc_by_pid(pid);
    descriptor->id = reserve_fd_id();
    descriptor->size = PROC_OUT_BUF;
    descriptor->cursor = 0;
    if (!proc_opened_files) 
        proc_opened_files = kalloc(proc_page, sizeof(clinkedlist_t), ALIGN_64B, MEM_PRIV_KERNEL);
    proc_open_file *file = kalloc(proc_page, sizeof(proc_open_file), ALIGN_64B, MEM_PRIV_KERNEL);
    file->fid = descriptor->id;
    file->buffer = proc->output;
    file->pid = proc->id;
    file->file_size = descriptor->size;
    clinkedlist_push_front(proc_opened_files, (void*)file);
    return FS_RESULT_SUCCESS;
}


int find_open_proc_file(void *node, void* key){
    uint64_t *fid = (uint64_t*)key;
    proc_open_file *file = (proc_open_file*)node;
    if (file->fid == *fid) return 0;
    return -1;
}

size_t read_proc(file* fd, char *buf, size_t size, file_offset offset){
    if (!proc_opened_files){
        kprint("No files open");
        return 0;
    }
    clinkedlist_node_t *node = clinkedlist_find(proc_opened_files, (void*)&fd->id, find_open_proc_file);
    if (!node->data) return 0;
    proc_open_file *file = (proc_open_file*)node->data;
    size = min(size, file->file_size);
    memcpy(buf, (void*)(file->buffer + fd->cursor), size);
    return size;
}

size_t write_proc(file* fd, const char *buf, size_t size, file_offset offset){
    if (!proc_opened_files){
        kprint("No files open");
        return 0;
    }
    clinkedlist_node_t *node = clinkedlist_find(proc_opened_files, (void*)&fd->id, find_open_proc_file);
    if (!node->data) return 0;
    proc_open_file *file = (proc_open_file*)node->data;
    if (size >= PROC_OUT_BUF){
        kprint("Output buffer too large");
        return 0;
    }
    if (fd->cursor + size >= PROC_OUT_BUF){
        fd->cursor = 0;
        memset((void*)file->buffer, 0, file->file_size);
    }
    memcpy((void*)(file->buffer + fd->cursor), buf, size);
    fd->cursor += size;
    kprintf("Wrote %i bytes into process %i's output buffer", size, file->pid);
    return size;
}

driver_module scheduler_module = (driver_module){
    .name = "scheduler",
    .mount = "/proc",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = start_scheduler,
    .fini = 0,
    .open = open_proc,
    .read = read_proc,
    .write = write_proc,
    .seek = 0,
    .readdir = list_processes,
};