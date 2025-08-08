#pragma once

#include "kconsole.hpp"

class Terminal: public KernelConsole {
public:
    void update();
protected:
    void handle_input();
    void end_command();
    void run_command();

    //TODO: proper commands
    void TMP_cat(const char *args);
    void TMP_test(const char *args);

    bool command_running;
};