#include "percent.h"
#include "std/string.h"

bool percent_decode(sizedptr in, string *out) {
    *out = string_repeat('\0', 0);
    const char *buf = (const char*)in.ptr;
    uint32_t len = in.size;

    uint32_t i = 0;
    while (i < len) {
        char c = buf[i];

        if (c == '%' && i + 2 < len) {
            int h1 = hex_val(buf[i+1]);
            int h2 = hex_val(buf[i+2]);
            if (h1 < 0 || h2 < 0) return false;

            char decoded =(char)((h1 << 4) | h2);
            string_append_bytes(out, &decoded, 1);
            i += 3;
            continue;
        }

        if (c == '+') {
            char sp = ' ';
            string_append_bytes(out, &sp, 1);
            i++;
            continue;
        }

        string_append_bytes(out, &c, 1);
        i++;
    }

    return true;
}
