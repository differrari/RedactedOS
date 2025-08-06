#pragma once

#include "kconsole.hpp"

class Terminal: public KernelConsole {
public:
    void handle_input();

};