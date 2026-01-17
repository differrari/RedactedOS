#include "scheduler.h"
#include "console/kio.h"
#include "memory/page_allocator.h"
#include "exceptions/irq.h"
#include "input/input_dispatch.h"
#include "exceptions/exception_handler.h"
#include "exceptions/timer.h"
#include "console/kconsole/kconsole.h"
#include "std/string.h"
#include "data_struct/hashmap.h"
#include "std/memory.h"
#include "math/math.h"
#include "memory/mmu.h"
#include "process/syscall.h"
#include "sysregs.h"
#include "filesystem/filesystem.h"

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

chashmap_t *proc_opened_files;

void* proc_page;

void save_return_address_interrupt(){
    save_pc_interrupt(cpec);
}

extern void mmu_swap();

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
    mmu_swap_ttbr(processes[current_proc].ttbr);
    process_restore();
}

void save_syscall_return(uint64_t value){
    processes[current_proc].PROC_X0 = value;
}

void process_restore(){
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

void* list_alloc(size_t size){
    return kalloc(proc_page, size, ALIGN_64B, MEM_PRIV_KERNEL);
}

bool init_scheduler_module(){
    if (!proc_opened_files) {
        proc_opened_files = chashmap_create(64);
        proc_opened_files->free = kfree;
        proc_opened_files->alloc = list_alloc;
    }
    return start_scheduler();
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
        if (proc->stack_phys) pfree((void*)proc->stack_phys-proc->stack_size,proc->stack_size);
    if (proc->heap_phys) free_managed_page((void*)proc->heap_phys);
    proc->pc = 0;
    proc->spsr = 0;
    proc->exit_code = 0;
    if (proc->code && proc->code_size){
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
            free_sizedptr(p);
        proc->packet_buffer.entries[k] = (sizedptr){0};
    }
    close_files_for_process(proc->id);
    if (proc->ttbr) {
        if (pttbr == proc->ttbr) panic("Trying to free process while mapped", (uintptr_t)proc->ttbr);
        mmu_free_ttbr(proc->ttbr);
    }
}

void init_main_process(){
    proc_page = palloc(0x1000, MEM_PRIV_KERNEL, MEM_RW, false);
    process_t* proc = &processes[0];
    cpec = (uintptr_t)&processes[0];
    proc->id = next_proc_index++;
    proc->state = BLOCKED;
    proc->heap = (uintptr_t)palloc(0x1000, MEM_PRIV_KERNEL, MEM_RW, false);
    proc->stack_size = 0x10000;
    proc->stack = (uintptr_t)palloc(proc->stack_size,MEM_PRIV_KERNEL, MEM_RW,true);
    proc->sp = ksp;
    proc->output = (uintptr_t)palloc(PROC_OUT_BUF, MEM_PRIV_KERNEL, MEM_RW, true);
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

void stop_process(uint16_t pid, int32_t exit_code){
    disable_interrupt();
    process_t *proc = get_proc_by_pid(pid);
    if (proc->state != READY) return;
    proc->state = STOPPED;
    proc->exit_code = exit_code;
    if (proc->focused)
        sys_unset_focus();
    if (proc->ttbr) {
        mmu_swap_ttbr(0);
        mmu_swap();
    }
    reset_process(proc);
    proc_count--;
    // kprintf("Stopped %i process %i",pid,proc_count);
    switch_proc(HALT);
}

void stop_current_process(int32_t exit_code){
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

size_t list_processes(const char *path, void *buf, size_t size, file_offset *offset){
    
   	int proc_index = 0;
    if (*offset){
        for (int i = 0; i < MAX_PROCS; i++){
            if (processes[i].id == *offset){
                proc_index = i+1;
                break;
            }
        }
    }

	uint32_t count = 0;
	
    char *write_ptr = (char*)buf + 4;
    for (; proc_index < MAX_PROCS; proc_index++){
    	if (size - (uintptr_t)write_ptr - (uintptr_t)buf - 4 < MAX_PROC_NAME_LENGTH) break;
   		process_t *proc = &processes[proc_index];
        if (proc->id != 0 && proc->state != STOPPED){
            count++;
            char* name = proc->name;
            while (*name) *write_ptr++ = *name++;
            *write_ptr++ = 0;
            *offset = proc->id;
        }
    }

    *(uint32_t*)buf = count;

    //TODO: allow seeing files belonging to a proc (/out, /in, etc)
    return size;
}

FS_RESULT open_proc(const char *path, file *descriptor){
    uint64_t fid = reserve_fd_gid(path);
    module_file *mfile = (module_file*)chashmap_get(proc_opened_files, &fid, sizeof(uint64_t));
    if (mfile){
        descriptor->id = mfile->fid;
        descriptor->size = mfile->file_size;
        mfile->references++;
        return FS_RESULT_SUCCESS;
    }
    const char *pid_s = seek_to(path, '/');
    path = seek_to(pid_s, '/');
    uint64_t pid = parse_int_u64(pid_s, path - pid_s);
    process_t *proc = get_proc_by_pid(pid);
    descriptor->id = fid;
    descriptor->cursor = 0;
    module_file *file = kalloc(proc_page, sizeof(module_file), ALIGN_64B, MEM_PRIV_KERNEL);
    file->fid = fid;
    if (strcmp_case(path, "out",true) == 0){
        descriptor->size = proc->output_size;
        file->buffer = proc->output;
    } else if (strcmp_case(path, "state",true) == 0){
        descriptor->size = sizeof(int);
        file->buffer = PHYS_TO_VIRT((uintptr_t)&proc->state);
        file->ignore_cursor = true;
        file->read_only = true;
    } else return FS_RESULT_NOTFOUND;
    file->file_size = descriptor->size;
    return chashmap_put(proc_opened_files, &descriptor->id, sizeof(uint64_t), file) >= 0 ? FS_RESULT_SUCCESS : FS_RESULT_DRIVER_ERROR;
}

int find_open_proc_file(void *node, void* key){
    uint64_t *fid = (uint64_t*)key;
    module_file *file = (module_file*)node;
    if (file->fid == *fid) return 0;
    return -1;
}

int find_open_proc_file_buffer(void *node, void* key){
    uint64_t *buf = (uint64_t*)key;
    module_file *file = (module_file*)node;
    if (file->buffer == *buf) return 0;
    return -1;
}

size_t read_proc(file* fd, char *buf, size_t size, file_offset offset){
    if (!proc_opened_files){
        kprint("No files open");
        return 0;
    }
    module_file *file = (module_file*)chashmap_get(proc_opened_files, &fd->id, sizeof(uint64_t));
    if (!file) return 0;
    uint64_t cursor = file->ignore_cursor ? 0 : fd->cursor;
    size = min(size, file->file_size - cursor);
    memcpy(buf, (void*)(file->buffer + cursor), size);
    return size;
}

size_t write_proc(file* fd, const char *buf, size_t size, file_offset offset){
    if (fd->id == FD_OUT){
        string fullpath = string_format("/%i/out",get_current_proc_pid());
        open_proc(fullpath.data, fd);
        string_free(fullpath);
    }
    if (!proc_opened_files){
        kprint("No files open");
        return 0;
    }
    uintptr_t pbuf;
    module_file *file = (module_file*)chashmap_get(proc_opened_files, &fd->id, sizeof(uint64_t));
    if (file->read_only) return 0;
    bool is_output = file->buffer == get_current_proc()->output;
    pbuf = file->buffer;

    if (is_output){//TODO: probably better to make these files be held by this module, and created only when needed
        size = min(size+1, PROC_OUT_BUF);
        
        fd->cursor = file->file_size;
        
        if (fd->cursor + size >= PROC_OUT_BUF){
            fd->cursor = 0;
            memset((void*)pbuf, 0, PROC_OUT_BUF);
        }

        memcpy((void*)(pbuf + fd->cursor), buf, size ? size-1 : 0);
        *(char*)(pbuf + fd->cursor + size-1) = '\n';
        fd->cursor += size;

        file->file_size += size;
        get_current_proc()->output_size += size;
    }
    return size;
}

system_module scheduler_module = (system_module){
    .name = "scheduler",
    .mount = "/proc",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = init_scheduler_module,
    .fini = 0,
    .open = open_proc,
    .read = read_proc,
    .write = write_proc,
    .close = 0,
    .sread = 0,
    .swrite = 0,//TODO implement simple io
    .readdir = list_processes,
};
