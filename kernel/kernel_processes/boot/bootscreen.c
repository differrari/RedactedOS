#include "bootscreen.h"
#include "../kprocess_loader.h"
#include "console/kio.h"
#include "graph/graphics.h"
#include "std/string.h"
#include "theme/theme.h"
#include "input/input_dispatch.h"
#include "process/scheduler.h"
#include "math/math.h"
#include "syscalls/syscalls.h"
#include "filesystem/filesystem.h"
#include "ui/uno/uno.h"
#include "../shared/audio/cuatro.h"
#include "../shared/audio/wav.h"
#include "../shared/audio/tone.h"

file boot_fd;

void boot_draw_name(gpu_size screen_size, int xoffset, int yoffset){
    uint16_t pid = get_current_proc_pid();
    if (boot_fd.size == 0){
        string proc_out = string_format("/proc/%i/out",pid);
        open_file(proc_out.data, &boot_fd);
        free(proc_out.data, proc_out.mem_length);
    }
    write_file(&boot_fd, "hello buffer", 12);

    draw_ctx *ctx = gpu_get_ctx();
    label(ctx, (text_ui_config){
        .text = BOOTSCREEN_TEXT,
        .font_size = 2,
    }, (common_ui_config){
        .point = {0, screen_size.height/2 + yoffset},
        .size = { screen_size.width, (screen_size.height/2) - yoffset},
        .horizontal_align = HorizontalCenter,
        .vertical_align = Top,
        .background_color = BG_COLOR,
        .foreground_color = COLOR_WHITE,
    });
}

const gpu_point offsets[BOOTSCREEN_NUM_SYMBOLS] = BOOTSCREEN_OFFSETS;

gpu_point boot_calc_point(gpu_point offset, gpu_size screen_size, gpu_point screen_middle){
    int xoff = (screen_size.width/BOOTSCREEN_DIV) * offset.x;
    int yoff = (screen_size.height/BOOTSCREEN_DIV) * offset.y;
    int xloc = BOOTSCREEN_PADDING + xoff;
    int yloc = BOOTSCREEN_PADDING + yoff;
    return (gpu_point){screen_middle.x + xloc - (abs(offset.x) == 1 ? BOOTSCREEN_ASYMM.x : 0),  screen_middle.y + yloc - (abs(offset.y) == 1 ? BOOTSCREEN_ASYMM.y : 0)};
}

void boot_draw_lines(gpu_point current_point, gpu_point next_point, gpu_size size, int num_lines, int separation){
    int steps = 0;
    for (int j = 0; j < num_lines; j++) {
        gpu_point ccurrent = current_point;
        gpu_point cnext = next_point;
        if (current_point.x != 0 && current_point.x < size.width)
            ccurrent.x += (separation * j);
        if (next_point.x != 0 && next_point.x < size.width)
            cnext.x += (separation * j);
        if (current_point.y != 0 && current_point.y < size.height)
            ccurrent.y -= (separation * j);
        if (next_point.y != 0 && next_point.y < size.height)
            cnext.y -= (separation * j);
        int xlength = abs(ccurrent.x - cnext.x);
        int ylength = abs(ccurrent.y - cnext.y);
        int csteps = xlength > ylength ? xlength : ylength;
        if (csteps > steps) steps = csteps;
    }
    
    for (int i = 0; i <= steps; i++) {
        for (int j = 0; j < num_lines; j++){
            gpu_point ccurrent = current_point;
            gpu_point cnext = next_point;
            if (current_point.x != 0 && current_point.x < size.width)
                ccurrent.x += (separation * j);
            if (next_point.x != 0 && next_point.x < size.width)
                cnext.x += (separation * j);
            if (current_point.y != 0 && current_point.y < size.height)
                ccurrent.y -= (separation * j);
            if (next_point.y != 0 && next_point.y < size.height)
                cnext.y -= (separation * j);
            int xlength = abs(ccurrent.x - cnext.x);
            int ylength = abs(ccurrent.y - cnext.y);
            int csteps = xlength > ylength ? xlength : ylength;
            if (csteps > i){
                gpu_point interpolated = {
                    lerp(i, ccurrent.x, cnext.x, csteps),
                    lerp(i, ccurrent.y, cnext.y, csteps)
                };
                gpu_draw_pixel(interpolated, 0xFFFFFFFF);
            }
        }
        kbd_event kbd = {};
        if (sys_read_event_current(&kbd))
            stop_current_process(0);
        gpu_flush();
    }
}

static audio_samples audio[3];

static void play_startup_sound(){
    mixer_master_level(AUDIO_LEVEL_MAX / 2);
    if (wav_load_as_int16("/boot/redos/startup.wav", audio)){
        audio_play_async(&audio[0], 0, AUDIO_ONESHOT_FREE, AUDIO_LEVEL_MAX / 12, PAN_CENTRE);
    }else{
        static envelope_defn env = { ENV_ASD, 0.025, 0.03 };
        static sound_defn s0 = { WAVE_SQUARE, 440, 440 };
        static sound_defn s1 = { WAVE_SQUARE, 587.33, 587.33 };
        static sound_defn s2 = { WAVE_SQUARE, 659.25, 659.25 };
        sound_create(3.f, &s0, &audio[0]);
        sound_shape(&env, &audio[0]);
        sound_create(3.f, &s1, &audio[1]);
        sound_shape(&env, &audio[1]);
        sound_create(3.f, &s2, &audio[2]);
        sound_shape(&env, &audio[2]);
        audio_play_async(&audio[0], 0, AUDIO_ONESHOT_FREE, AUDIO_LEVEL_MAX / 16, PAN_HALFLEFT);
        audio_play_async(&audio[1], 150, AUDIO_ONESHOT_FREE, AUDIO_LEVEL_MAX / 16, PAN_CENTRE);
        audio_play_async(&audio[2], 300, AUDIO_ONESHOT_FREE, AUDIO_LEVEL_MAX / 16, PAN_HALFRIGHT);
    }
}

int bootscreen(){
    disable_visual();
    sys_focus_current();
    gpu_size screen_size = gpu_get_screen_size();
    mouse_config((gpu_point){screen_size.width/2,screen_size.height/2}, screen_size);
    play_startup_sound();
    while (1)
    {
        gpu_clear(BG_COLOR);
        gpu_point screen_middle = {screen_size.width/2,screen_size.height/2};
        
        gpu_point current_point = boot_calc_point(offsets[BOOTSCREEN_NUM_SYMBOLS-1],screen_size,screen_middle);
        for (int i = 0; i < BOOTSCREEN_NUM_STEPS; i++){
            gpu_point offset = offsets[i];
            gpu_point next_point = boot_calc_point(offset,screen_size,screen_middle);
            boot_draw_name(screen_size, 0, BOOTSCREEN_PADDING + screen_size.height/BOOTSCREEN_UPPER_Y_DIV + 10);
            boot_draw_lines(current_point, next_point, screen_size, BOOTSCREEN_REPEAT, 5);
            current_point = next_point;
        }
        sleep(1000);
    }
    return 0;
}

process_t* start_bootscreen(){
    return create_kernel_process("bootscreen",bootscreen, 0, 0);
}