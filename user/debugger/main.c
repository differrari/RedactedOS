#include "syscalls/syscalls.h"
#include "input_keycodes.h"
#include "memory/memory.h"

// - [x] control the program's execution
// - [x] read the log
// - [-] trace
// - [] step
// - [] exception handling
// - [] inspect memory

// - [] breakpoints
// - [] watchpoints
// 
typedef enum { PROC_STOPPED, PROC_READY, PROC_RUNNING, PROC_BLOCKED, PROC_SLEEPING } process_state;

buffer proc_out_buf;

int main(int argc, char* argv[]){
    
    if (argc < 2) return -1;
    
    char *proc_id_str = argv[1];
    
    u16 proc_id = parse_int_u64(proc_id_str, strlen(proc_id_str));
    
    string proc_out_s = string_format("/proc/%i/out", proc_id);
    string proc_state_s = string_format("/proc/%i/state", proc_id);
    i32 proc_state = 0;
    sreadf(proc_state_s.data, &proc_state, sizeof(proc_state));
    
    file proc_out_fd = {};
    openf(proc_out_s.data, &proc_out_fd);
    
    proc_out_buf = buffer_create(0x1000, buffer_can_grow);

    if (!proc_state){
        return 0;
    }
    
    draw_ctx ctx = {
        // .width = 300,
        // .height = 170
    };

    request_draw_ctx(&ctx);

    bool running = true;//TODO: tie this with proc_state
    
    // char *prog_buf = zalloc(256);
    // const char *bundle = "com.idsoftware.doom";
    // memcpy(prog_buf, bundle, strlen(bundle));

    // transformf("/apps/resolve", prog_buf, 256);

    // exec(prog_buf, 0, 0, EXEC_MODE_DEFAULT);

    // print("App location: %s",prog_buf);
    while (true){
        fb_clear(&ctx,0xff123456);

        fb_fill_rect(&ctx, 10, 10, 30, 30, proc_state == PROC_READY || proc_state == PROC_RUNNING ? 0xFFcc0000 : 0xFF00cc00);

        char buf[256];
        size_t n = readf(&proc_out_fd, buf, 256);

        if (n){
            buffer_write_lim(&proc_out_buf, buf, n);
            
            fb_draw_slice(&ctx, (string_slice){buf, n}, 10, 50, 2, 0xFFcccccc);
        }

        sreadf(proc_state_s.data, &proc_state, sizeof(proc_state));
        if (!proc_state){
            fb_clear(&ctx,0xff123456);
            fb_draw_slice(&ctx, SLICE("Program exited. Goodbye"), 10, 50, 2, 0xFFcccccc);
            commit_draw_ctx(&ctx);
            msleep(3000);
            return 0;
        }

        commit_draw_ctx(&ctx);

        kbd_event ev = {};
        if (read_event(&ev)){
            if (ev.key == KEY_ESC) halt(0);
            if (ev.key == KEY_F1 && ev.type == KEY_PRESS){
                if (running){
                    send_signal(SIG_STOP, proc_id);
                } else {
                    send_signal(SIG_CONT, proc_id);
                }
                running = !running;
            }
        }
    }
}