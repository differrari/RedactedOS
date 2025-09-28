#include "utils/console.hpp"

class KernelConsole: public Console {
    draw_ctx* get_ctx() override;
    void flush(draw_ctx *ctx) override;
    bool screen_ready() override;
};