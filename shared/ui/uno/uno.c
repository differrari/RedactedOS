#include "uno.h"
#include "ui/draw/draw.h"

gpu_size calculate_label_size(text_ui_config text_config){
    int num_lines = 1;
    int num_chars = 0;
    int local_num_chars = 0;
    for (unsigned int i = 0; i < strlen(text_config.text, 0); i++){
        if (text_config.text[i] == '\n'){
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
    return (gpu_size){size * num_chars, size * num_lines};
}

gpu_point calculate_label_pos(text_ui_config text_config, common_ui_config common_config){
    gpu_point point = common_config.point;
    switch (common_config.horizontal_align)
    {
    case Trailing:
        point.x = (common_config.point.x + common_config.size.width) - calculate_label_size(text_config).width;
        break;
    case HorizontalCenter:
        point.x = (common_config.point.x + (common_config.size.width/2)) - (calculate_label_size(text_config).width/2);
        break;
    default:
        break;
    }

    switch (common_config.vertical_align)
    {
    case Bottom:
        point.y = (common_config.point.y + common_config.size.height) - calculate_label_size(text_config).height;
        break;
    case VerticalCenter:
        point.y = (common_config.point.y + (common_config.size.height/2)) - (calculate_label_size(text_config).height/2);
        break;
    default:
        break;
    }

    return point;
}

common_ui_config label(draw_ctx *ctx, text_ui_config text_config, common_ui_config common_config){
    gpu_point p = calculate_label_pos(text_config, common_config);
    fb_draw_string(ctx, text_config.text, p.x, p.y, text_config.font_size, common_config.foreground_color);
    return common_config;
}

common_ui_config textbox(draw_ctx *ctx, text_ui_config text_config, common_ui_config common_config){
    rectangle(ctx, (rect_ui_config){0,0}, common_config);
    label(ctx, text_config, common_config);
    return common_config;
}

common_ui_config rectangle(draw_ctx *ctx, rect_ui_config rect_config, common_ui_config common_config){
    if (rect_config.border_size > 0){
        fb_fill_rect(ctx, common_config.point.x, common_config.point.y, rect_config.border_size, common_config.size.height, rect_config.border_color);
        fb_fill_rect(ctx, common_config.point.x + common_config.size.width - rect_config.border_size, common_config.point.y, rect_config.border_size, common_config.size.height, rect_config.border_color);
        fb_fill_rect(ctx, common_config.point.x, common_config.point.y, common_config.size.width, rect_config.border_size, rect_config.border_color);
        fb_fill_rect(ctx, common_config.point.x, common_config.point.y  + common_config.size.height - rect_config.border_size, common_config.size.width, rect_config.border_size, rect_config.border_color);
    }
    fb_fill_rect(ctx, common_config.point.x + rect_config.border_size, common_config.point.y + rect_config.border_size, common_config.size.width - rect_config.border_size*2, common_config.size.height - rect_config.border_size*2, common_config.background_color);
    return (common_ui_config){
        .point = { common_config.point.x + rect_config.border_size, common_config.point.y + rect_config.border_size },
        .size = {common_config.size.width - rect_config.border_size*2, common_config.size.height - rect_config.border_size*2},
        .background_color = common_config.background_color,
        .foreground_color = common_config.foreground_color
    };
}