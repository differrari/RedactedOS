#include "draw.h"
#include "ui/font8x8_bridge.h"
#include "std/memory.h"

int try_merge(gpu_rect* a, gpu_rect* b) {
    uint32_t ax1 = a->point.x;
    uint32_t ay1 = a->point.y;
    uint32_t ax2 = ax1 + a->size.width;
    uint32_t ay2 = ay1 + a->size.height;
    uint32_t bx1 = b->point.x;
    uint32_t by1 = b->point.y;
    uint32_t bx2 = bx1 + b->size.width;
    uint32_t by2 = by1 + b->size.height;

    if (ax1 >= bx2 || bx1 >= ax2 || ay1 >= by2 || by1 >= ay2)
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
    if (x >= ctx->width || y >= ctx->height) return;

    if (x + w > ctx->width) w = ctx->width - x;
    if (y + h > ctx->height) h = ctx->height - y;
    if (w == 0 || h == 0) return;

    gpu_rect new_rect = (gpu_rect){{x, y}, {w, h}};

    int merged = 0;
    for (uint32_t i = 0; i < ctx->dirty_count; i++) {
        if (try_merge(&ctx->dirty_rects[i], &new_rect)) {
            merged = 1;
            break;
        }
    }
    if (!merged) {
        if (ctx->dirty_count < MAX_DIRTY_RECTS) {
            ctx->dirty_rects[ctx->dirty_count++] = new_rect;
        } else {
            ctx->full_redraw = 1;
            return;
        }
    }

    for (uint32_t i = 0; i < ctx->dirty_count; ++i) {
        for (uint32_t j = i + 1; j < ctx->dirty_count; ) {
            if (try_merge(&ctx->dirty_rects[i], &ctx->dirty_rects[j])) {
                for (uint32_t k = j + 1; k < ctx->dirty_count; ++k)
                    ctx->dirty_rects[k - 1] = ctx->dirty_rects[k];
                ctx->dirty_count--;
            } else {
                ++j;
            }
        }
    }

    uint64_t area_sum = 0;
    for (uint32_t r = 0; r < ctx->dirty_count; ++r)
        area_sum += (uint64_t)ctx->dirty_rects[r].size.width * (uint64_t)ctx->dirty_rects[r].size.height;

    const uint64_t screen_area = (uint64_t)ctx->width * (uint64_t)ctx->height;
    if (area_sum * 100 >= screen_area * FULL_REDRAW_THRESHOLD_PCT) ctx->full_redraw = 1;
}

#ifndef CROSS

void fb_clear(draw_ctx *ctx, uint32_t color) {
    uint32_t* row = ctx->fb;
    const uint32_t w = ctx->width;
    const uint32_t h = ctx->height;
    const uint32_t pitch = ctx->stride >> 2;

    for (uint32_t y = 0; y < h; y++) {
        uint32_t *p = row;
        uint32_t n = w;

        if (((uintptr_t)p & 7) && n) {
            *p++ = color;
            --n;
        }
        uint64_t pat = ((uint64_t)color << 32) | color;
        uint64_t *q = (uint64_t*)p;
        for (uint32_t i = 0; i < (n >> 1); i++) q[i] = pat;
        p = (uint32_t*)(q + (n >> 1));
        if (n & 1) *p = color;

        row += pitch;
    }

    ctx->full_redraw = 1;
}


//TODO: all functions should include this with the (if alpha < 0xFF) check
uint32_t pixel_blend(uint32_t p1, uint32_t p2){
    uint16_t a1 = (p1 >> 24) & 0xFF;
    uint16_t a2 = (p2 >> 24) & 0xFF;
    if (a2 == 0) return p1;
    if (a1 == 0) return p2;

    uint16_t r1 = (p1 >> 16) & 0xFF;
    uint16_t r2 = (p2 >> 16) & 0xFF;

    uint16_t g1 = (p1 >> 8) & 0xFF;
    uint16_t g2 = (p2 >> 8) & 0xFF;

    uint16_t b1 = p1        & 0xFF;
    uint16_t b2 = p2        & 0xFF;

    uint8_t a = a1 + ((255 - a1) * a2)/255;

    uint8_t r = ((r1 * a1) + (r2 * a2 * (255 - a1))/255)/a;
    uint8_t g = ((g1 * a1) + (g2 * a2 * (255 - a1))/255)/a;
    uint8_t b = ((b1 * a1) + (b2 * a2 * (255 - a1))/255)/a;

    return (a << 24) | (r << 16) | (g << 8) | (b);
}

void fb_draw_raw_pixel(draw_ctx *ctx, uint32_t x, uint32_t y, color color){
    if (x >= ctx->width || y >= ctx->height) return;
    if (((color >> 24) & 0xFF) < 0xFF)
        ctx->fb[y * (ctx->stride >> 2) + x] = pixel_blend(color, ctx->fb[y * (ctx->stride >> 2) + x]);
    else
        ctx->fb[y * (ctx->stride >> 2) + x] = color;
}

void fb_draw_pixel(draw_ctx *ctx, uint32_t x, uint32_t y, color color){
    fb_draw_raw_pixel(ctx, x, y, color);
    mark_dirty(ctx, x,y,1,1);
}

void fb_fill_rect(draw_ctx *ctx, int32_t x, int32_t y, uint32_t width, uint32_t height, color color){
    if (x >= (int32_t)ctx->width || y >= (int32_t)ctx->height || x + (int32_t)width < 0 || y + (int32_t)height < 0) return;

    int32_t w = width;
    int32_t h = height;
    if (x < 0){
        width -= -x;
        x = 0;
    }
    if (y < 0){
        height -= -y;
        y = 0;
    }
    if (x + w > (int32_t)ctx->width) w = ctx->width - x;
    else if (x < 0){ w += x; x = 0; }
    if (y + h > (int32_t)ctx->height) h = ctx->height - y;
    else if (y < 0){ h += y; y = 0; }

    if (w <= 0 || h <= 0) return;

    uint8_t alpha = ((color >> 24) & 0xFF);

    if (alpha < 0xFF && alpha > 0){
        for (int32_t dy = 0; dy < h; dy++){
            for (int32_t dx = 0; dx < w; dx++){
                fb_draw_raw_pixel(ctx, x + dx, y + dy, color);  
            }
        }
        return;
    }

    const uint32_t dst_pitch_px = (ctx->stride >> 2);
    uint32_t* dst_row = ctx->fb + y * dst_pitch_px + x;
    for (int32_t row = 0; row < h; ++row) {
        uint32_t *p = dst_row;
        uint32_t n = w;
        uint32_t col = (uint32_t)color;

        if (((uintptr_t)p & 7) && n) { *p++ = col; --n; }
        uint64_t pat = ((uint64_t)col << 32) | (uint64_t)col;
        uint64_t *q = (uint64_t*)p;
        for (uint32_t i = 0; i < (n >> 1); ++i) q[i] = pat;
        p = (uint32_t*)(q + (n >> 1));
        if (n & 1) *p = col;

        dst_row += dst_pitch_px;
    }
    mark_dirty(ctx, x,y,w,h);
}

void fb_draw_img(draw_ctx *ctx, uint32_t x, uint32_t y, uint32_t *img, uint32_t img_width, uint32_t img_height){
    fb_draw_partial_img(ctx, img, x, y, img_width, img_height, (image_transform){});
}

void fb_draw_partial_img(draw_ctx *ctx, uint32_t *img, uint32_t x, uint32_t y, uint32_t full_width, uint32_t full_height, image_transform transform){
    if (x >= ctx->width || y >= ctx->height) return;

    if (transform.start_x >= full_width) return;

    uint32_t w = transform.img_width == 0 ? full_width : transform.img_width;
    uint32_t h = transform.img_height == 0 ? full_height : transform.img_height;

    if (w > full_width - transform.start_x) w = full_width - transform.start_x;

    if (x + w > ctx->width) w = ctx->width - x;
    if (y + h > ctx->height) h = ctx->height - y;
    if (!w || !h) return;

    uint32_t y_sind = transform.flip_y ? (full_height-transform.start_y) : transform.start_y;

    const uint32_t dst_pitch_px = (ctx->stride >> 2);
    uint32_t* dst_row = ctx->fb + y * dst_pitch_px + x;

    const uint32_t src_pitch_px = full_width;
    const uint32_t* src_row = img + (y_sind * full_width);

    for (uint32_t row = 0; row < h; ++row) {
        const uint32_t* src = src_row;
        uint32_t* dst = dst_row;
        for (uint32_t col = 0; col < w; ++col) {
            uint32_t x_ind = transform.flip_x ? full_width-transform.start_x-col : transform.start_x + col;
            uint32_t pix = src[x_ind];
            if (((pix >> 24) & 0xFF) < 0xFF)
                dst[col] = pixel_blend(pix, dst[col]);
            else
                dst[col] = pix;
        }
        dst_row += dst_pitch_px;
        if (transform.flip_y) src_row -= src_pitch_px;
        else src_row += src_pitch_px;
    }

    mark_dirty(ctx, x,y,w, h);
}

gpu_rect fb_draw_line(draw_ctx *ctx, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, color color){
    const uint32_t ox0 = x0, oy0 = y0, ox1 = x1, oy1 = y1;

    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 > y0) ? (y0 - y1) : (y1 - y0);
    dy = dy < 0 ? -dy : dy;
    int sy = (y0 < y1) ? 1 : -1;
    int err = (dx > dy ? dx : -dy) / 2, e2;

    for (;;) {
        fb_draw_raw_pixel(ctx, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 < dy) { err += dx; y0 += sy; }
    }

    uint32_t min_x = (ox0 < ox1) ? ox0 : ox1;
    uint32_t min_y = (oy0 < oy1) ? oy0 : oy1;
    uint32_t max_x = (ox0 > ox1) ? ox0 : ox1;
    uint32_t max_y = (oy0 > oy1) ? oy0 : oy1;

    if (max_x >= ctx->width) max_x = ctx->width - 1;
    if (max_y >= ctx->height) max_y = ctx->height - 1;

    const uint32_t bw = (max_x >= min_x) ? (max_x - min_x + 1) : 0;
    const uint32_t bh = (max_y >= min_y) ? (max_y - min_y + 1) : 0;

    if (bw && bh) mark_dirty(ctx, min_x, min_y, bw, bh);

    return (gpu_rect) { {min_x, min_y}, {bw, bh} };
}

void fb_draw_raw_char(draw_ctx *ctx, uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color){
    const uint8_t* glyph = get_font8x8((uint8_t)c);
    const uint32_t char_size = CHAR_SIZE * scale;
    if (x >= ctx->width || y >= ctx->height) return;

    uint32_t max_w = ctx->width - x;
    uint32_t max_h = ctx->height - y;
    uint32_t draw_w = char_size <= max_w ? char_size : max_w;
    uint32_t draw_h = char_size <= max_h ? char_size : max_h;

    const uint32_t row_pitch = ctx->stride >> 2;

    for (uint32_t gy = 0; gy < 8; ++gy) {
        uint32_t base_y = gy * scale;
        if (base_y >= draw_h) break;
        uint32_t ry_lim = scale;
        if (base_y + ry_lim > draw_h) ry_lim = draw_h - base_y;
        uint8_t bits = glyph[gy];
        for (uint32_t ry = 0; ry < ry_lim; ++ry) {
            uint32_t* dst = ctx->fb + (y + base_y + ry) * row_pitch + x;
            for (uint32_t gx = 0; gx < 8; ++gx) {
                uint32_t base_x = gx * scale;
                if (base_x >= draw_w) break;
                if (bits & (1u << (7 - gx))) {
                    uint32_t rx_lim = scale;
                    if (base_x + rx_lim > draw_w) rx_lim = draw_w - base_x;
                    uint32_t* p = dst + base_x;
                    for (uint32_t rx = 0; rx < rx_lim; ++rx) { 
                        if (((color >> 24) & 0xFF) < 0xFF)
                            p[rx] = pixel_blend(color, p[rx]);
                        else
                            p[rx] = color;
                    }
                }
            }
        }
    }
}

void fb_draw_char(draw_ctx *ctx, uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color){
    fb_draw_raw_char(ctx, x, y, c, scale, color);
    mark_dirty(ctx, x,y,CHAR_SIZE*scale,CHAR_SIZE*scale);
}

gpu_size fb_draw_string(draw_ctx *ctx, const char* s, uint32_t x0, uint32_t y0, uint32_t scale, uint32_t color){
    const uint32_t char_size = fb_get_char_size(scale);
    const int str_length = strlen(s);
    
    uint32_t xoff = 0;
    uint32_t xSize = 0;
    uint32_t xRowSize = 0;
    uint32_t ySize = 0;

    const uint32_t start_y = y0;
    uint32_t rows = 1;

    for (int i = 0; i < str_length; ++i){    
        char c = s[i];
        if (c == '\n'){
            if (xRowSize > xSize)
                xSize = xRowSize;
            xRowSize = 0;
            xoff = 0;
            y0 += (char_size + 2);
            rows++;
        } else {
            fb_draw_raw_char(ctx, x0 + (xoff * char_size),y0,c,scale, color);
            xoff++;
            xRowSize += char_size;
        }
    }
    if (xRowSize > xSize)
        xSize = xRowSize;

    ySize = rows * (char_size + 2);

    uint32_t bbox_w = xSize;
    uint32_t bbox_h = ySize;

    if (x0 + bbox_w > ctx->width) bbox_w = ctx->width - x0;
    if (start_y + bbox_h > ctx->height) bbox_h = ctx->height - start_y;

    if (bbox_w && bbox_h) mark_dirty(ctx, x0,start_y,bbox_w,bbox_h);

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

    const uint32_t row_pitch = (ctx->stride >> 2);
    for (uint32_t y = 0; y < 22; y++){
        uint32_t* dst = ctx->fb + y * row_pitch;
        for (uint32_t x = 0; x < 24; x++)
        {
            if (cursor_bitmap[y][x])
            {
                dst[x] = color;
            }
        }
    }
}

#endif