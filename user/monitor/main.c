#include "syscalls/syscalls.h"
#include "files/helpers.h"
#include "debug/proc.h"
#include "math/math.h"
#include "uno/uno.h"
#include "utils/theme.h"
#include "kbd_helper.h"
#include "debug/profiler.h"

chunk_array_t *active_procs;

void on_proc(const char *dir, const char *file){
    print("Process id %s",file);
    
    i64 proc_id = parse_int_u64(file, min(5,strlen(file)));
    
    if (proc_id > UINT16_MAX) return;
    
    string fmt = string_format("/proc/%i/info",proc_id);
    
    proc_info inf = {};
    
    if (sreadf(fmt.data, &inf, sizeof(proc_info)) == sizeof(proc_info)){
        chunk_array_push(active_procs, &inf);
    }
    
    string_free(fmt);
}

theme_palette palette;

chunk_array_t *strings_list;

void render(){
    size_t proc_count = chunk_array_count(active_procs);

    if (!strings_list) strings_list = chunk_array_create(sizeof(string), 64);
    else {
        size_t string_count = chunk_array_count(strings_list);
        for (u64 i = 0; i < string_count; i++)
            string_free(CHUNK_ARRAY_GET(string, strings_list, i));
        chunk_array_reset(strings_list);
    }
    
    VERTICAL((node_info){

    }, {
        for (u64 i = 0; i < proc_count; i++){
            proc_info *info = chunk_array_get(active_procs, i);
            HORIZONTAL((node_info){}, {
                uno_label((node_info){.fg_color = palette.foreground }, doc_text_body, SLICE("["));
                string id = string_format("%i ",info->id);
                chunk_array_push(strings_list, &id);
                uno_label((node_info){.fg_color = palette.foreground }, doc_text_body, slice_from_string(id));
                uno_label((node_info){.fg_color = palette.foreground }, doc_text_body, SLICE(info->state == PROC_READY || info->state == PROC_RUNNING ? "RUNNING" : "STOPPED"));
                uno_label((node_info){.fg_color = palette.foreground }, doc_text_body, info->privilege ? SLICE(" K") : SLICE(" U"));
                uno_label((node_info){.fg_color = palette.foreground }, doc_text_body, SLICE("] "));
                uno_label((node_info){.fg_color = palette.foreground }, doc_text_body, (string_slice){.data = info->procname, .length = info->procnamelen});
                string heap = string_format(" Memory: %llx",info->heap.size);
                chunk_array_push(strings_list, &heap);
                uno_label((node_info){.fg_color = palette.foreground}, doc_text_body, slice_from_string(heap));
                // string stack = string_format(" Stack: %llx",info->stack.size);
                // chunk_array_push(strings_list, &stack);
                // uno_label((node_info){.fg_color = palette.foreground}, doc_text_body, slice_from_string(stack));
                DEPTH((node_info){}, {
                    size_t total_size = 75;
                    uno_begin_horizontal((node_info){
                        .sizing_rule = size_absolute, /* .padding = 10,*/ .bg_color = palette.background + 0x111111, .rect = (gpu_rect){ .size = {total_size,fb_line_height(text_to_scale(doc_text_body))}}
                    });
                    uno_end_horizontal();
                    size_t used_stack = (info->stack.ptr-info->sp);
                    size_t fill_size = total_size*((float)used_stack/info->stack.size);
                    if (used_stack && !fill_size) fill_size = 1;
                    uno_begin_horizontal((node_info){
                        .sizing_rule = size_absolute, /* .padding = 10,*/ .bg_color = palette.background + 0x444444, .rect = (gpu_rect){ .size = {fill_size,fb_line_height(text_to_scale(doc_text_body))}}
                    });
                    uno_end_horizontal();
                });
            });
        }
    });
}

void refresh(){
    chunk_array_reset(active_procs);
    traverse_directory("/proc", false, on_proc);
    
    uno_refresh();
}

void input(){
    kbd_event ev = {};
    if (read_event(&ev)){
        if (handle_modifier(&ev)) return;
        if (ev.type != KEY_PRESS) return;
        if (ev.key == KEY_ESC && is_mod_pressed(KEY_LEFTCTRL, true)){
            halt(0);
        }
    }
}

int main(){
    
    active_procs = chunk_array_create(sizeof(proc_info), 64);
    
    draw_ctx ctx = {};
    
    request_draw_ctx(&ctx);
    
    uno_set_document_view(render, draw_ctx_rect(&ctx));
    
    if (!get_theme(&palette)){
        palette.background = 0xFF222233;
        palette.foreground = 0xFFCCCCCC;
    }
    
    uno_clear_color(palette.background);
    
    refresh();
    
    profiler_init();
    
    i64 refresh_delta = 1000;
    
    while (true){
        uno_draw(&ctx);
        commit_draw_ctx(&ctx);
        refresh_delta -= profiler_delta();
        if (refresh_delta < 0){
            refresh();
            refresh_delta = 1000;
        }
        input();
    }
    
    return 0;
    
}