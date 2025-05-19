#pragma once

#include "UIElement.hpp"
#include "kstring.h"

enum HorizontalAlignment : int {
    Leading = 1 << 1,
    HorizontalCenter = 1 << 2,
    Trailing = 1 << 3,
};

enum VerticalAlignment : int {
    Top = 1 << 1, 
    Bottom = 1 << 2,
    VerticalCenter = 1 << 3
};  

class Label: UIElement {
public:
    Label();
    gpu_rect rect;
    void set_text(kstring text);
    void set_bg_color(color bg);
    void set_text_color(color txt);
    void set_font_size(unsigned int size);
    void set_alignment(HorizontalAlignment horizontal_alignment,VerticalAlignment vertical_alignment);
    void render();
private:
    kstring content;
    color background_color;
    color text_color;
    VerticalAlignment vert_alignment = VerticalAlignment::Top;
    HorizontalAlignment horz_alignment = HorizontalAlignment::Leading;
    gpu_point calculate_label_pos();
    gpu_size calculate_label_size();
    unsigned int scale = 1;
};