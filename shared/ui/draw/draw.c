#include "draw.h"
#include "ui/font8x8_bridge.h"
#include "std/memory.h"
#include "math/math.h"

#define line_height char_size + 2

int try_merge(gpu_rect* a, gpu_rect* b) {
    //TODO: drawing to full height is not working, needs to be -1
    uint32_t ax2 = a->point.x + a->size.width;
    uint32_t ay2 = a->point.y + a->size.height;
    uint32_t bx2 = b->point.x + b->size.width;
    uint32_t by2 = b->point.y + b->size.height;

    if (a->point.x > bx2 || b->point.x > ax2 || a->point.y > by2 || b->point.y > ay2)
        return false;

    uint32_t min_x = a->point.x < b->point.x ? a->point.x : b->point.x;
    uint32_t min_y = a->point.y < b->point.y ? a->point.y : b->point.y;
    uint32_t max_x = ax2 > bx2 ? ax2 : bx2;
    uint32_t max_y = ay2 > by2 ? ay2 : by2;

    a->point.x = min_x;
    a->point.y = min_y;
    a->size.width = max_x - min_x;
    a->size.height = max_y - min_y;

    return true;
}

void mark_dirty(draw_ctx *ctx, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (ctx->full_redraw) return;
    
    if (x >= ctx->width || y >= ctx->height)
        return;

    if (x + w > ctx->width)
        w = ctx->width - x;

    if (y + h > ctx->height)
        h = ctx->height - y;

    if (w == 0 || h == 0)
        return;

    gpu_rect new_rect = { {x, y}, {w, h} };

    for (uint32_t i = 0; i < ctx->dirty_count; i++)
        if (try_merge(&ctx->dirty_rects[i], &new_rect))
            return;

    if (ctx->dirty_count < MAX_DIRTY_RECTS)
        ctx->dirty_rects[ctx->dirty_count++] = new_rect;
    else
        ctx->full_redraw = true;
}

void fb_clear(draw_ctx *ctx, uint32_t color) {
    memset(ctx->fb, color, ctx->stride * ctx->height);
    ctx->full_redraw = true;
}

void fb_draw_raw_pixel(draw_ctx *ctx, uint32_t x, uint32_t y, color color){
    if (x >= ctx->width || y >= ctx->height) return;
    ctx->fb[y * (ctx->stride / 4) + x] = color;
}

void fb_draw_pixel(draw_ctx *ctx, uint32_t x, uint32_t y, color color){
    fb_draw_raw_pixel(ctx, x, y, color);
    mark_dirty(ctx, x,y,1,1);
}

void fb_fill_rect(draw_ctx *ctx, uint32_t x, uint32_t y, uint32_t width, uint32_t height, color color){
    if (x + width >= ctx->width || y + height >= ctx->height) return;
    if (width == ctx->width && x == 0){
        memset(ctx->fb + (y * (ctx->width)) , color, ctx->stride * height);
    } else {
        for (uint32_t dy = 0; dy < height; dy++)
            memset(ctx->fb + ((y+dy) * (ctx->width)) + x, color, width*4);
    }
    mark_dirty(ctx, x,y,width,height);
}

void fb_draw_img(draw_ctx *ctx, uint32_t x, uint32_t y, uint32_t *img, uint32_t img_width, uint32_t img_height){
    if (x + img_width >= ctx->width || y + img_height >= ctx->height) return;
    if (img_width == ctx->width && x == 0){
        memcpy(ctx->fb + (y * (ctx->width)), img, ctx->stride * img_height);
    } else {
        for (uint32_t dy = 0; dy < img_height; dy++)
            memcpy(ctx->fb + ((y+dy) * (ctx->width)) + x, img + (dy * img_width), img_width*4);
    }
    mark_dirty(ctx, x,y,img_width,img_height);
}

gpu_rect fb_draw_line(draw_ctx *ctx, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, color color){
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sy = (y0 < y1) ? 1 : -1;
    int err = (dx > dy ? dx : -dy) / 2, e2;

    for (;;) {
        fb_draw_raw_pixel(ctx, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 < dy) { err += dx; y0 += sy; }
    }

    int min_x = (x0 < x1) ? x0 : x1;
    int min_y = (y0 < y1) ? y0 : y1;
    int max_x = (x0 > x1) ? x0 : x1;
    int max_y = (y0 > y1) ? y0 : y1;

    mark_dirty(ctx, min_x,min_y,max_x - min_x + 1,max_y - min_y + 1);

    return (gpu_rect) { {min_x, min_y}, {max_x - min_x + 1, max_y - min_y + 1}};
}

void fb_draw_raw_char(draw_ctx *ctx, uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color){
    const uint8_t* glyph = get_font8x8((uint8_t)c);
    for (uint32_t row = 0; row < (CHAR_SIZE * scale); row++) {
        uint8_t bits = glyph[row/scale];
        for (uint32_t col = 0; col < (CHAR_SIZE * scale); col++) {
            if (bits & (1 << (7 - (col / scale)))) {
                fb_draw_raw_pixel(ctx, x + col, y + row, color);
            }
        }
    }
}

void fb_draw_char(draw_ctx *ctx, uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color){
    fb_draw_raw_char(ctx, x, y, c, scale, color);
    mark_dirty(ctx, x,y,CHAR_SIZE*scale,CHAR_SIZE*scale);
}

gpu_size fb_draw_string(draw_ctx *ctx, const char* s, uint32_t x0, uint32_t y0, uint32_t scale, uint32_t color){
    int char_size = fb_get_char_size(scale);
    int str_length = strlen(s,0);
    
    uint32_t xoff = 0;
    uint32_t xSize = 0;
    uint32_t xRowSize = 0;
    uint32_t ySize = line_height;
    for (int i = 0; i < str_length; i++){    
        char c = s[i];
        if (c == '\n'){
            y0 += line_height; 
            ySize += line_height;
            if (xRowSize > xSize)
                xSize = xRowSize;
            xRowSize = 0;
            xoff = 0;
        } else {
            fb_draw_raw_char(ctx, x0 + (xoff * char_size),y0,c,scale, color);
            xoff++;
            xRowSize += char_size;
        }
    }
    if (xRowSize > xSize)
        xSize = xRowSize;

    mark_dirty(ctx, x0,y0,xSize,ySize);

    return (gpu_size){xSize,ySize};
}

uint32_t fb_get_char_size(uint32_t scale){
    return CHAR_SIZE * scale;
}

void fb_draw_cursor(draw_ctx *ctx, uint32_t color) {
    // 22x24 Px
    const uint8_t cursor_bitmap[22][24] =
    {
        {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0},
        {1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
    };

    for (uint32_t y = 0; y < 22; y++){
        for (uint32_t x = 0; x < 24; x++)
        {
            if (cursor_bitmap[y][x] == 1)
            {
                fb_draw_raw_pixel(ctx, x, y, color);
            }
        }
    }
}
