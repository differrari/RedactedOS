#include "scheduler.h"
#include "console/kio.h"
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
#include "memory/mmu.h"
#include "process/syscall.h"

extern void save_pc_interrupt(uintptr_t ptr);
extern void restore_context(uintptr_t ptr);

bool allow_va = true;
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
    bool ignore_cursor;
    bool read_only;
} proc_open_file;

void* proc_page;

void save_return_address_interrupt(){
    save_pc_interrupt(cpec);
}

void switch_proc(ProcSwitchReason reason) {
    // kprintf("Stopping execution of process %i at %x",current_proc, processes[current_proc].spsr);
    if (proc_count == 0)
        panic("No processes active", 0);
    int next_proc = (current_proc + 1) % MAX_PROCS;
    while (processes[next_proc].state != READY) {
        next_proc = (next_proc + 1) % MAX_PROCS;
    }

    current_proc = next_proc;
    cpec = (uintptr_t)&processes[current_proc];
    timer_reset(processes[current_proc].priority);
    process_restore();
}

void save_syscall_return(uint64_t value){
    processes[current_proc].PROC_X0 = value;
}

void process_restore(){
    syscall_depth--;
    restore_context(cpec);
}

bool start_scheduler(){
    kprint("Starting scheduler");
    kconsole_clear();
    disable_interrupt();
    timer_init(processes[current_proc].priority);
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
    bool just_finished = processes[current_proc].id == proc->id;
    proc->sp = 0;
    if (!just_finished || !(processes[current_proc].PROC_PRIV))//Privileged processes use their own stack even in an exception. We'll free it when we reuse it
        if (proc->stack) pfree((void*)proc->stack-proc->stack_size,proc->stack_size);
    if (proc->heap) free_managed_page((void*)proc->heap);//Sadly, full pages of alloc'd memory are not kept track and will not be freed
    proc->pc = 0;
    proc->spsr = 0;
    proc->exit_code = 0;
    if (proc->code && proc->code_size){
        if (proc->use_va){
            for (uintptr_t i = 0; i < proc->code_size; i += PAGE_SIZE){
                mmu_unmap(proc->va + i, (uintptr_t)proc->code + i);
            }
            allow_va = true;
        } 
        pfree(proc->code, proc->code_size);
    }
    if (!just_finished && proc->output)
        kfree((void*)proc->output, PROC_OUT_BUF);
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
    cpec = (uintptr_t)&processes[0];
    proc->id = next_proc_index++;
    proc->state = BLOCKED;
    proc->heap = (uintptr_t)palloc(0x1000, MEM_PRIV_KERNEL, MEM_RW, false);
    proc->stack_size = 0x1000;
    proc->stack = (uintptr_t)palloc(proc->stack_size,MEM_PRIV_KERNEL, MEM_RW,true);
    proc->sp = ksp;
    proc->output = (uintptr_t)palloc(0x1000, MEM_PRIV_KERNEL, MEM_RW, true);
    proc->priority = PROC_PRIORITY_LOW;
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
        panic("Out of process memory", 0);
    }

    proc = &processes[next_proc_index];
    reset_process(proc);
    proc->id = next_proc_index++;
    proc->state = READY;
    proc->priority = PROC_PRIORITY_LOW;
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
    if (!msec) switch_proc(YIELD);
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

size_t list_processes(const char *path, void *buf, size_t size, file_offset offset){
    if (strlen(path, 100) == 0){
        uint32_t count = 0;
    
        char *write_ptr = (char*)buf + 4;
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
        *(uint32_t*)buf = count;
    }
    //TODO:
    //else advance to / and get the pid
        //if that's it print that
        //else open the file (out, in, etc)
    return size;
}

void* list_alloc(size_t size){
    return kalloc(proc_page, size, ALIGN_64B, MEM_PRIV_KERNEL);
}

FS_RESULT open_proc(const char *path, file *descriptor){
    const char *pid_s = seek_to(path, '/');
    path = seek_to(pid_s, '/');
    uint64_t pid = parse_int_u64(pid_s, path - pid_s);
    process_t *proc = get_proc_by_pid(pid);
    descriptor->id = reserve_fd_id();
    descriptor->cursor = 0;
    if (!proc_opened_files) {
        proc_opened_files = kalloc(proc_page, sizeof(clinkedlist_t), ALIGN_64B, MEM_PRIV_KERNEL);
        proc_opened_files->free = kfree;
        proc_opened_files->alloc = list_alloc;
    }
    proc_open_file *file = kalloc(proc_page, sizeof(proc_open_file), ALIGN_64B, MEM_PRIV_KERNEL);
    file->fid = descriptor->id;
    file->pid = proc->id;
    
    if (strcmp(path, "out", true) == 0){
        descriptor->size = 0;//TODO: sizeof buffer, could already have data
        file->buffer = proc->output;
    } else if (strcmp(path, "state", true) == 0){
        descriptor->size = sizeof(int);
        file->buffer = (uintptr_t)&proc->state;
        file->ignore_cursor = true;
        file->read_only = true;
    } else return FS_RESULT_NOTFOUND;
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

int find_open_proc_file_buffer(void *node, void* key){
    uint64_t *buf = (uint64_t*)key;
    proc_open_file *file = (proc_open_file*)node;
    if (file->buffer == *buf) return 0;
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
    if (!file) return 0;
    uint64_t cursor = file->ignore_cursor ? 0 : fd->cursor;
    size = min(size, file->file_size - cursor);
    memcpy(buf, (void*)(file->buffer + cursor), size);
    return size;
}

size_t write_proc(file* fd, const char *buf, size_t size, file_offset offset){
    if (!proc_opened_files && fd->id != FD_OUT){
        kprint("No files open");
        return 0;
    }
    uintptr_t pbuf;
    clinkedlist_node_t *node;
    if (fd->id == FD_OUT){
        process_t *proc = get_current_proc();
        pbuf = proc->output;
    } else {
        node = clinkedlist_find(proc_opened_files, (void*)&fd->id, find_open_proc_file);
        if (!node->data) return 0;
        proc_open_file *file = (proc_open_file*)node->data;
        if (file->read_only) return 0;
        pbuf = file->buffer;
    }
    
    if (size >= PROC_OUT_BUF){
        kprint("Output too large");
        return 0;
    }
    if (fd->cursor + size >= PROC_OUT_BUF){
        fd->cursor = 0;
        memset((void*)pbuf, 0, PROC_OUT_BUF);
    }
    memcpy((void*)(pbuf + fd->cursor), buf, size);
    fd->cursor += size;
    //TODO: Need a better way to handle opening a file multiple times
    for (clinkedlist_node_t *start = proc_opened_files->head; start != proc_opened_files->tail; start = start->next){
        if (start != node){
            proc_open_file *n_file = (proc_open_file*)start->data;
            if (n_file && n_file->buffer == pbuf){
                n_file->file_size += size;
            } 
        }
    }
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
    .sread = 0,
    .swrite = 0,//TODO implement simple io
    .readdir = list_processes,
};