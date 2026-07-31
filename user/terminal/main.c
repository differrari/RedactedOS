#include "syscalls/syscalls.h"
#include "draw/textdraw.h"
#include "shell/shell.h"
#include "shell/sheldon/sheldon.h"
#include "environment/env_types.h"
#include "data/serialize/binary_serial.h"
#include "kbd_helper.h"
#include "memory/memory.h"
#include "utils/embedded_fmt/tcf.h"
#include "files/helpers.h"
#include "header_utils/screenprinter.h"
#include "header_utils/composite.h"
#include "files/system_module.h"
#include "files/stack_fs.h"

embedded_fmt input_format = {};

#define INPUT_MARGIN 10
#define INPUT_HEIGHT (line_height+(INPUT_MARGIN*2))

int history_ptr = 0;
int history_count = 0;

bool headless = false;

bool cursor_on = false;
u64 cursor_blink_time = 500;

u32 bg_color;

buffer input_buf = {};

draw_ctx ctx = {};

gpu_rect screen_rect = {};

shell_handle* main_shell;

void flush(shell_handle *handle, bool can_scroll);

static char log_buf[1024];

int debug_print(const char *fmt, ...){
    __attribute__((aligned(16))) va_list args;
    va_start(args, fmt); 
    memset(log_buf, 0, 1024);
    size_t n = string_format_va_buf(fmt, log_buf, sizeof(log_buf), args);
    va_end(args);
    if (n >= sizeof(log_buf)) log_buf[sizeof(log_buf)-1] = '\0';
    printl(log_buf);
    return 0;
}

shell_handle* make_default_shell(shell_bindings bindings){
    return create_sheldon(bindings, 0, 0);
}

void clear(shell_handle *handle){
    if (main_shell && main_shell != handle) return;
    // buffer_wipe(&contents);
    fb_clear(&ctx, screen_printer_formatting.current_bg_color);
}

bool return_from_parse = false;

void put_char(shell_handle *handle, char c){
    // print("New char %c %x %x %i",c,handle,main_shell,contents.cursor);
    if (!c || (main_shell && main_shell != handle)) return;
    bool render = true;
    if (embedded_fmt_parse(&screen_printer_formatting, c)){
        render = false;
        return_from_parse = true;
        if (screen_printer_formatting.wipe){
            screen_printer_formatting.wipe = false;
            clear(handle);
            return;
        }
    }
    if (!render)
        return;
    if (headless)
        serial_transmit(c);
    else {
        screen_printer_put_char(c);
        if (return_from_parse) flush(handle, true);
        return_from_parse = false;
    }
}

void flush(shell_handle *handle, bool can_scroll){
    // print("Flush %v {%i,%i}",slice_from_buffer(&contents),rerender_range.start,rerender_range.size);
    if (main_shell && main_shell != handle) return;
    
    screen_print_flush(can_scroll);
}

void refresh_input(){
    fb_fill_rect(&ctx, 0, ctx.height-INPUT_HEIGHT, ctx.width, INPUT_HEIGHT, input_format.current_bg_color);
    gpu_size offset = fb_draw_slice(&ctx, SLICE("> "), 0, ctx.height-INPUT_HEIGHT+INPUT_MARGIN, TEXT_SCALE, input_format.current_text_color);
    fb_draw_slice(&ctx, slice_from_buffer(&input_buf), offset.width, ctx.height-INPUT_HEIGHT+INPUT_MARGIN, TEXT_SCALE, input_format.current_text_color);
    if (cursor_on)
        fb_fill_rect(&ctx, offset.width + (char_width * input_buf.cursor), ctx.height-INPUT_HEIGHT+INPUT_MARGIN, char_width, INPUT_HEIGHT-(INPUT_MARGIN*2), input_format.current_text_color);
    commit_draw_ctx(&ctx);
}

void bell(shell_handle *handle){
    if (main_shell && main_shell != handle) return;
    print("DING");
}

void ascii_cmd(shell_handle *handle, char cmd, u16 proc_id){
    if (main_shell && main_shell != handle) return;
    switch (cmd){
        case ASCII_CMD_ETX:
            send_signal(SIG_QUIT, proc_id);
        break;
        case ASCII_CMD_SUB:
            send_signal(SIG_STOP, proc_id);
        break;
        case ASCII_CMD_CAN:
            send_signal(SIG_CONT, proc_id);
        break;
        default: break;
    }
}

void console_ctrl(shell_handle *handle, console_ctrls ctrl){
    if (main_shell && main_shell != handle) return;
    switch (ctrl) {
        case console_ctrl_close: halt(0);
        default: break;
    }
}

void emit_data(structdef field, sizedptr data, bool is_allocated){
    // if (main_shell && main_shell != handle) return;
    print("[TERMINAL implementation error] structured data displaying not implemented");
    // if (!data.ptr || !data.size) return;
    //     switch (field.type) {
    //     case binary_type_i8: print("%S: %i",field.name,*(i8*)data.ptr); break;
    //     case binary_type_i16: print("%S: %i",field.name,*(i16*)data.ptr); break;
    //     case binary_type_i32: print("%S: %i",field.name,*(i32*)data.ptr); break;  
    //     case binary_type_i64: print("%S: %i",field.name,*(i64*)data.ptr); break;
    //     case binary_type_float: print("%S: %f",field.name,*(float*)data.ptr); break;
    //     case binary_type_double: print("%S: %f",field.name,*(double*)data.ptr); break;
    //     case binary_type_string: print("%S: %v",field.name,data); break;
    //     default: return;
    // }
    // if (is_allocated) release((void*)data.ptr);
}

void flush_proxy(shell_handle *handle){
    flush(handle, true);
}

shell_bindings terminal_bindings = (shell_bindings){
    .console_output = put_char,
    .console_flush = flush_proxy,
    .console_clean = clear,
    .console_bell = bell,
    .console_ascii_cmd = ascii_cmd,
    .console_control = console_ctrl
};

shell_handle* create_shell(){
    shell_handle *handle = make_default_shell(terminal_bindings);
    if (!handle) return 0;
    return handle;
}

bool erase(bool forward){
    if (forward && input_buf.cursor >= input_buf.buffer_size) return false;
    if (!buffer_delete(&input_buf, input_buf.cursor + forward, 1)) return false;

    refresh_input();
    return true;
}

bool run_command(){

    bool success = false;

    screen_printer_append("\r\n");

    write_full_file("/termhistory", input_buf.buffer, input_buf.buffer_size);
    history_count++;
    history_ptr = history_count;
    
    if (run_cmd(main_shell, slice_from_buffer(&input_buf))) success = true;

    buffer_wipe(&input_buf);
    return success;
}

bool move_buf_cursor(i64 amount){
    if (!amount) return false;
    uptr old_in_c = input_buf.cursor;
    if (buffer_seek(&input_buf, amount, false) == old_in_c) return false;
    refresh_input();
    return true;
}

bool scroll_history(i64 amount){
    int new_history_ptr = history_ptr + amount;
    if (new_history_ptr < 0) new_history_ptr = 0;
    string file = string_format("/termhistory/%i",new_history_ptr);
    fs_stat st = {};
    if (statf(file.data, &st)){
        size_t hist_size = 0;
        char *history = read_full_file(file.data, &hist_size);
        if (input_buf.buffer) buffer_destroy(&input_buf);
        input_buf = (buffer){
            .buffer = history,
            .buffer_size = hist_size,
            .limit = hist_size,
            .options = buffer_can_grow,
            .cursor = hist_size
        };
        history_ptr = new_history_ptr;
        return true;
    }
    buffer_wipe(&input_buf);
    return false;
}

void handle_mouse(){
    mouse_data data = {};
    get_mouse_status(&data);
    if (data.raw.scroll){
        screen_print_scroll(data.raw.scroll, false);
    }
}

bool handle_input(){
    
    handle_mouse();
    
    kbd_event event;
    if (!read_event(&event)) return false;
    if (event.type == KEY_RELEASE) return true;
    if (handle_modifier(&event)) return true;

    char key = event.key;
    char readable = hid_to_char((uint8_t)key, current_modifier, special_key);

    if (key == KEY_ENTER || key == KEY_KPENTER) return run_command();

    if (key == KEY_BACKSPACE) return erase(false);
    if (key == KEY_DELETE) return erase(true);

    if (key == KEY_UP) return scroll_history(-1);
    if (key == KEY_DOWN) return scroll_history(1);
    
    if (key == KEY_LEFT) return move_buf_cursor(-1);
    if (key == KEY_RIGHT) return move_buf_cursor(1);

    if (key == KEY_PAGEDOWN || key == KEY_PAGEUP)
        screen_print_scroll(key == KEY_PAGEDOWN ? -1 : 1, false);
    
    if (!readable) return false;
    
    if (!input_buf.buffer) input_buf = buffer_create(0x100, buffer_can_grow);

    buffer_write_lim(&input_buf, &readable, 1);
    refresh_input();

    flush(main_shell, true);
    
    return true;
    
}

void toggle_cursor(){
    cursor_on = !cursor_on;
    refresh_input();
}

bool quit = false;

bool termhistory_init(system_module *mod){
    printl("Mounting mod");
    // while (true) printl("I'm told to quit %i");
    return true;
}

bool custom_stat(const char *path, fs_stat *out_stat){
    print("Stat %s into %x",path, out_stat);
    stat_dir(out_stat);
    return true;
}

size_t custom_readdir(const char *path, void *buf, size_t size, file_offset *offset){
    print("READDIR %s info %x (%i) (%x)",path,buf,size,offset);
    fs_dir_list_helper helper = create_dir_list_helper(buf, size);
    dir_list_fill(&helper, "test");
    return dir_buf_size(&helper);
}

FS_RESULT custom_open(const char *path, file *fd){
    fd->id = 1;
    print("Sending id %i",fd->id);
    fd->size = 100;
    fd->data_type = DATA_SIGNATURE("ASYNC");
    return FS_RESULT_SUCCESS;
}

system_module termhistory_mod = {
    .name = "terminal history",
    .mount = "termhistory",
    .version = VERSION_NUM(0, 1, 0, 0),
    .open = custom_open,
    .read = stackfs_read,
    .write = stackfs_write,
    .readdir = custom_readdir,
    .getstat = custom_stat
};

int main(){    
    request_app_ctx(&ctx);

    screen_printer_init(dummy_draw_ctx(ctx.width, ctx.height-INPUT_HEIGHT));

    stackfs_init();
    load_fsmodule(&termhistory_mod, true);

    u32 color_buf[2] = {};
    sreadf("/theme", &color_buf, sizeof(uint64_t));
    if ((color_buf[0] & 0xFF000000) == 0) color_buf[0] |= 0xFF000000;
    if ((color_buf[1] & 0xFF000000) == 0) color_buf[1] |= 0xFF000000;
    screen_printer_formatting.default_bg_color = screen_printer_formatting.current_bg_color = color_buf[0];
    screen_printer_formatting.default_text_color = screen_printer_formatting.current_text_color = color_buf[1];

    init_tcf(&screen_printer_formatting);

    memcpy(&input_format, &screen_printer_formatting, sizeof(text_format));

    fb_clear(&ctx, screen_printer_formatting.current_bg_color);

    main_shell = create_shell();

    current_shell = main_shell;

    u64 cursor_current = 0;

    u64 last_time = get_time();
    refresh_input();
    while (true){
        if (quit) halt(0);
        // append("Bonjour %.3i \t",i++);
        u64 current_time = get_time();
        cursor_current += (current_time-last_time);
        if (cursor_current >= cursor_blink_time){
            toggle_cursor();
            cursor_current = 0;
        }
        last_time = current_time;
        handle_input();
        msleep(1);
        ctx.full_redraw = true;
        composite(&printer_ctx, (int_point){}, 1, &ctx, draw_ctx_rect(&ctx));
        commit_draw_ctx(&ctx);
    }
    
    return 0;
}