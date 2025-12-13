#include "query_string.h"
#include "std/string.h"
#include "syscalls/syscalls.h"

void query_parse(const char *buf, uint32_t len, QueryParam **out_params, uint32_t *out_count) {
    *out_params = 0;
    *out_count = 0;
    if (!buf || !len) return;

    uint32_t max_params = 1; //b query_parse

    for (uint32_t i = 0; i < len; i++) if (buf[i] == '&') max_params++;

    QueryParam *params = (QueryParam*)malloc(sizeof(QueryParam) * max_params);
    if (!params) return;

    uint32_t count = 0;
    uint32_t pos = 0;

    while (pos < len) {
        uint32_t seg_start = pos;
        while (pos < len && buf[pos] != '&') pos++;
        uint32_t seg_end = pos;

        if (seg_end > seg_start) {
            uint32_t eq_pos = seg_end;
            for (uint32_t i = seg_start; i < seg_end; i++) {
                if (buf[i] =='=') {
                    eq_pos = i;
                    break;
                }
            }

            QueryParam *qp = &params[count];

            if (eq_pos < seg_end) {
                qp->key.ptr = (uintptr_t)(buf + seg_start);
                qp->key.size = eq_pos - seg_start;
                qp->value.ptr = (uintptr_t)(buf + eq_pos + 1);
                qp->value.size = seg_end - (eq_pos + 1);
            } else {
                qp->key.ptr = (uintptr_t)(buf + seg_start);
                qp->key.size = seg_end - seg_start;
                qp->value.ptr = (uintptr_t)(buf + seg_end);
                qp->value.size = 0;
            }

            count++;
        }

        if (pos < len && buf[pos] == '&') pos++;
    }

    *out_params = params;
    *out_count = count;
}

void query_parse_z(const char *buf, QueryParam **out_params, uint32_t *out_count) {
    if (!buf) {
        *out_params = 0;
        *out_count = 0;
        return;
    }
    size_t n = strlen(buf);
    query_parse(buf, (uint32_t)n, out_params, out_count);
}
