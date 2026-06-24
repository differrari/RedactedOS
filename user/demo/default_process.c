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
#include "net/net_ctrl.h"

#define BORDER 20

int img_example() {
    draw_ctx ctx = {};
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
    socket_handle_t sock = socket_create(PROTO_UDP, NULL);
    print("Created socket");
    SockBindSpec spec = {};
    spec.kind = BIND_IP;
    spec.ver = IP_VER4;
    //Fill in manually with your local IPV4. address (0.0.0.0 is ANYV4)
    spec.ip[0] = 0;
    spec.ip[1] = 0;
    spec.ip[2] = 0;
    spec.ip[3] = 0;
    if (socket_bind(sock, &spec, 9000) < 0) return -1;

    // socket_listen(&spec);

    void *ptr = zalloc(0x1000);
    print("Waiting for data %i.%i.%i.%i", spec.ip[0],spec.ip[1],spec.ip[2],spec.ip[3]);
    net_l4_endpoint rc = {};
    while (socket_receive(sock, ptr, 0x1000, &rc) == SOCK_ERR_WOULDBLOCK){
    }

    print("Received data from %i.%i.%i.%i:%i", rc.ip[0],rc.ip[1],rc.ip[2],rc.ip[3],rc.port);

    print(ptr);

    // socket_accept(&spec);

    char *str = "Hello node";

    print("Sent %i",socket_send_to(sock, &rc, str, strlen(str)));

    socket_close(sock);

    return 1;
}

int net_ctrl_example() {
    //create ctrl socket
    SocketOptions opt = {};
    opt.special_kind = SOCKET_SPECIAL_CTRL;
    opt.flags = SOCK_OPT_SPECIAL;
    socket_handle_t sock = socket_create(PROTO_NONE, &opt);
    if (!sock) return -1;

    //create a REQUEST to get ADDR
    NetCtrlMsg req = {};
    req.object = NET_CTRL_OBJ_ADDR;
    req.op = NET_CTRL_OP_GET;
    req.flags = NET_CTRL_F_REQUEST;
    req.length = sizeof(req);

    if (socket_send(sock, &req, req.length) < 0) {
        socket_close(sock);
        return -1;
    }

    //read response
    uint8_t rx[1024];
    int64_t n = socket_receive(sock, rx, sizeof(rx), NULL);
    if (n < (int64_t)sizeof(NetCtrlMsg)) {
        socket_close(sock);
        return -1;
    }

    NetCtrlMsg* res = (NetCtrlMsg*)rx;
    if (res->status != SOCK_OK) {
        socket_close(sock);
        return -1;
    }

    //parse respose
    NetCtrlAddrInfo* addrs = (NetCtrlAddrInfo*)NET_CTRL_MSG_DATA(res);
    uint32_t count = NET_CTRL_MSG_PAYLOAD_LEN(res) / sizeof(NetCtrlAddrInfo);
    NetCtrlAddrInfo* main_v4 = NULL;

    //find a valid ip
    for (uint32_t i = 0; i < count; i++) {
        if (addrs[i].prefix.address.ver != IP_VER4) continue;
        if (addrs[i].config != IPV4_CFG_DHCP && addrs[i].config != IPV4_CFG_STATIC) continue;
        main_v4 = &addrs[i];
        break;
    }

    if (!main_v4) {
        socket_close(sock);
        return -1;
    }

    uint32_t current_ip = 0;
    memcpy(&current_ip, main_v4->prefix.address.ip, sizeof(current_ip));

    uint32_t mask = 0xFFFFFFFF << (32 - main_v4->prefix.prefix_len);

    print("main IPv4 is %u.%u.%u.%u/%u mask %u.%u.%u.%u",
        current_ip >> 24, (current_ip >> 16) & 0xFF, (current_ip >> 8) & 0xFF, current_ip & 0xFF,
        main_v4->prefix.prefix_len, mask >> 24, (mask >> 16) & 0xFF, (mask >> 8) & 0xFF, mask & 0xFF);

    uint8_t msg_buf[128];
    memset(msg_buf, 0, sizeof(msg_buf));

    //create a REQUEST to add OBJ_ADDR
    NetCtrlMsg* msg = (NetCtrlMsg*)msg_buf;
    msg->object = NET_CTRL_OBJ_ADDR;
    msg->op = NET_CTRL_OP_ADD;
    msg->flags = NET_CTRL_F_REQUEST;

    uint32_t off = sizeof(NetCtrlMsg);

    //reuse the same L2 interface as the found ipv4
    NetCtrlAttr* attr = (NetCtrlAttr*)(msg_buf + off);
    attr->ext = NET_CTRL_EXT_IFINDEX;
    attr->length = sizeof(main_v4->prefix.ifindex);
    off += sizeof(NetCtrlAttr);
    memcpy(msg_buf + off, &main_v4->prefix.ifindex, sizeof(main_v4->prefix.ifindex));
    off += sizeof(main_v4->prefix.ifindex);

    //reuse the same mask (prefix length)
    attr = (NetCtrlAttr*)(msg_buf + off);
    attr->ext = NET_CTRL_EXT_PREFIX_LEN;
    attr->length = sizeof(main_v4->prefix.prefix_len);
    off += sizeof(NetCtrlAttr);
    memcpy(msg_buf + off, &main_v4->prefix.prefix_len, sizeof(main_v4->prefix.prefix_len));
    off += sizeof(main_v4->prefix.prefix_len);

    uint32_t host_mask = ~mask;
    if (host_mask <= 1) {
        socket_close(sock);
        return -1;
    }

    uint32_t gateway_ip = 0;
    if (main_v4->prefix.gateway.ver == IP_VER4) memcpy(&gateway_ip, main_v4->prefix.gateway.ip, sizeof(gateway_ip));

    uint32_t network = current_ip & mask;
    uint32_t first_host = 1;
    uint32_t last_host = host_mask - 1;
    if (host_mask > 32) {
        first_host = 10;
        last_host = host_mask - 10;
    }
    uint32_t host_span = last_host - first_host + 1;
    uint32_t host = first_host + (((uint32_t)get_time() ^ current_ip) % host_span);
    uint32_t static_ip = network | host;
    for (uint32_t i = 0; i < host_span; i++) {
        if (static_ip != current_ip && static_ip != gateway_ip) break;
        host++;
        if (host > last_host) host = first_host;
        static_ip = network | host;
    }

    print("adding static %u.%u.%u.%u on the same /%u network",
        static_ip >> 24, (static_ip >> 16) & 0xFF, (static_ip >> 8) & 0xFF, static_ip & 0xFF, main_v4->prefix.prefix_len);

    //add another address in the same network and with the same mask
    net_l4_endpoint static_addr = {};
    static_addr.ver = IP_VER4;
    memcpy(static_addr.ip, &static_ip, sizeof(static_ip));

    attr = (NetCtrlAttr*)(msg_buf + off);
    attr->ext = NET_CTRL_EXT_ADDRESS;
    attr->length = sizeof(static_addr);
    off += sizeof(NetCtrlAttr);
    memcpy(msg_buf + off, &static_addr, sizeof(static_addr));
    off += sizeof(static_addr);

    //static config
    int16_t cfg = IPV4_CFG_STATIC;
    attr = (NetCtrlAttr*)(msg_buf + off);
    attr->ext = NET_CTRL_EXT_CONFIG;
    attr->length = sizeof(cfg);
    off += sizeof(NetCtrlAttr);
    memcpy(msg_buf + off, &cfg, sizeof(cfg));
    off += sizeof(cfg);
    msg->length = off;

    int64_t sent = socket_send(sock, msg, msg->length);
    if (sent != (int64_t)msg->length) {
        socket_close(sock);
        return -1;
    }

    n = socket_receive(sock, rx, sizeof(rx), NULL);
    if (n < (int64_t)sizeof(NetCtrlMsg)) {
        socket_close(sock);
        return -1;
    }

    res = (NetCtrlMsg*)rx;
    if (res->status != SOCK_OK) print("add failed with status %i", res->status);
    else print("add ok");
    socket_close(sock);
    return res->status == SOCK_OK ? 0 : -1;
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

void file_sync(){
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
}

void concurrent_write(){
    file fd1 = {};
    file fd2 = {};
    
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
    
    readf(&fd1, buf, 64);
    
    print("Buffer now %s",buf);
}

void write_large_file(){
    void *buf = zalloc(1024);
    
    memset(buf, 'B', 1024);
    
    print("Wrote %x",write_full_file("/boot/redos/fattest", buf, 1024));
    
}

void copypaste(){
    
    char *copythis = "hello";
    clipboard_copy(copythis, strlen(copythis), DATA_SIG_TEXT);
    
    char *buf = clipboard_paste(DATA_SIG_TEXT, 0);
    
    char *copythis2 = "hello1";
    clipboard_copy(copythis2, strlen(copythis2), DATA_SIG_TEXT);
    
    char *copythis3 = "hello2";
    clipboard_copy(copythis3, strlen(copythis3), DATA_SIG_TEXT);
    
    print("Pasted text %s",buf);
    
}

bool should_quit = false;

bool on_quit(signal_info_t *do_not_use_this){
    print("I'm told to quit");
    should_quit = true;
    return true;
}

int main(int argc, char* argv[]){

    img_example();
    
    return 0;
}
