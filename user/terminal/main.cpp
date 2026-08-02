#include "terminal.hpp"
#include "string/string.h"

Terminal *term;

int main(int argc, char **argv){
    term = new Terminal();
    if (argc && strncmp_case(argv[0], "headless", true, 8) == 0){
        print("Terminal in headless mode");
        term->headless = true; 
    }
    while (1){
        term->update();
    }
}

void term_put_char(shell_handle *handle, char c){
    if (!term || (term->term_current_shell && term->term_current_shell != handle)) return;
    if (term->headless && c)
        serial_transmit(c);
    else 
        term->put_char(c);
}

void term_clear(shell_handle *handle){
    if (!term || (term->term_current_shell && term->term_current_shell != handle)) return;
    term->clear();
}

void term_flush(shell_handle *handle){
    if (!term || (term->term_current_shell && term->term_current_shell != handle)) return;
    term->refresh();
}

void term_bell(shell_handle *handle){
    if (!term || (term->term_current_shell && term->term_current_shell != handle)) return;
    term->bell();
}

void term_ascii_cmd(shell_handle *handle, char cmd, u16 proc_id){
    if (!term || (term->term_current_shell && term->term_current_shell != handle)) return;
    term->interpret_cmd_code(cmd, proc_id);
}

void term_console_ctrl(shell_handle *handle, console_ctrls ctrl){
    if (!term || (term->term_current_shell && term->term_current_shell != handle)) return;
    term->ctrl(ctrl);
}

void term_emit_data(structdef field, sizedptr data, bool is_allocated){
    if (!term) return;
    term->emit_data(field,data,is_allocated);
}