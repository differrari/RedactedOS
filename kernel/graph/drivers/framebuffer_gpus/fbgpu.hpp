#pragma once 

#include "../gpu_driver.hpp"

class FBGPUDriver : public GPUDriver {
public:
    FBGPUDriver(){}

    void flush() override;

    void clear(color color) override;
    void draw_pixel(uint32_t x, uint32_t y, color color) override;
    void fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, color color) override;
    void draw_line(uint32_t x0, uint32_t y0, uint32_t x1,uint32_t y1, color color) override;
    void draw_char(uint32_t x, uint32_t y, char c, uint32_t scale, uint32_t color) override;
    void draw_string(string s, uint32_t x, uint32_t y, uint32_t scale, uint32_t color) override;
    uint32_t get_char_size(uint32_t scale) override;
    
    virtual void update_gpu_fb() = 0;

    draw_ctx* get_ctx() override;

    draw_ctx new_cursor(uint32_t color);
    void create_window(uint32_t x, uint32_t y, uint32_t width, uint32_t height, draw_ctx *ctx) override;
    void resize_window(uint32_t width, uint32_t height, draw_ctx *win_ctx) override;
    void setup_cursor() override;   
    void restore_below_cursor(); 
    void update_cursor(uint32_t x, uint32_t y, bool full) override;
    void set_cursor_pressed(bool pressed) override;

    ~FBGPUDriver() = default;
    
protected: 
    uint32_t* framebuffer;
    uint32_t* back_framebuffer;
    size_t framebuffer_size;
    gpu_size screen_size;
    uint32_t stride;
    void* mem_page;
    bool cursor_pressed, cursor_updated;
    draw_ctx ctx, cursor_unpressed_ctx, cursor_pressed_ctx;
    uint32_t* cursor_backup;//TODO: remove this, it leads to a crash if i do it right now (yes, again)
    uint32_t cursor_x, cursor_y;
};