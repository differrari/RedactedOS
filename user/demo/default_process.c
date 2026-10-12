#include "default_process.h"
#include "mouse_input.h"
#include "syscalls/syscalls.h"
#include "input_keycodes.h"
#include "std/string.h"
#include "image/png.h"
#include "image/bmp.h"
#include "audio/cuatro.h"
#include "audio/wav.h"
#include "memory/memory.h"
#include "files/helpers.h"
#include "utils/clipboard.h"
#include "math/math.h"
#include "draw/textdraw.h"
#include "environment/env_types.h"
#include "utils/theme.h"

draw_ctx ctx = {};

#define BORDER 20

int img_example() {
    file descriptor = {};
    FS_RESULT res = openf("/resources/jest.bmp", &descriptor);
    void *img = 0;
    image_info info;
    void* file_img = zalloc(descriptor.size);
    readf(&descriptor, file_img, descriptor.size);
    closef(&descriptor);
    if (res != FS_RESULT_SUCCESS) print("Couldn't open image");
    else {
        info = bmp_get_info(file_img, descriptor.size);
        print("info %ix%i",info.width,info.height);
        img = zalloc(info.width*info.height*system_bpp);
        bmp_read_image(file_img, descriptor.size, img);
    }
    ctx.width = info.width+BORDER*2;
    ctx.height = info.height+BORDER*2;
    request_draw_ctx(&ctx);
    while (1) {
        mouse_data data;
        get_mouse_status(&data);
        keypress kp = {};
        // printf("Print console test %f", (get_time()/1000.f));
        if (read_key(&kp))
            if (kp.keys[0] == KEY_ESC)
                halt(0);
        fb_clear(&ctx, 0xFFFFFFFF);
        // fb_fill_rect(&ctx, rect.point.x, rect.point.y, rect.size.width, rect.size.height, 0xFF222233);
        if (img) fb_draw_img(&ctx, BORDER, BORDER, img, info.width, info.height);
        // fb_draw_string(&ctx, "Print screen test", rect.point.x, rect.point.y, 2, 0xFFFFFFFF);
        commit_draw_ctx(&ctx);
    }
    return 0;
}

int net_example() {
    SocketHandle spec = {};
    socket_create(SOCKET_SERVER, PROTO_UDP, NULL, &spec);
    print("Created socket for type %i",spec.protocol);
    //Fill in manually with your local IP. A syscall will be added soon to get it for you
    spec.connection.ip[0] = 0;
    spec.connection.ip[1] = 0;
    spec.connection.ip[2] = 0;
    spec.connection.ip[3] = 0;
    if (socket_bind(&spec, IP_VER4, 9000) < 0) return -1;

    // socket_listen(&spec);

    void *ptr = zalloc(0x1000);
    print("Waiting for data %i.%i.%i.%i", spec.connection.ip[0],spec.connection.ip[1],spec.connection.ip[2],spec.connection.ip[3]);
    net_l4_endpoint rc = {};
    while (!socket_receive(&spec, ptr, 0x1000, &rc)){
    }

    print("Received data from %i.%i.%i.%i:%i", rc.ip[0],rc.ip[1],rc.ip[2],rc.ip[3],rc.port);

    print(ptr);

    // socket_accept(&spec);

    char *str = "Hello node";

    print("Sent %i",socket_send(&spec, DST_ENDPOINT, &rc.ip, rc.port, str, strlen(str)));

    socket_close(&spec);

    return 1;
}

static int8_t mixin[MIXER_INPUTS] = { NULL };
static audio_samples audio[MIXER_INPUTS];

int audio_example(){
    for (int i = 0; i < MIXER_INPUTS; ++i) mixin[i] = -1;
    mixer_master_level(AUDIO_LEVEL_MAX * 0.75f);
    if (wav_load_as_int16("/resources/scale.wav", audio)){
        mixin[0] = audio_play_sync(&audio[0], 0, AUDIO_ONESHOT, AUDIO_LEVEL_MAX/4, PAN_CENTRE);
        return 0;
    } else {
        print("Could not load wav");
        return -1;
    }
    return 0;
}

char reffub[256];

int file_sync(){
    file testfd = {};
    openf("/shared/test", &testfd);
    
    readf(&testfd, reffub, 256);
    
    print("Initial contents of file: %s",reffub);
    
    testfd.cursor = testfd.size;
    
    writef(&testfd, "Ciao mond", 9);
    
    testfd.cursor = 0;
    
    memset(reffub, 0, 256);
    readf(&testfd, reffub, 256);
    print("New contents of file: %s",reffub);

    return 0;
}

int concurrent_write(){
    file fd1 = { .sync_type = cursor_sync_follow };
    file fd2 = { .sync_type = cursor_sync_follow };
    
    openf("/tmp/std", &fd1);
    openf("/tmp/std", &fd2);
    
    print("FD1: %i FD2: %i",fd1.id,fd2.id);
    
    print("one wrote %i. Now %i",writef(&fd1, "one", 3), fd1.cursor);
    print("two wrote %i. Now %i",writef(&fd2, "two", 3), fd2.cursor);
    print("three wrote %i. Now %i",writef(&fd1, "three", 5), fd1.cursor);
    
    char buf[64];
    
    seek(&fd1, 0, SEEK_ABSOLUTE);
    
    print("first wrote %i. Now %i",writef(&fd1, "first", 5));
    
    seek(&fd1, 0, SEEK_ABSOLUTE);
    
    size_t n = readf(&fd1, buf, 64);

    if (!slice_lit_match((string_slice){buf,n}, "firstonetwothree", false)){
        print("Slice |%v| does not match firstonetwothree",(string_slice){buf,n});
        shame();
    }

    return 0;
}

int concurrent_write_proc(bool spawn){
    
    if (spawn){
        const char* arg = "0xC0CE717E2";
        exec("/boot/redos/system/demo.red", 1, &arg, EXEC_MODE_KEEP_FOCUS);
    }
    
    file fd = {.sync_type = cursor_sync_follow};
    
    openf("/tmp/std", &fd);
    
    int total_written = 0;
    
    for (int i = 0; i < 100; i++){
        char buf[4];
        size_t n = string_format_buf(buf, 4, "%i\n", i);
        size_t r = writef(&fd, buf, n);
        total_written += r;
        print("Writing %v: %i",(string_slice){buf,n},r);
        msleep(0);
    }
    
    msleep(0);
    print("Total written by %i = %i",spawn,total_written);
    
    if (spawn){
        // fs_stat out_stat = {};
        // statf("/tmp/std", &out_stat);
        // fd.size = out_stat.size;
        // print("> %i %i",fd.cursor,fd.size);
        // seek(&fd, 0, SEEK_ABSOLUTE);
        // print("> %i %i",fd.cursor,fd.size);
        // char *buf = zalloc(fd.size);
        // size_t n = readf(&fd, buf, fd.size);
        // print("Final result %i %s",n,buf);
        print("%v",read_file_slice("/tmp/std"));
    }
    
    return 0;
}

int concurrent_write_parent(){
    return concurrent_write_proc(true);
}

int concurrent_write_child(){
    return concurrent_write_proc(false);
}

int write_large_file(){
    void *buf = zalloc(1024);
    
    memset(buf, 'B', 1024);
    
    print("Wrote %x",write_full_file("/boot/redos/fattest", buf, 1024));

    return 0;
    
}

int copypaste(){
    
    char *copythis = "hello";
    clipboard_copy(copythis, strlen(copythis), DATA_SIG_TEXT);
    
    char *buf = clipboard_paste(DATA_SIG_TEXT, 0);
    
    char *copythis2 = "hello1";
    clipboard_copy(copythis2, strlen(copythis2), DATA_SIG_TEXT);
    
    char *copythis3 = "hello2";
    clipboard_copy(copythis3, strlen(copythis3), DATA_SIG_TEXT);
    
    print("Pasted text %s",buf);

    return 0;
    
}

int test_mouse(){
    while (true){
        mouse_data data = {};
        get_mouse_status(&data);
        fb_clear(&ctx, 0xff0c0c0c);
        kbd_event ev;
        if (read_event(&ev)){
            if (ev.key == KEY_ESC) return 0;
        }
        color col = 0xffb4dd13;
        if (data.raw.buttons & 1) col = 0xff674928;
        if ((data.raw.buttons >> 1) & 1) col = 0xff398019;
        if ((data.raw.buttons >> 2) & 1) col = 0xff029387;
        fb_fill_rect(&ctx, data.position.x, data.position.y, 128 + data.raw.x, 128 + data.raw.y, col);
        commit_draw_ctx(&ctx);
    }
}

bool should_quit = false;

bool on_quit(signal_info_t *do_not_use_this){
    msleep(3000);
    should_quit = true;
    // while (true) printl("I'm told to quit");
    return true;
}

int log_test(){
    int count = 0;
    while (true){
        fb_clear(&ctx, 0xff0c0c0c);
        kbd_event ev;
        if (read_event(&ev)){
            if (ev.key == KEY_ESC) return 0;
        }
        print("Log %i",count++);
        commit_draw_ctx(&ctx);
    }
}

int pipe_source(){
    const char* arg = "0x919ED";
    print("Hello from source pipe");
    u16 id = exec("/boot/redos/system/demo.red", 1, &arg, EXEC_MODE_KEEP_FOCUS);//TODO: exec does not automatically add argv[0] as it should
    thread_inspect(TINSPECT_INPUT, id, 1);
    u16 own_id = 0;
    sreadf("/proc/id",&own_id,sizeof(u16));
    string src = string_format("/proc/%i/out",own_id);
    string dst = string_format("/proc/%i/in",id);
    file pipe = {};
    pipef(src.data,dst.data, pipe_from_beginning, &pipe);
    print("Piping %S into %S with fd %i",src,dst,pipe.id);
    msleep(2000);
    return 0;
}

int pipe_destination(){
    print("Hello from destination pipe");
    size_t n = 0;
    char buf[256];
    while (!n){
        n = readf(&(file){
            .id = FD_IN,
        }, buf, 256);
    }
    string_slice msg = {buf,n};
    print("Received message %v",msg);
    msleep(1000);
    return 0;
}

struct { u64 code; char* name; int (*fn)(); } demos[] = {
    {0xC0CE717E1, "Concurrent write [Parent]", concurrent_write_parent},
    {0xC0CE717E2, "Concurrent write [Child]", concurrent_write_child},
    {0x919E5,"Pipe source", pipe_source},
    {0x919ED,"Pipe destination", pipe_destination},
    {0xD1591A1,"Display image on screen", img_example},
    // {0x0000000000000000,"Networking demo", net_example},
    {0xAD10,"Audio demo", audio_example},
    {0x5F01DE9,"Shared folder demo (requires 9Pfs)", file_sync},
    {0xC0CE7F11E,"Concurrent writing to file (WIP)", concurrent_write},
    {0xC0501E07,"Console output test",log_test},
    {0x917E1A96EF11E,"Write large file", write_large_file},
    {0xC099A57E,"Copy-paste to clipboard", copypaste},
    {0x05E7E57,"Mouse test",test_mouse},
};

int run_code(u64 code){
    for (u64 i = 0; i < N_ARR(demos); i++){
        if (demos[i].code == code)
            return demos[i].fn();
    }
    return 0;
}

char code_buf[18];
u8 code_size = 0;

void print_code(char c, gpu_point *cursor,gpu_rect rect, gpu_size *size, text_format text_fmt){
    if (code_size >= 18) return;
    if ((c >= 'a' && c >= 'f') || (c >= '0' && c >= '9')) code_buf[code_size++] = c;
    range_t range = {0,1};
    fb_continuous_draw_text(&ctx, draw_text_render, cursor, (string_slice){&c,1}, &range, rect, size, (gpu_point){}, text_fmt, (text_format_arr){ });
    ctx.full_redraw = true;
    commit_draw_ctx(&ctx);
}

int main(int argc, char* argv[]){
    
    handle_signal(SIG_QUIT, on_quit);
    // send_signal(SIG_QUIT, 7);//TODO: get own procid?

    window_info_t info = {
        .name = "Demo program",
        .name_length = 12
    };
    env_set_window_info(&info);

    request_draw_ctx(&ctx);

    gpu_point cursor = {};

    string_slice initial = SLICE("Collection of demos of the system's functionality. The code is the most relevant part, they might not do anything interesting visually and might outright close the program after they're done.\n\n");
    
    range_t range = {.size = initial.length};

    gpu_rect rect = {{0,0},{ctx.width,ctx.height}};

    text_format text_fmt = {.scale = 3,.foreground = 0xFFFFFFFF,.background = 0xFF354657,.wrap = wrap_word};
    
    theme_palette palette = {};
    get_theme(&palette);
    
    fb_clear(&ctx, palette.background);
    
    if (argc > 0){
        char *id = argv[0];
        size_t l = strlen(id);
        if (l >= 2 && !strncmp(id, "0x", 2)){
            u64 code = parse_hex_u64(id, l);
            print("Executing code %llx",code);
            return run_code(code);
        } else {
            u16 selection = parse_int_u64(id, 1);
            if (selection < N_ARR(demos)){
                return demos[selection].fn();
            }
        }
    }

    gpu_size size = {};
    fb_continuous_draw_text(&ctx, draw_text_render, &cursor, initial, &range, rect, &size, (gpu_point){}, text_fmt, (text_format_arr){ });

    int count = min(9,N_ARR(demos));
    for (int i = 0; i < count; i++){
        string f = string_format("[%i]: %s\n",i+1, demos[i].name);
        range.size = f.length;
        fb_continuous_draw_text(&ctx, draw_text_render, &cursor, slice_from_string(f), &range, rect, &size, (gpu_point){}, text_fmt, (text_format_arr){ });
        string_free(f);
    }

    bool code_mode = false;
    while (true) {
        commit_draw_ctx(&ctx);
        if (should_quit) {
            print("SIG QUIT");
            halt(0);
        }
        kbd_event ev = {};
        if (read_event(&ev)){
            if (ev.type == KEY_PRESS){
                if (ev.key == KEY_TAB){ 
                    code_mode = !code_mode;
                    code_size = 0;
                    if (code_mode){
                        print_code('\n',&cursor,rect,&size,text_fmt);
                        print_code('0',&cursor,rect,&size,text_fmt);
                        print_code('x',&cursor,rect,&size,text_fmt);
                    }
                }
                if (!code_mode){
                    if (ev.key >= KEY_1 && ev.key <= (KEY_1 + count - 1)){
                        int index = ev.key - KEY_1;
                        if (index >= 0 && index < count){
                            fb_clear(&ctx, 0);
                            commit_draw_ctx(&ctx);
                            return demos[index].fn();
                        }
                    }
                } else {
                    if ((ev.key >= KEY_1 && ev.key <= KEY_0) || (hid_keycode_to_char[ev.key] >= 'a' && hid_keycode_to_char[ev.key] <= 'f')){
                        if (code_size < 18){
                            char c = hid_keycode_to_char[ev.key];
                            print_code(c,&cursor,rect,&size,text_fmt);                            
                        }
                    }
                    if (ev.key == KEY_ENTER){
                        u64 code = parse_hex_u64(code_buf, code_size);
                        return run_code(code);
                    }
                }
                if (ev.key == KEY_ESC) halt(0);
            }
        }
    }
    
    return 0;
}
