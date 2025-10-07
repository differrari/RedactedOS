#include "default_process.h"
#include "syscalls/syscalls.h"
#include "input_keycodes.h"
#include "std/string.h"
#include "image/png.h"

#define BORDER 20

int img_example() {
    draw_ctx ctx = {};
    request_draw_ctx(&ctx);
    file descriptor;
    FS_RESULT res = fopen("/resources/test.png", &descriptor);
    void *img = 0;
    image_info info;
    void* file_img = malloc(descriptor.size);
    fread(&descriptor, file_img, descriptor.size);
    if (res != FS_RESULT_SUCCESS) printf("Couldn't open image");
    else {
        info = png_get_info(file_img, descriptor.size);
        printf("info %ix%i",info.width,info.height);
        img = malloc(info.width*info.height*system_bpp);
        png_read_image(file_img, descriptor.size, img);
    }
    // resize_draw_ctx(&ctx, info.width+BORDER*2, info.height+BORDER*2);
    while (1) {
        mouse_input mouse = {};
        get_mouse_status(&mouse);
        // printf("Mouse %i",mouse.x);
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
    socket_create(SOCKET_SERVER, PROTO_UDP, &spec);
    printf("Created socket for type %i",spec.protocol);
    spec.connection.ip[0] = 192;
    spec.connection.ip[1] = 168;
    spec.connection.ip[2] = 1;
    spec.connection.ip[3] = 108;
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

    printf("Sent %i",socket_send(&spec, DST_ENDPOINT, &rc.ip, rc.port, str, strlen(str, 0)));

    socket_close(&spec);

    return 1;
}

int main(){
    img_example();
    return 0;
}