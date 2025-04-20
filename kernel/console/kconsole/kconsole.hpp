#pragma once

// #include <cstdint>

class KernelConsole {
public:
    KernelConsole();

    void put_char(char c);
    void put_string(const char *str);
    void newline();
    void scroll();
    void clear();
    void resize();

private:
    bool check_ready();

    unsigned int cursor_x;
    unsigned int cursor_y;
    unsigned int columns;
    unsigned int rows;
    bool is_initialized = false;
    static constexpr int char_width = 8;
    static constexpr int char_height = 16;
    char buffer[100][100];
};
extern KernelConsole kconsole;