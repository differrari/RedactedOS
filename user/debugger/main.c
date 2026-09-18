#include "syscalls/syscalls.h"
#include "input_keycodes.h"
#include "memory/memory.h"
#include "uno/uno.h"

// - [x] control the program's execution
// - [] get proc's information /proc/id/info
// - [x] read the log
// - [-] trace
// - [] step
// - [] exception handling
// - [] inspect memory /proc/id/mem/addr <- read
// - [] modify memory /proc/id/mem/addr <- write

// - [] breakpoints
// - [] watchpoints
// 
typedef enum { PROC_STOPPED, PROC_READY, PROC_RUNNING, PROC_BLOCKED, PROC_SLEEPING } process_state;

buffer proc_out_buf;

enum {
    button_none,
    button_pauseplay,
};

void toggle_proc(int tag, gpu_point loc){
    print("Pressed");
}

i32 proc_state = 0;
u16 proc_id = 0;

button_info pp_button = {.press = toggle_proc };

u32 theme[2];

gpu_point log_scroll = {.x = -30};

text_field_info log_text_info = {
    .content = &proc_out_buf,
};

void view_builder(){
    VERTICAL(((node_info){.bg_color = theme[0], .sizing_rule = size_fill}), {
        HORIZONTAL((node_info){},{
            uno_button(button_pauseplay, (node_info){.bg_color = proc_state == PROC_READY || proc_state == PROC_RUNNING ? 0xFFcc0000 : 0xFF00cc00, .padding = 5}, &pp_button, SLICE(proc_state == PROC_READY || proc_state == PROC_RUNNING ? "|" : ">"));
            uno_label((node_info){.fg_color = theme[1], .padding = 5}, doc_text_body, SLICE("Process name goes here")); 
        });
        uno_text_field(2, (node_info){ .sizing_rule = size_fill, .fg_color = theme[1], .offset = &log_scroll, .type = doc_text_body }, &log_text_info);
    });
}

int main(int argc, char* argv[]){
    
    if (argc < 2) return -1;
    
    char *proc_id_str = argv[1];
    
    proc_id = parse_int_u64(proc_id_str, strlen(proc_id_str));
    
    sreadf("/theme", &theme, sizeof(theme));
    
    thread_inspect(TINSPECT_CONTROL | TINSPECT_TRACE | TINSPECT_INFO | TINSPECT_STATE, proc_id, 1);
    
    string proc_out_s = string_format("/proc/%i/out", proc_id);
    string proc_state_s = string_format("/proc/%i/state", proc_id);
    sreadf(proc_state_s.data, &proc_state, sizeof(proc_state));
    
    file proc_out_fd = {};
    openf(proc_out_s.data, &proc_out_fd);
    
    proc_out_buf = buffer_create(0x1000, buffer_can_grow);

    if (!proc_state){
        return 0;
    }
    
    draw_ctx ctx = {};

    request_draw_ctx(&ctx);
    
    uno_set_document_view(view_builder, draw_ctx_rect(&ctx));

    bool running = true;//TODO: tie this with proc_state
    
    // char *prog_buf = zalloc(256);
    // const char *bundle = "com.idsoftware.doom";
    // memcpy(prog_buf, bundle, strlen(bundle));

    // transformf("/apps/resolve", prog_buf, 256);

    // exec(prog_buf, 0, 0, EXEC_MODE_DEFAULT);

    // print("App location: %s",prog_buf);

    while (true){
        
        uno_draw(&ctx);

        char buf[256];
        size_t n = readf(&proc_out_fd, buf, 256);

        if (n){
            buffer_write_lim(&proc_out_buf, buf, n);
            // for (u64 i = 0; i < n; i++)
            //     if (buf[i] == '\n') 
            log_scroll.y += fb_line_height(3);//TODO: this is too verbose
            uno_refresh();
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