#include "label.hpp"
#include "graph/graphics.h"
#include "console/kio.h"

Label::Label(){

}

void Label::render(){
    gpu_fill_rect(rect, background_color);
    gpu_draw_string(content, calculate_label_pos(), scale, text_color);
}

gpu_size Label::calculate_label_size(){
    int num_lines = 1;
    int num_chars = 0;
    int local_num_chars = 0;
    for (unsigned int i = 0; i < content.length; i++){
        if (content.data[i] == '\n'){
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
    unsigned int size = gpu_get_char_size(scale);
    return (gpu_size){size * num_chars, size * num_lines};
}

gpu_point Label::calculate_label_pos(){
    gpu_point point = rect.point;
    switch (horz_alignment)
    {
    case Trailing:
        point.x = (rect.point.x + rect.size.width) - calculate_label_size().width;
        break;
    case HorizontalCenter:
        point.x = (rect.point.x + (rect.size.width/2)) - (calculate_label_size().width/2);
        break;
    default:
        break;
    }

    switch (vert_alignment)
    {
    case Bottom:
        point.y = (rect.point.y + rect.size.height) - calculate_label_size().height;
        break;
    case VerticalCenter:
        point.y = (rect.point.y + (rect.size.height/2)) - (calculate_label_size().height/2);
        break;
    default:
        break;
    }

    return point;
}

void Label::set_bg_color(color bg){
    this->background_color = bg;
}

void Label::set_text_color(color txt){
    this->text_color = txt;
}

void Label::set_text(kstring text){
    this->content = text;
}

void Label::set_font_size(unsigned int size){
    this->scale = size;
}

void Label::set_alignment(HorizontalAlignment horizontal_alignment,VerticalAlignment vertical_alignment){
    horz_alignment = horizontal_alignment;
    vert_alignment = vertical_alignment;
}