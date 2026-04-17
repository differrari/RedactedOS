#pragma once

#include "utils/console.hpp"

class Terminal: public Console {
public:
    Terminal();
    void update();
protected:
    bool handle_input();
    void repeat_tick();
    void end_command();
    int prompt_length;
    void run_command();
    const char** parse_arguments(char *args, int *count);

    void redraw_input_line();
    void set_input_line(const char *s);
    void cursor_tick();
    void cursor_set_visible(bool visible);

    bool exec_cmd(const char *cmd, int argc, const char *args[]);

    draw_ctx* get_ctx() override;
    void flush(draw_ctx *ctx) override;
    bool screen_ready() override;

    bool command_running;

    static constexpr uint32_t input_max = 1024;
    char input_buf[input_max];
    uint32_t input_len;
    uint32_t input_cursor;

    static constexpr uint32_t history_max = 32;
    char *history[history_max];
    uint32_t history_len;
    uint32_t history_index;

    uint64_t last_blink_ms;
    bool cursor_visible;

    bool dirty;
};