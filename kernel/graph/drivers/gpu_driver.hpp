#include "types.h"
#include "ui/graphic_types.h"

class GPUDriver {
    bool init(gpu_size preferred_screen_size);

    void flush();

    void clear(color color);
    void draw_pixel(gpu_point p, color color);
    void draw_single_pixel(uint32_t x, uint32_t y, color color);
    void fill_rect(gpu_rect r, color color);
    void draw_line(gpu_point p0, gpu_point p1, color color);
    void draw_char(gpu_point p, char c, uint32_t scale, uint32_t color);
    void draw_single_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color);
    gpu_size get_screen_size();
    void draw_string(kstring s, gpu_point p, uint32_t scale, uint32_t color);
    uint32_t get_char_size(uint32_t scale);
};