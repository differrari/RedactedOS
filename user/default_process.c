#include "default_process.h"
#include "mouse_input.h"
#include "syscalls/syscalls.h"
#include "input_keycodes.h"
#include "std/string.h"
#include "image/png.h"
#include "image/bmp.h"
#include "audio/cuatro.h"
#include "audio/wav.h"

#define BORDER 20

int img_example() {
    draw_ctx ctx = {};
    file descriptor = {};
    FS_RESULT res = openf("/resources/jest.bmp", &descriptor);
    void *img = 0;
    image_info info;
    void* file_img = malloc(descriptor.size);
    readf(&descriptor, file_img, descriptor.size);
    closef(&descriptor);
    if (res != FS_RESULT_SUCCESS) printf("Couldn't open image");
    else {
        info = bmp_get_info(file_img, descriptor.size);
        printf("info %ix%i",info.width,info.height);
        img = malloc(info.width*info.height*system_bpp);
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
    printf("Created socket for type %i",spec.protocol);
    //Fill in manually with your local IP. A syscall will be added soon to get it for you
    spec.connection.ip[0] = 0;
    spec.connection.ip[1] = 0;
    spec.connection.ip[2] = 0;
    spec.connection.ip[3] = 0;
    if (socket_bind(&spec, IP_VER4, 9000) < 0) return -1;

    // socket_listen(&spec);

    void *ptr = malloc(0x1000);
    printf("Waiting for data %i.%i.%i.%i", spec.connection.ip[0],spec.connection.ip[1],spec.connection.ip[2],spec.connection.ip[3]);
    net_l4_endpoint rc = {};
    while (!socket_receive(&spec, ptr, 0x1000, &rc)){
    }

    printf("Received data from %i.%i.%i.%i:%i", rc.ip[0],rc.ip[1],rc.ip[2],rc.ip[3],rc.port);

    printf(ptr);

    // socket_accept(&spec);

    char *str = "Hello node";

    printf("Sent %i",socket_send(&spec, DST_ENDPOINT, &rc.ip, rc.port, str, strlen(str)));

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
    }else{
        printf("Could not load wav");
        return -1;
    }
    return 0;
}

int main(int argc, char* argv[]){
    img_example();
    return 0;
}
