#pragma once

#include "utils/console.hpp"

class Terminal: public Console {
public:
    Terminal();
    void update();
protected:
    void handle_input();
    void end_command();
    int prompt_length;
    void run_command();
    const char** parse_arguments(char *args, int *count);

    bool exec_cmd(const char *cmd, int argc, const char *args[]);
    void TMP_test(int argc, const char *args[]);

    draw_ctx* get_ctx() override;
    void flush(draw_ctx *ctx) override;
    bool screen_ready() override;

    bool command_running;
};
