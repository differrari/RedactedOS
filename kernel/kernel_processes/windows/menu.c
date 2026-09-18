#include "menu.h"

#include "graphic_types.h"
#include "graph/graphics.h"
#include "graph/tres.h"
#include "theme/theme.h"
#include "syscalls/syscalls.h"
#include "input/input_dispatch.h"
#include "filesystem/modules/fs_isolation.h"
#include "filesystem/filesystem.h"
#include "data/struct/stack.h"
#include "math/math.h"
#include "memory/memory.h"

#define draw_eye(x_off, blink, eyelid) fb_fill_rect(ctx, rect.point.x + eye_margin + x_off, rect.point.y + (rect.size.height * 0.1), eye_size.width, eye_size.height, (blink) ? eyelid : 0xFFcccccc);\
fb_fill_rect(ctx, rect.point.x + eye_margin + p.x + x_off, rect.point.y + (rect.size.height * 0.1) + p.y, eye_size.width-pupil_distance.width, eye_size.height-pupil_distance.height, (blink) ? eyelid : 0xFF333333);

void test_widget(draw_ctx *ctx, gpu_rect rect, color bg){

    gpu_size eye_size = {rect.size.width * 0.2,rect.size.height * 0.3};
    i32 eye_distance = eye_size.width * 1.1;
    i32 eye_margin = (rect.size.width - (eye_size.width*2 + eye_distance))/2;

    gpu_size pupil_distance = {eye_size.width*0.3,eye_size.height*0.3};
    
    mouse_data data;
    get_mouse_status(&data);

    gpu_point p = get_mouse_pos();
    p.x = p.x * pupil_distance.width/ctx->width;
    p.y = p.y * pupil_distance.height/ctx->height;

    draw_eye(0,data.raw.buttons & 1, bg+0x111111);
    draw_eye(eye_size.width + eye_distance,(data.raw.buttons >> 1) & 1, bg+0x111111);

    fb_fill_rect(ctx, rect.point.x + eye_margin, rect.size.height - eye_size.height - eye_size.height/3, rect.size.width-(eye_margin*2), eye_size.height-(get_raw_mouse_in().scroll*3), 0xFF222222);
    if ((data.raw.buttons >> 2) & 1) fb_fill_rect(ctx, rect.point.x + eye_margin, rect.size.height - eye_size.height/2 - eye_size.height/3, rect.size.width-(eye_margin*2), eye_size.height/2, 0xFF442222);
    
}

bool menu_dirty = true;

void refresh_menu(){
#if FEATURE_MENU
    menu_dirty = true;  
#endif
}

#define MENU_MAX_NAME 64
#define MENU_MAX_TOOLTIP 256
#define MENU_MAX_FILEPATH 256

typedef struct menu_item_t {
    u8 name_len;
    char name[MENU_MAX_NAME];
    u8 tooltip_len;
    char tooltip[MENU_MAX_TOOLTIP];
    u8 filepath_len;
    char filepath[MENU_MAX_TOOLTIP];
    size_t num_children;
    bool is_submenu;
    module_root *module;
    struct menu_item_t *parent;
    struct menu_item_t *child;
    struct menu_item_t *sibling;
} menu_item_t;

arr_stack_t *menu_items = 0;

menu_item_t *first_menu_item = 0, *last_menu_item = 0;

menu_item_t* menu_add_entry(char *name, menu_item_t *parent){
    menu_item_t *item = stack_new_item(menu_items,menu_item_t);
    string_slice sl = slice_from_literal(name);
    item->name_len = min(MENU_MAX_NAME,sl.length);
    memcpy(item->name, sl.data, item->name_len);
    item->parent = parent;
    if (last_menu_item){
        if (last_menu_item->parent == parent) last_menu_item->sibling = item;
        else if (parent) parent->child = item;
        if (parent) parent->num_children++;
    } else first_menu_item = item;
    last_menu_item = item;
    return item;
}

string menu_create_pathname(menu_item_t *parent){
    string prec = {};
    if (parent && parent->parent){
        prec = menu_create_pathname(parent->parent);
    } else prec = string_from_literal("/menu");
    if (parent){
        string new_p = string_format("%S/%v",prec,(string_slice){parent->name,parent->name_len});
        string_free(prec);
        prec = new_p;
    }
    return prec;
}

void load_menu_level(menu_item_t *parent){
    process_t *menu_proc = get_proc_by_pid(sys_get_focused_pid());
    if (!menu_proc){ print("[MENU debug] no focused process"); return; }
    string path = menu_create_pathname(parent);
    module_root *localfs = get_fs_for_id(menu_proc->permissions.fs_id);
    if (!localfs) return;

    void *buf = zalloc(0x1000);
    u64 off = 0;
    size_t s = list_directory_contents(localfs, path.data, buf, 0x1000, &off);
    if (!s) return;
    string_list *list = (string_list*)buf;
    if (list){
        char* reader = (char*)list->array;
        for (uint32_t i = 0; i < list->count; i++){
            char *file = reader;
            if (*file){
                menu_item_t *item = menu_add_entry(file,parent);
                item->module = localfs;
                string_format_buf(item->filepath, MENU_MAX_FILEPATH, "%S/%s",path,file);
                fs_stat stat = {};
                get_stat(localfs, item->filepath, &stat);
                item->is_submenu = stat.type == entry_directory;
            }
            while (*reader) reader++;
            reader++;
        }
    }
    string_free(path);
}

void menu_reset(){
    if (!menu_items) menu_items = stack_create(sizeof(menu_item_t), 64);
    else stack_reset(menu_items);
    
    last_menu_item = 0;
    first_menu_item = 0;
}

void load_menu(){
    menu_reset();
    load_menu_level(0);
}

size_t calc_children(menu_item_t *parent){
    if (!parent) return 0;
    size_t acc = parent->num_children;
    menu_item_t *child = parent->child;
    while (child){
        acc += calc_children(child);
        child = child->sibling;
    }
    return acc;
}

void unload_menu_children(menu_item_t *parent){
    if (!parent) {
        menu_item_t *child = first_menu_item;
        size_t num_menu_items = 0;
        while (child){
            if (child->parent) break;
            num_menu_items++;
            child->child = 0;
            child->num_children = 0;
            child = child->sibling;
        }
        stack_remove(menu_items,stack_count(menu_items)-num_menu_items);
        last_menu_item = stack_get(menu_items,stack_count(menu_items)-1);
        return;
    }
    size_t num_to_delete = calc_children(parent);
    stack_remove(menu_items, num_to_delete);
    parent->child = 0;
    parent->num_children = 0;
    last_menu_item = stack_get(menu_items,stack_count(menu_items)-1);
}

bool mouse_in_menu(gpu_rect menu, gpu_point click){
    if (click.x < menu.point.x || click.x >= menu.point.x + (i32)menu.size.width || 
        click.y < menu.point.y || click.y >= menu.point.y + (i32)menu.size.height) return false;
    return true;
}

typedef union {
    struct {
        u8 quit: 1;
        u8 clicked: 1;
        u8 inside: 1;
    };
    u8 info;
} menu_info;

menu_info draw_submenu(draw_ctx *ctx, gpu_point origin, menu_item_t *parent, bool did_click, gpu_point mouse_click){
    int width = 256;
    fb_fill_rect(ctx, origin.x, origin.y, width, parent->num_children*MENU_HEIGHT, system_theme.bg_color+0x181818);
    fb_outline_rect(ctx, origin.x, origin.y, width, parent->num_children*MENU_HEIGHT, 2, 0x44000000);
    
    menu_item_t *menu = parent->child;
    
    int y = (MENU_HEIGHT-fb_line_height(2))/2;
    int y_top = 0;
    menu_info info;
    while (menu){
        int x = 10;
        gpu_rect bounds = {{origin.x,origin.y + y_top },{width,MENU_HEIGHT}};
        bool clicked_inside = mouse_in_menu(bounds, mouse_click);
        info.inside |= clicked_inside;
        if (did_click && clicked_inside){
            if (menu->is_submenu){
                if (!menu->child) load_menu_level(menu);
                else unload_menu_children(menu);
            } else if (menu->module){
                transform_file(menu->module, menu->filepath, 0, 0);
                info.quit = true;
            }
            info.clicked = true;
        }
        if (menu->child){
            draw_submenu(ctx, (gpu_point){ origin.x + width, origin.y + y_top }, menu, did_click, mouse_click);
            fb_fill_rect(ctx, origin.x + 3, origin.y + y_top + 3, width - 6, MENU_HEIGHT-6, 0x44000000);
        }
        fb_draw_slice(ctx, (string_slice){menu->name,menu->name_len}, origin.x + x, origin.y + y, 2, 0xFFcccccc);
        if (menu->is_submenu) fb_draw_slice(ctx, SLICE(">"), origin.x + width-fb_get_char_size(2)-10, origin.y + y, 2, 0xFFcccccc);
        y += MENU_HEIGHT;
        y_top += MENU_HEIGHT;
        menu = menu->sibling;
    }
    return info;
}

bool mouse_can_click = true;

bool draw_menu(gpu_point mouse_pos){
    if (menu_dirty){
        load_menu();
        menu_dirty = false;
    }
    draw_ctx *screen_ctx = gpu_get_ctx();
    fb_fill_rect(screen_ctx, 0, 0, screen_ctx->width, MENU_HEIGHT, system_theme.bg_color+0x111111);
    fb_fill_rect(screen_ctx, 0, MENU_HEIGHT-BORDER_SIZE, screen_ctx->width, BORDER_SIZE, 0x44000000);
    
    menu_item_t *menu = first_menu_item;
    
    int x = 10;
    bool did_click = false, did_click_inside = false;
    gpu_point mouse_click = {};
    
    bool mouse_in = mouse_pos.y < MENU_HEIGHT;
    
    if (mouse_button_pressed(LMB)){
        if (mouse_can_click)
            did_click = true;
        mouse_can_click = false;
    } else mouse_can_click = true;
    mouse_click = mouse_pos;
    while (menu){
        int y = (MENU_HEIGHT-fb_line_height(2))/2;
        fb_draw_slice(screen_ctx, (string_slice){menu->name,menu->name_len}, x, y, 2, 0xFFcccccc);
        gpu_rect bounds = {{x,y},{(menu->name_len * fb_get_char_size(2)),fb_line_height(2)}};
        if (did_click && mouse_in_menu(bounds, mouse_click)){
            did_click_inside = true;
            if (!menu->child){
                unload_menu_children(0);
                load_menu_level(menu);
            } else unload_menu_children(menu);
        }
        if (menu->child){
            menu_info info = draw_submenu(screen_ctx, (gpu_point){ x, MENU_HEIGHT-BORDER_SIZE }, menu, did_click, mouse_click);
            did_click_inside |= info.clicked;
            mouse_in |= info.inside;
            if (info.quit) {
                unload_menu_children(0);
                return mouse_in;
            }
        }
        x += bounds.size.width + 10;
        menu = menu->sibling;
    }
    
    if (did_click && !did_click_inside) {
        unload_menu_children(0);
    }

    // fb_fill_rect(screen_ctx, screen_ctx->width/2 - 100, 0, screen_ctx->width/2 + 100, MENU_HEIGHT, 0xb4dd13);

    test_widget(screen_ctx, (gpu_rect){{screen_ctx->width/2 - 100, 0}, {200, MENU_HEIGHT}},system_theme.bg_color+0x111111);
    
    return mouse_in;
}