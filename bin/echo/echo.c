#include <stdio.h>
#include <string.h>
#include <stdbool.h>

void print_escaped(const char *str) {
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == '\\' && str[i + 1] != '\0') {
            switch (str[i + 1]) {
                case 'n': putchar('\n'); i++; break;
                case 't': putchar('\t'); i++; break;
                case '\\': putchar('\\'); i++; break;
                case 'r': putchar('\r'); i++; break;
                case 'a': putchar('\a'); i++; break;
                case 'b': putchar('\b'); i++; break;
                case 'v': putchar('\v'); i++; break;
                case 'f': putchar('\f'); i++; break;
                default: putchar(str[i]); break;
            }
        } else {
            putchar(str[i]);
        }
    }
}

int main(int argc, char *argv[]) {
    bool no_newline = false;
    bool interpret_escapes = false;
    int start = 1;

    // Parse options
    for (int i = 1; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            no_newline = true;
            start = i + 1;
        } else if (strcmp(argv[i], "-e") == 0) {
            interpret_escapes = true;
            start = i + 1;
        } else if (strcmp(argv[i], "-E") == 0) {
            interpret_escapes = false;
            start = i + 1;
        } else if (strcmp(argv[i], "--") == 0) {
            start = i + 1;
            break;
        } else {
            break;
        }
    }

    // arguments
    for (int i = start; i < argc; i++) {
        if (interpret_escapes) {
            print_escaped(argv[i]);
        } else {
            fputs(argv[i], stdout);
        }
        
        if (i < argc - 1) {
            putchar(' ');
        }
    }

    // newline unless -n was specified
    if (!no_newline) {
        putchar('\n');
    }

    return 0;
}