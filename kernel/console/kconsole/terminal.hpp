#pragma once

#include "kconsole.hpp"

class Terminal: public KernelConsole {
public:
    Terminal() : KernelConsole(){};
    void update();
protected:
    void handle_input();
    void end_command();
    void run_command();
    const char** parse_arguments(char *args, int *count);

    //TODO: proper commands
    bool exec_cmd(const char *cmd, int argc, const char *args[]);
    void TMP_test(int argc, const char *args[]);

    bool command_running;
};