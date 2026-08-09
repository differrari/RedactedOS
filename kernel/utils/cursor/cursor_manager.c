#include "cursor_manager.h"
#include "syscalls/syscalls.h"
#include "files/helpers.h"
#include "image/bmp.h"

draw_ctx current_cursor;
cursor_types current_cursor_type;

char *cursor_names[] = {
    [cursor_pointer] = "pointer.bmp",
    [cursor_crosshair] = "crosshair.bmp",
    [cursor_pencil] = "pencil.bmp",
};

#define CURSOR_DIR "/boot/redos/system/cursors"

void default_cursor(){
    if (!switch_cursor(cursor_pointer)){
        draw_ctx ctx = dummy_draw_ctx(64, 64);
        fb_draw_cursor(&ctx, 0xFFFFFFFF);
        current_cursor = ctx;
    }
}

bool switch_cursor(cursor_types type){
    if (current_cursor_type == type) return true;
    if (current_cursor.fb) release(current_cursor.fb);
    image_info info = {};
    char *name = cursor_names[type];
    if (!name) {
        print("Cursor type %i not found",name);
        return false;
    }
    string s = string_format("%s/%s",CURSOR_DIR,name);
    void *pixels = load_bmp(s.data, &info);
    string_free(s);
    if (info.width != CURSOR_WIDTH || info.height != CURSOR_HEIGHT){ 
        print("[CURSOR error] wrong cursor size %ix%i, expected %ix%i",info.width,info.height,CURSOR_WIDTH,CURSOR_HEIGHT);
        return false;
    }
    if (!pixels){
        print("[CURSOR error] failed to load cursor image");
        return false;
    }
    current_cursor = buffer_to_draw_ctx(pixels, info.width, info.height);
    current_cursor_type = type;
    return true;
}

void print_cursor(const char *dir, const char *name){
    print("%s",name);
}

void list_cursors(){
    traverse_directory(CURSOR_DIR, false, print_cursor);
}

draw_ctx get_cursor(){
    return get_resized_cursor(CURSOR_WIDTH,CURSOR_HEIGHT);
}

draw_ctx get_resized_cursor(int width, int height){
    if (!current_cursor.fb) default_cursor();
    if (width != CURSOR_WIDTH || height != CURSOR_HEIGHT){
        //resize
        print("[CURSOR implementation error] resizing cursors not supported");
    }
    return current_cursor;
}
