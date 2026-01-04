#include "uno.h"
#include "ui/draw/draw.h"

gpu_size calculate_label_size(text_ui_config text_config){
    if (!text_config.text || text_config.font_size==0) return (gpu_size){0,0};
    int num_lines = 1;
    int num_chars = 0;
    int local_num_chars = 0;
    for (const char* p = text_config.text; *p; ++p){
        if (*p == '\n'){
            if (local_num_chars > num_chars)
                num_chars = local_num_chars;
            num_lines++;
            local_num_chars = 0;
        }
        else 
            local_num_chars++;
    }
    if (local_num_chars > num_chars)
        num_chars = local_num_chars;
    unsigned int size = fb_get_char_size(text_config.font_size);
    return (gpu_size){size * num_chars, (size + 2) * num_lines };
}

int_point calculate_label_pos(text_ui_config text_config, common_ui_config common_config){
    gpu_size s = calculate_label_size(text_config);
    int_point point = common_config.point;
    switch (common_config.horizontal_align)
    {
    case Trailing:
        point.x = (s.width>=common_config.size.width) ? common_config.point.x : (int32_t)(common_config.point.x + (common_config.size.width - s.width));
        break;
    case HorizontalCenter:
        point.x = (s.width>=common_config.size.width) ? common_config.point.x : (int32_t)(common_config.point.x + ((common_config.size.width - s.width)/2));
        break;
    default:
        break;
    }

    switch (common_config.vertical_align)
    {
    case Bottom:
        point.y = (s.height>=common_config.size.height) ? common_config.point.y : (int32_t)(common_config.point.y + (common_config.size.height - s.height));
        break;
    case VerticalCenter:
        point.y = (s.height>=common_config.size.height) ? common_config.point.y : (int32_t)(common_config.point.y + ((common_config.size.height - s.height)/2));
        break;
    default:
        break;
    }

    return point;
}

common_ui_config label(draw_ctx *ctx, text_ui_config text_config, common_ui_config common_config){
    if (!text_config.text || text_config.font_size==0) return common_config;
    int_point p = calculate_label_pos(text_config, common_config);
    fb_draw_string(ctx, text_config.text, p.x, p.y, text_config.font_size, common_config.foreground_color);
    return common_config;
}

common_ui_config textbox(draw_ctx *ctx, text_ui_config text_config, common_ui_config common_config){
    common_ui_config inner = rectangle(ctx, (rect_ui_config){}, common_config);
    label(ctx, text_config, inner);
    return inner;
}

common_ui_config rectangle(draw_ctx *ctx, rect_ui_config rect_config, common_ui_config common_config){
    if ((common_config.background_color >> 24 | rect_config.border_color >> 24) == 0) return common_config;
    int32_t bx = common_config.point.x;
    int32_t by = common_config.point.y;
    uint32_t bw = common_config.size.width;
    uint32_t bh = common_config.size.height;
    uint32_t b = rect_config.border_size;
    uint32_t p = rect_config.border_padding;
    uint32_t twice_p = p << 1;
    
    if (bx < 0) bw -= -bx;
    if (by < 0) bh -= -by;
    
    int32_t inner_x = bx + (rect_config.border_padding ? 0 : b);
    int32_t inner_y = by + (rect_config.border_padding ? 0 : b);
    uint32_t twice_b = rect_config.border_padding ? 0 : b << 1;
    uint32_t inner_w = (bw > twice_b) ? (bw - twice_b) : 0;
    uint32_t inner_h = (bh > twice_b) ? (bh - twice_b) : 0;
    if (inner_w && inner_h) fb_fill_rect(ctx, inner_x, inner_y, inner_w, inner_h, common_config.background_color);
    
    fb_fill_rect(ctx, bx + p,          by + p,            b,         bh - b - twice_p, rect_config.border_color);
    fb_fill_rect(ctx, bx + p + bw - b - twice_p, by + p + b,        b,         bh - b - twice_p, rect_config.border_color);
    fb_fill_rect(ctx, bx + p + b,      by + p,            bw - b - twice_p,    b, rect_config.border_color);
    fb_fill_rect(ctx, bx + p,          by - p + bh - b ,  bw - b - twice_p,    b, rect_config.border_color);
    
    return (common_ui_config){
        .point = {inner_x + p + b, inner_y + p + b},
        .size = {inner_w + ((p + b) << 1), inner_h + ((p + b) << 1)},
        .background_color = common_config.background_color,
        .foreground_color = common_config.foreground_color,
        .horizontal_align = common_config.horizontal_align,
        .vertical_align = common_config.vertical_align
    };
}