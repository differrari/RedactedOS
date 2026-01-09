#include "std/string.h"
#include "syscalls/syscalls.h"
#include "std/memory.h"
#include "types.h"
#include "string_slice.h"

#define TRUNC_MARKER "[…]"

//TODO move these in a dedicated helper file
static inline void append_char(char **p, size_t *rem, char c, int *truncated) {
    if (*rem > 1) {
        **p = c;
        (*p)++;
        (*rem)--;
    } else {
        *truncated = 1;
    }
}

static inline void append_strn(char **p, size_t *rem, const char *s, size_t n, int *truncated) {
    for (size_t i = 0; i < n; i++) {
        append_char(p, rem, s[i], truncated);
        if (*truncated) break;
    }
}

static inline void append_block(char **p, size_t *rem, const char *s, size_t n, int *truncated) {
    if (n == 0) return;
    if (*rem <= 1) {
        *truncated = 1;
        return;
    }
    size_t can = *rem - 1;
    size_t w = (n <= can) ? n : can;
    memcpy(*p, s, w);
    *p += w;
    *rem -= w;
    if (w < n) *truncated = 1;
}

static inline void append_repeat(char **p, size_t *rem, char c, size_t n, int *truncated) {
    if (n == 0) return;
    if (*rem <= 1) {
        *truncated = 1;
        return;
    }
    size_t can = *rem - 1;
    size_t w = (n <= can) ? n : can;

    if (w <= 16) {
        for (size_t i = 0; i < w; ++i) (*p)[i] = c;
        *p += w;
    } else {
        char blk[32];
        for (size_t i = 0; i < sizeof(blk); ++i) blk[i] = c;
        size_t t = w;
        while (t >= 32) {
            memcpy(*p, blk, 32);
            *p += 32;
            t -= 32;
        }
        if (t >= 16) {
            memcpy(*p, blk, 16);
            *p += 16;
            t -= 16;
        }
        if (t) {
            memcpy(*p, blk, t); 
            *p += t; 
        }
    }

    *rem -= w;
    if (w < n) *truncated = 1;
}

static inline uint32_t u64_to_dec(char *tmp, uint64_t v) {
    uint32_t parts[3];
    uint32_t np = 0;
    while (v >= 1000000000ULL) {
        uint64_t q = v / 1000000000ULL;
        uint32_t r = (uint32_t)(v - q * 1000000000ULL);
        parts[np++] = r;
        v = q;
    }
    uint32_t n = 0;
    uint32_t h = (uint32_t)v;
    do {
        tmp[n++] = (char)('0' + (h % 10));
        h /= 10;
    } while (h);

    memreverse(tmp, n);
    while (np) {
        uint32_t x = parts[--np];
        char buf9[9];
        for (int i = 8; i >= 0; --i) {
            buf9[i] = (char)('0' + (x % 10));
            x /= 10;
        }
        memcpy(tmp + n, buf9, 9);
        n += 9;
    }
    return n;
}

uint32_t u64_to_base(char *tmp, uint64_t v, unsigned base, int upper) {
    const char *hx = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    uint32_t n = 0;

    if (base == 16) {
        int started = 0;
        for (int i = 60; i >= 0; i -= 4) {
            uint8_t nib = (uint8_t)((v >> i) & 0xF);
            if (!started) {
                if (!nib) continue;
                started = 1;
            }
            tmp[n++] = hx[nib];
        }
        if (!n) tmp[n++] = '0';
        return n;
    } else if (base == 8) {
        int started = 0;
        for (int i = 63; i >= 0; i -= 3) {
            uint8_t tri = (uint8_t)((v >> i) & 0x7);
            if (!started) {
                if (!tri) continue;
                started = 1;
            }
            tmp[n++] = (char)('0' + tri);
        }
        if (!n) tmp[n++] = '0';
        return n;
    } else if (base == 2) {
        int started = 0;
        for (int i = 63; i >= 0; --i) {
            uint8_t bit = (uint8_t)((v >> i) & 1u);
            if (!started) {
                if (!bit) continue;
                started = 1;
            }
            tmp[n++] = (char)('0' + bit);
        }
        if (!n) tmp[n++] = '0';
        return n;
    }

    do {
        tmp[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v && n < 65);
    memreverse(tmp, n);
    return n;
}

static inline void emit_padded(char **restrict p, size_t *restrict rem,
                               const char *restrict buf, uint32_t len,
                               int width, int flag_minus, int *restrict truncated) {
    if (width < 0) width = 0;
    uint32_t pad = (width > (int)len) ? (uint32_t)width - len : 0;
    if (pad > (uint32_t)2147483647) pad = (uint32_t)2147483647;
    if (!flag_minus) append_repeat(p, rem, ' ', pad, truncated);
    append_block(p, rem, buf, len, truncated);
    if (flag_minus) append_repeat(p, rem, ' ', pad, truncated);
}

size_t strlen_max(const char *s, uint32_t max_length){
    if (s == NULL) return 0;
    
    size_t len = 0;
    while ((max_length == 0 || len < max_length) && s[len]) len++;
    
    return len;
}

string string_from_literal(const char *literal){
    if (literal == NULL) return (string){ .data = NULL, .length = 0, .mem_length = 0};
    
    uint32_t len = strlen(literal);
    char *buf = (char*)malloc(len + 1);
    if (!buf) return (string){ .data = NULL, .length = 0, .mem_length = 0 };

    for (uint32_t i = 0; i < len; i++) buf[i] = literal[i];

    buf[len] = '\0';
    return (string){ .data = buf, .length = len, .mem_length = len + 1 };
}

string string_repeat(char symbol, uint32_t amount){
    char *buf = (char*)malloc(amount + 1);
    if (!buf) return (string){0};
    memset(buf, symbol, amount);
    buf[amount] = 0;
    return (string){ .data = buf, .length = amount, .mem_length = amount+1 };
}

string string_tail(const char *array, uint32_t max_length){
    
    if (array == NULL) return (string){ .data = NULL, .length = 0, .mem_length = 0 };

    uint32_t len = strlen(array);
    int offset = (int)len - (int)max_length;
    if (offset < 0) offset = 0;
    
    uint32_t adjusted_len = len - offset;
    char *buf = (char*)malloc(adjusted_len + 1);
    if (!buf) return (string){ .data = NULL, .length = 0, .mem_length = 0 };

    for (uint32_t i = 0; i < adjusted_len; i++) buf[i] = array[offset + i];
    
    buf[adjusted_len] = '\0';
    return (string){.data = buf, .length = adjusted_len, .mem_length = adjusted_len + 1 };
}

string string_from_literal_length(const char *array, uint32_t max_length){
    if (array == NULL) return (string){.data = NULL, .length = 0, .mem_length= 0 };

    uint32_t len = strlen_max(array, max_length);
    char *buf = (char*)malloc(len + 1);
    if(!buf) return (string){ .data = NULL, .length = 0, .mem_length=0 };

    for (uint32_t i = 0; i < len; i++) buf[i] = array[i];

    buf[len] = '\0';
    return (string){ .data = buf, .length = len, .mem_length = len+1};
}

string string_from_char(const char c){
    char *buf = (char*)malloc(2);
    if (!buf) return (string){0};
    buf[0] = c;
    buf[1] = 0;
    return (string){.data = buf, .length = 1, .mem_length = 2};
}

uint32_t parse_hex(uint64_t value, char* buf){
    uint32_t len = 0;
    buf[len++] = '0';
    buf[len++] = 'x';
    bool started = false;
    for (uint32_t i = 60;; i -= 4) {
        uint8_t nibble = (value >> i) & 0xF;
        char curr_char = nibble < 10 ? '0' + nibble : 'A' + (nibble - 10);
        if (started || curr_char != '0' || i == 0) {
            started = true;
            buf[len++] = curr_char;
        }
        
        if (i == 0) break;
    }

    buf[len] = 0;
    return len;
}

string string_from_hex(uint64_t value){
    char *buf = (char*)malloc(18);
    if (!buf) return (string){0};
    uint32_t len = parse_hex(value, buf);
    return (string){ .data = buf, .length = len, .mem_length = 18 };
}

uint32_t parse_bin(uint64_t value, char* buf){
    uint32_t len = 0;
    buf[len++] = '0';
    buf[len++] = 'b';
    bool started = false;
    for (uint32_t i = 63;; i --){
        char bit = (value >> i) & 1  ? '1' : '0';
        if (started || bit != '0' || i == 0){
            started = true;
            buf[len++] = bit;
        }
        
        if (i == 0) break;
    }

    buf[len] = 0;
    return len;
}

string string_from_bin(uint64_t value){
    char *buf = (char*)malloc(66);
    if (!buf) return (string){0};
    uint32_t len = parse_bin(value, buf);
    return (string){ .data = buf, .length = len, .mem_length = 66 };
}

bool string_equals(string a, string b){
    return strcmp(a.data,b.data) == 0;
}

string string_replace(const char *str, char orig, char repl){
    size_t str_size = strlen(str);
    char *buf = (char*)malloc(str_size+1);
    for (size_t i = 0; i < str_size && str[i]; i++){
        buf[i] = str[i] == orig ? repl : str[i];
    }
    buf[str_size] = 0;
    return (string){ .data = buf, .length = str_size, .mem_length = str_size + 1};
}

string string_format(const char *fmt, ...){
    if (fmt == NULL) return (string){ .data = NULL, .length = 0, .mem_length = 0};

    __attribute__((aligned(16))) va_list args;
    va_start(args, fmt);
    string result = string_format_va(fmt, args);
    va_end(args);
    return result;
}

string string_format_va(const char *fmt, va_list args){
    char *buf = (char*)malloc(STRING_MAX_LEN);
    if (!buf) return (string){0};
    size_t len = string_format_va_buf(fmt, buf, STRING_MAX_LEN, args);
    return (string){ .data = buf, .length = len, .mem_length = STRING_MAX_LEN };
}

size_t string_format_buf(char *out, size_t cap, const char *fmt, ...){
    __attribute__((aligned(16))) va_list args;
    va_start(args, fmt);
    size_t size = string_format_va_buf(fmt, out, cap, args);
    va_end(args);
    return size;
}

size_t string_format_va_buf(const char *restrict fmt, char *restrict out, size_t cap, va_list args) {
    char *p = out;
    size_t rem = cap ? cap : 1;
    int truncated_all = 0;

    for (uint32_t i = 0; fmt && fmt[i] && rem > 1;) {
        if (fmt[i] != '%') {
            uint32_t j = i;
            while (fmt[j] && fmt[j] != '%') j++;
            append_block(&p, &rem, fmt + i, (uint32_t)(j - i), &truncated_all);
            i = j;
            continue;
        }

        i++;

        unsigned flags = 0;
        for (;;) {
            char c = fmt[i];
            int done = 0;
            switch (c) {
                case '-': flags |= 1u << 0; i++; break;
                case '+': flags |= 1u << 1; i++; break;
                case ' ': flags |= 1u << 2; i++; break;
                case '0': flags |= 1u << 3; i++; break;
                case '#': flags |= 1u << 4; i++; break;
                default: done = 1; break;
            }
            if (done) break;
        }
        int flag_minus = (flags & (1u << 0)) != 0;
        int flag_plus = (flags & (1u << 1)) != 0;
        int flag_space = (flags & (1u << 2)) != 0;
        int flag_zero = (flags & (1u << 3)) != 0;
        int flag_hash = (flags & (1u << 4)) != 0;

        int width = 0, width_star = 0;
        if (fmt[i] == '*') {
            width_star = 1;
            i++;
        } else {
            while (fmt[i] >= '0' && fmt[i] <= '9') {
                width = width * 10 + (fmt[i] - '0');
                i++;
            }
        }

        int precision_set = 0, precision = 0;
        if (fmt[i] == '.') {
            i++;
            precision_set = 1;
            if (fmt[i] == '*') {
                precision = va_arg(args, int);
                i++;
            } else {
                while (fmt[i] >= '0' && fmt[i] <= '9') {
                    precision = precision * 10 + (fmt[i] - '0');
                    i++;
                }
            }
        }
        if (precision_set && precision < 0) precision_set = 0;
        int had_precision = precision_set;

        enum { LEN_DEF, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_Z, LEN_T, LEN_J } len = LEN_DEF;
        switch (fmt[i]) {
            case 'h': if (fmt[i + 1] == 'h') { len = LEN_HH; i += 2; } else { len = LEN_H; i++; } break;
            case 'l': if (fmt[i + 1] == 'l') { len = LEN_LL; i += 2; } else { len = LEN_L; i++; } break;
            case 'z': len = LEN_Z; i++; break;
            case 't': len = LEN_T; i++; break;
            case 'j': len = LEN_J; i++; break;
            default: break;
        }

        if (width_star) {
            width = va_arg(args, int);
            if (width < 0) {
                flag_minus = 1;
                width = -width;
            }
        }
        if (!fmt[i]) {
            append_char(&p, &rem, '%', &truncated_all);
            break;
        }
        char spec = fmt[i++];

        if (spec == '%') {
            uint32_t pad = (width > 1) ? (uint32_t)(width - 1) : 0;
            if (pad > (uint32_t)2147483647) pad = (uint32_t)2147483647;
            if (!flag_minus) append_repeat(&p, &rem, ' ', pad, &truncated_all);
            append_char(&p, &rem, '%', &truncated_all);
            if (flag_minus) append_repeat(&p, &rem, ' ', pad, &truncated_all);
            continue;
        }

        if (!flag_plus && !flag_space && !flag_zero && !flag_hash && !had_precision && width == 0) {
            if (spec == 's') {
                const char *s = va_arg(args, char *); if (!s) s = "(null)";
                append_block(&p, &rem, s, strlen(s), &truncated_all);
                continue;
            } else if (spec == 'c') {
                int ch = va_arg(args, int);
                append_char(&p, &rem, (char)ch, &truncated_all);
                continue;
            }
        }

        char numtmp[66];
        char sbuf[256];
        const char *obuf = sbuf;
        uint32_t outlen = 0;
        int is_num = 0;
        int negative = 0;
        uint32_t k = 0;

        switch (spec) {
            case 'c': {
                int ch = va_arg(args, int);
                char one = (char)ch;
                emit_padded(&p, &rem, &one, 1, width, flag_minus, &truncated_all);
            } continue;

            case 's': {
                const char *s = va_arg(args, char *);
                if (!s) s = "(null)";
                uint32_t sl = strlen(s);
                if (precision_set && (uint32_t)precision < sl) sl = (uint32_t)precision;
                emit_padded(&p, &rem, s, sl, width, flag_minus, &truncated_all);
            } continue;

            case 'S': {
                const string sv = va_arg(args, string);
                const char *s = sv.data ? sv.data : "(null)";
                uint32_t sl = sv.data ? sv.length : 6;
                if (precision_set && (uint32_t)precision < sl) sl = (uint32_t)precision;
                emit_padded(&p, &rem, s, sl, width, flag_minus, &truncated_all);
            } continue;
                
            case 'v': {
                const string_slice sv = va_arg(args, string_slice);
                append_strn(&p, &rem, sv.data, sv.length, &truncated_all);
            } continue;

            case 'p': {
                uintptr_t v = (uintptr_t)va_arg(args, void *);
                uint64_t x = (uint64_t)v;
                for (int nib = 15; nib >= 0; --nib) sbuf[15 - nib] = "0123456789abcdef"[(x >> (nib * 4)) & 0xF];
                obuf = sbuf; outlen = 16; is_num = 1;
            } break;

            case 'b': case 'o': case 'u': case 'x': case 'X': case 'd': case 'i': {
                int base = 10;
                switch (spec) {
                    case 'b': base = 2; break;
                    case 'o': base = 8; break;
                    case 'x': case 'X': base = 16; break;
                    default: base = 10; break;
                }
                int upper = (spec == 'X');
                int is_signed = (spec == 'd' || spec == 'i');
                uint64_t u = 0;

                if (is_signed) {
                    int64_t sv;
                    switch (len) {
                        case LEN_HH: sv = (signed char)va_arg(args, int); break;
                        case LEN_H: sv = (short)va_arg(args, int); break;
                        case LEN_L: sv = va_arg(args, long); break;
                        case LEN_LL: sv = va_arg(args, long long); break;
                        case LEN_Z: sv = (long long)va_arg(args, size_t); break;
                        case LEN_T: sv = (long long)va_arg(args, intptr_t); break;
                        case LEN_J: sv = va_arg(args, int64_t); break;
                        default: sv = va_arg(args, int); break;
                    }
                    if (base == 10) {
                        char dtmp[32];
                        negative = (sv < 0);
                        uint64_t mag = negative ? (uint64_t)(-(sv + 1)) + 1 : (uint64_t)sv;
                        uint32_t dn = u64_to_dec(dtmp, mag);
                        obuf = dtmp;
                        outlen = dn;
                    } else {
                        u = (uint64_t)sv;
                        uint32_t n = u64_to_base(numtmp, u, (unsigned)base, upper);
                        obuf = numtmp;
                        outlen = n;
                        negative = (sv < 0);
                    }
                } else {
                    switch (len) {
                        case LEN_HH: u = (unsigned char)va_arg(args, int); break;
                        case LEN_H: u = (unsigned short)va_arg(args, int); break;
                        case LEN_L: u = va_arg(args, unsigned long); break;
                        case LEN_LL: u = va_arg(args, unsigned long long); break;
                        case LEN_Z: u = (uint64_t)va_arg(args, size_t); break;
                        case LEN_T: u = (uint64_t)va_arg(args, uintptr_t); break;
                        case LEN_J: u = (uint64_t)va_arg(args, uint64_t); break;
                        default: u = va_arg(args, unsigned int); break;
                    }
                    if (base == 10) {
                        char dtmp[32];
                        uint32_t dn = u64_to_dec(dtmp, u);
                        obuf = dtmp;
                        outlen = dn;
                    } else {
                        uint32_t n = u64_to_base(numtmp, u, (unsigned)base, upper);
                        obuf = numtmp;
                        outlen = n;
                    }
                }

                if (precision_set && (uint32_t)precision == 0 && outlen == 1 && obuf[0] == '0') outlen = 0;
                is_num = 1;
            } break;

            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                double dv = va_arg(args, double);
                int upper = (spec == 'F' || spec == 'E' || spec == 'G' || spec == 'A');

                uint64_t bits; memcpy(&bits, &dv, sizeof(bits));
                int is_nan = ((bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) && (bits & 0x000FFFFFFFFFFFFFULL);
                int is_inf = ((bits & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL);
                int is_zero = ((bits << 1) == 0);
                int signbit = (int)((bits >> 63) & 1);
                negative = signbit;
                if (negative) dv = -dv;

                if (is_nan) {
                    if (upper) {
                        sbuf[0] = 'N';
                        sbuf[1] = 'A';
                        sbuf[2] = 'N';
                    }else {
                        sbuf[0] = 'n';
                        sbuf[1] = 'a';
                        sbuf[2] = 'n';
                    }
                    obuf = sbuf;
                    outlen = 3;
                    is_num = 1;
                    break;
                } else if (is_inf) {
                    if (upper) {
                        sbuf[0] = 'I';
                        sbuf[1] = 'N';
                        sbuf[2] = 'F';
                    } else {
                        sbuf[0] = 'i';
                        sbuf[1] = 'n';
                        sbuf[2] = 'f';
                    }
                    obuf = sbuf;
                    outlen = 3;
                    is_num = 1;
                    break;
                }

                if (!precision_set) precision = 6;
                if (precision < 0) precision = 0;

                if (spec == 'a' || spec == 'A') {
                    int neg_local = (int)((bits >> 63) & 1);
                    uint64_t exp = (bits >> 52) & 0x7FFULL;
                    uint64_t frac = (bits & 0xFFFFFFFFFFFFFULL);
                    negative = neg_local;

                    if ((exp | frac) == 0) {
                        k = 0;
                        sbuf[k++] = '0';
                        sbuf[k++] = 'x';
                        sbuf[k++] = '0';
                        if (precision > 0) {
                            sbuf[k++] = '.';
                            for (int z = 0; z < precision && k < sizeof(sbuf); z++) sbuf[k++] = '0';
                        } else if (flag_hash) sbuf[k++] = '.';
                        sbuf[k++] = upper ? 'P' : 'p';
                        sbuf[k++] = '+';
                        sbuf[k++] = '0';
                        obuf = sbuf;
                        outlen = k;
                    } else {
                        int e = 0;
                        uint64_t mant = 0;
                        if (exp == 0) {
                            e = -1022;
                            mant = frac;
                            while (mant && (mant & (1ULL << 52)) == 0) {
                                mant <<= 1;
                                e--;
                            }
                        } else {
                            e = (int)exp - 1023;
                            mant = (1ULL << 52) | frac;
                        }

                        uint64_t hex_frac = mant & ((1ULL << 52) - 1);
                        k = 0;
                        sbuf[k++] = '0';
                        sbuf[k++] = 'x';
                        sbuf[k++] = '1';
                        if (precision > 0) {
                            sbuf[k++] = '.';
                            for (int d = 0; d < precision && k < sizeof(sbuf); d++) {
                                uint8_t nib = (uint8_t)((hex_frac >> (52 - 4 - 4 * d)) & 0xF);
                                sbuf[k++] = upper ? "0123456789ABCDEF"[nib] : "0123456789abcdef"[nib];
                            }
                        } else if (flag_hash) sbuf[k++] = '.';
                        sbuf[k++] = upper ? 'P' : 'p';
                        sbuf[k++] = (e >= 0) ? '+' : '-';
                        char etmp[32];
                        uint32_t en = u64_to_dec(etmp, (uint64_t)(e >= 0 ? e : -e));
                        for (uint32_t t = 0; t < en && k < sizeof(sbuf); t++) sbuf[k++] = etmp[t];
                        obuf = sbuf;
                        outlen = k;
                    }
                    is_num = 1;
                } else if (spec == 'f' || spec == 'F') {
                    uint64_t scale = 1;
                    int prec = precision;
                    if (prec > 9) prec = 9;
                    for (int d = 0; d < prec; d++) scale *= 10ull;
                    uint64_t whole = (uint64_t)dv;
                    double fr = dv - (double)whole;
                    uint64_t F = 0;
                    if (prec > 0) {
                        double x = fr * (double)scale;
                        uint64_t xi = (uint64_t)x;
                        double fp = x - (double)xi;
                        const double eps = 1e-12;
                        if (fp > 0.5 + eps || (fp > 0.5 - eps && fp < 0.5 + eps && (xi & 1))) xi++;
                        F = xi;
                    }
                    if (prec > 0 && F >= scale) {
                        whole += 1;
                        F -= scale;
                    }

                    char dtmp[32];
                    uint32_t dn = u64_to_dec(dtmp, whole);
                    k = 0;
                    for (uint32_t t0 = 0; t0 < dn && k < sizeof(sbuf); t0++) sbuf[k++] = dtmp[t0];
                    if ((precision > 0 || flag_hash) && k < sizeof(sbuf)) sbuf[k++] = '.';
                    if (precision > 0) {
                        char ftmp[16];
                        for (int d = prec - 1; d >= 0; d--) {
                            ftmp[d] = (char)('0' + (F % 10));
                            F /= 10;
                        }
                        for (int d = 0; d < prec && k < sizeof(sbuf); d++) sbuf[k++] = ftmp[d];
                    }
                    obuf = sbuf;
                    outlen = k;
                    is_num = 1;
                } else {
                    if (is_zero) {
                        if (spec == 'e' || spec == 'E') {
                            int prec = precision;
                            if (prec > 9) prec = 9;
                            k = 0; sbuf[k++] = '0';
                            if ((precision > 0 || flag_hash) && k < sizeof(sbuf)) sbuf[k++] = '.';
                            for (int d = 0; d < prec && k < sizeof(sbuf); d++) sbuf[k++] = '0';
                            sbuf[k++] = (spec == 'E' ? 'E' : 'e');
                            sbuf[k++] = '+';
                            sbuf[k++] = '0';
                            sbuf[k++] = '0';
                            obuf = sbuf;
                            outlen = k;
                            is_num = 1;
                        } else if (spec == 'g' || spec == 'G') {
                            int p_sig = precision == 0 ? 1 : precision;
                            if (!flag_hash) {
                                sbuf[0] = '0';
                                k = 1;
                            } else {
                                sbuf[0] = '0';
                                sbuf[1] = '.';
                                k = 2;
                                for (int d = 1; d < p_sig && k < sizeof(sbuf); d++) sbuf[k++] = '0';
                            }
                            obuf = sbuf;
                            outlen = k;
                            is_num = 1;
                        }
                        break;
                    }

                    int bexp = (int)((bits >> 52) & 0x7FFULL) - 1023;
                    int dec_est = (int)((((int64_t)bexp) * 1233) >> 12);
                    int aexp = dec_est < 0 ? -dec_est : dec_est;
                    double base = 10.0, pow10 = 1.0, acc = 1.0;
                    int t = aexp;
                    while (t) {
                        if (t & 1) acc *= base;
                        base *= base; t >>= 1;
                    }
                    pow10 = acc;
                    double m = dv;
                    if (dec_est > 0) m /= pow10;
                    else if (dec_est < 0) m *= pow10;
                    int exp10 = dec_est;
                    if (m >= 10.0) {
                        m /= 10.0;
                        exp10++;
                    }
                    else if (m < 1.0) {
                        m *= 10.0;
                        exp10--;
                    }

                    if (spec == 'e' || spec == 'E') {
                        int prec = precision;
                        if (prec > 9) prec = 9;
                        uint64_t scale = 1;
                        for (int d = 0; d < prec; d++) scale *= 10ull;
                        uint64_t W = (uint64_t)m;
                        double fr = m - (double)W;
                        uint64_t F = 0;
                        if (prec > 0) {
                            double x = fr * (double)scale;
                            uint64_t xi = (uint64_t)x;
                            double fp = x - (double)xi;
                            const double eps = 1e-12;
                            if (fp > 0.5 + eps || (fp > 0.5 - eps && fp < 0.5 + eps && (xi & 1))) xi++;
                            F = xi;
                        }
                        if (prec > 0 && F >= scale) {
                            W += 1;
                            F -= scale;
                            if (W >= 10) {
                                W = 1;
                                exp10++;
                            }
                        }
                        char dtmp[32];
                        uint32_t dn = u64_to_dec(dtmp, W);
                        k = 0;
                        for (uint32_t t0 = 0; t0 < dn && k < sizeof(sbuf); t0++) sbuf[k++] = dtmp[t0];
                        if ((precision > 0 || flag_hash) && k < sizeof(sbuf)) sbuf[k++] = '.';
                        if (precision > 0) {
                            char ftmp[16];
                            for (int d = prec - 1; d >= 0; d--) {
                                ftmp[d] = (char)('0' + (F % 10));
                                F /= 10;
                            }
                            for (int d = 0; d < prec && k < sizeof(sbuf); d++) sbuf[k++] = ftmp[d];
                        }
                        sbuf[k++] = (spec == 'E' ? 'E' : 'e');
                        sbuf[k++] = (exp10 >= 0) ? '+' : '-';
                        uint32_t aexp2 = (uint32_t)(exp10 >= 0 ? exp10 : -exp10);
                        char etmp[32];
                        uint32_t en = u64_to_dec(etmp, aexp2);
                        if (en < 2) sbuf[k++] = '0';
                        for (uint32_t t0 = 0; t0 < en && k < sizeof(sbuf); t0++) sbuf[k++] = etmp[t0];
                        obuf = sbuf;
                        outlen = k;
                        is_num = 1;
                    } else if (spec == 'g' || spec == 'G') {
                        int p_sig = precision == 0 ? 1 : precision;
                        int use_e = (exp10 < -4 || exp10 >= p_sig);

                        if (use_e) {
                            int prc = p_sig - 1;
                            if (!precision_set) prc = 6 - 1;
                            if (prc < 0) prc = 0;
                            if (prc > 9) prc = 9;
                            uint64_t scale = 1;
                            for (int d = 0; d < prc; d++) scale *= 10ull;
                            uint64_t W = (uint64_t)m;
                            double fr = m - (double)W;
                            uint64_t F = 0;
                            if (prc > 0) {
                                double x = fr * (double)scale;
                                uint64_t xi = (uint64_t)x;
                                double fp = x - (double)xi;
                                const double eps = 1e-12;
                                if (fp > 0.5 + eps || (fp > 0.5 - eps && fp < 0.5 + eps && (xi & 1))) xi++;
                                F = xi;
                            }
                            if (prc > 0 && F >= scale) {
                                W += 1;
                                F -= scale;
                                if (W >= 10) {
                                    W = 1;
                                    exp10++;
                                }
                            }
                            char dtmp[32];
                            uint32_t dn = u64_to_dec(dtmp, W);
                            k = 0;
                            for (uint32_t t0 = 0; t0 < dn && k < sizeof(sbuf); t0++) sbuf[k++] = dtmp[t0];
                            if ((prc > 0 || flag_hash) && k < sizeof(sbuf)) sbuf[k++] = '.';
                            if (prc > 0) {
                                char ftmp[16];
                                for (int d = prc - 1; d >= 0; d--) {
                                    ftmp[d] = (char)('0' + (F % 10));
                                    F /= 10;
                                }
                                for (int d = 0; d < prc && k < sizeof(sbuf); d++) sbuf[k++] = ftmp[d];
                            }
                            if (!flag_hash && prc > 0) {
                                while (k > 0 && sbuf[k - 1] == '0') k--;
                                if (k > 0 && sbuf[k - 1] == '.') k--;
                            }
                            sbuf[k++] = (spec == 'G' ? 'E' : 'e');
                            sbuf[k++] = (exp10 >= 0) ? '+' : '-';
                            uint32_t aexp2 = (uint32_t)(exp10 >= 0 ? exp10 : -exp10);
                            char etmp[32];
                            uint32_t en = u64_to_dec(etmp, aexp2);
                            if (en < 2) sbuf[k++] = '0';
                            for (uint32_t t0 = 0; t0 < en && k < sizeof(sbuf); t0++) sbuf[k++] = etmp[t0];
                            obuf = sbuf;
                            outlen = k;
                            is_num = 1;
                        } else {
                            int pr = p_sig - 1 - exp10;
                            if (pr < 0) pr = 0;
                            if (!precision_set) pr = 6;
                            if (pr > 9) pr = 9;
                            uint64_t scale = 1;
                            for (int d = 0; d < pr; d++) scale *= 10ull;
                            uint64_t whole = (uint64_t)dv;
                            double fr = dv - (double)whole;
                            uint64_t F = 0;
                            if (pr > 0) {
                                double x = fr * (double)scale;
                                uint64_t xi = (uint64_t)x;
                                double fp = x - (double)xi;
                                const double eps = 1e-12;
                                if (fp > 0.5 + eps || (fp > 0.5 - eps && fp < 0.5 + eps && (xi & 1))) xi++;
                                F = xi;
                            }
                            if (pr > 0 && F >= scale) {
                                whole += 1;
                                F -= scale;
                            }
                            char dtmp[32];
                            uint32_t dn = u64_to_dec(dtmp, whole);
                            k = 0;
                            for (uint32_t t0 = 0; t0 < dn && k < sizeof(sbuf); t0++) sbuf[k++] = dtmp[t0];
                            if ((pr > 0 || flag_hash) && k < sizeof(sbuf)) sbuf[k++] = '.';
                            if (pr > 0) {
                                char ftmp[16];
                                for (int d = pr - 1; d >= 0; d--) {
                                    ftmp[d] = (char)('0' + (F % 10));
                                    F /= 10;
                                }
                                for (int d = 0; d < pr && k < sizeof(sbuf); d++) sbuf[k++] = ftmp[d];
                            }
                            if (!flag_hash && pr > 0)
                            {
                                while (k > 0 && sbuf[k - 1] == '0') k--;
                                if (k > 0 && sbuf[k - 1] == '.') k--;
                            }
                            obuf = sbuf;
                            outlen = k;
                            is_num = 1;
                        }
                    }
                }
            } break;

            default:
                append_char(&p, &rem, '%', &truncated_all);
                append_char(&p, &rem, spec, &truncated_all);
                continue;
        }

        uint32_t zero_prec = 0, zero_width = 0, left_spaces = 0, right_spaces = 0;
        int need_sign = 0;
        char signch = 0;

        int allow_sign = (spec == 'd' || spec == 'i' || spec == 'f' || spec == 'F' || spec == 'e' || spec == 'E' || spec == 'g' || spec == 'G' || spec == 'a' || spec == 'A');
        if (is_num && allow_sign) {
            if (negative) need_sign = 1, signch = '-';
            else if (flag_plus) need_sign = 1, signch = '+';
            else if (flag_space) need_sign = 1, signch = ' ';
        }

        const char *prefix = NULL;
        uint32_t plen = 0;
        int is_int_spec = (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'o' || spec == 'x' || spec == 'X' || spec == 'b' || spec == 'p');

        if (is_int_spec && spec != 'p') {
            if (had_precision && (uint32_t)precision > outlen) zero_prec = (uint32_t)precision - outlen;
        }
        if (spec == 'p') {
            prefix = "0x";
            plen = 2;
        }
        else switch (spec) {
            case 'b': if (!flag_hash && outlen > 0) {
                prefix = "0b";
                plen = 2;
            } break;
            case 'x': if (!flag_hash && !(outlen == 0 || (outlen == 1 && obuf[0] == '0'))) {
                prefix = "0x";
                plen = 2;
            } break;
            case 'X': if (!flag_hash && !(outlen == 0 || (outlen == 1 && obuf[0] == '0'))) {
                prefix = "0X";
                plen = 2;
            } break;
            case 'o':
                if (flag_hash) {
                    if (outlen == 0) {
                        prefix = "0";
                        plen = 1;
                    }
                    else if (zero_prec == 0 && obuf[0] != '0') {
                        prefix = "0";
                        plen = 1;
                    }
                }
                break;
            default: break;
        }

        int pad_zero = 0;
        if (!flag_minus) pad_zero = (!is_int_spec ? flag_zero : (flag_zero && !had_precision));

        uint32_t sign_len = (need_sign ? 1u : 0u);
        uint32_t base_len = sign_len + plen + zero_prec + outlen;

        if (pad_zero) {
            if (width > (int)base_len) zero_width = (uint32_t)width - base_len;
        } else {
            if (width > (int)base_len) left_spaces = (uint32_t)width - base_len;
        }
        if (flag_minus) {
            if (width > (int)base_len) right_spaces = (uint32_t)width - base_len;
        }
        if (left_spaces > (uint32_t)2147483647) left_spaces = (uint32_t)2147483647;
        if (right_spaces > (uint32_t)2147483647) right_spaces = (uint32_t)2147483647;
        if (zero_width > (uint32_t)2147483647) zero_width = (uint32_t)2147483647;

        append_repeat(&p, &rem, ' ', left_spaces, &truncated_all);
        if (need_sign) append_char(&p, &rem, signch, &truncated_all);
        if (plen) append_block(&p, &rem, prefix, plen, &truncated_all);
        append_repeat(&p, &rem, '0', zero_width, &truncated_all);
        append_repeat(&p, &rem, '0', zero_prec, &truncated_all);
        append_block(&p, &rem, obuf, outlen, &truncated_all);
        append_repeat(&p, &rem, ' ', right_spaces, &truncated_all);

        if (truncated_all) break;
    }

    if (truncated_all) {
        size_t w = (size_t)(p - out);
        const char *m = TRUNC_MARKER;
        size_t ml = strlen(m);
        if (w >= ml) {
            for (size_t i = 0; i < ml; i++) p[-(intptr_t)ml + i] = m[i];
        } else if (w) {
            size_t off = ml - w;
            for (size_t i = 0; i < w; i++) p[-(intptr_t)w + i] = m[off + i];
        }
    }

    if (cap > 0 && out) *p = 0;
    return (size_t)(p - out);
}

int tolower(int c){
    if (c >= 'A' && c <= 'Z') return c + 'a' - 'A';
    return c;
}

int toupper(int c){
    if (c >= 'a' && c <= 'z') return c - ('a' - 'A');
    return c;
}

int strcmp_case(const char *a, const char *b, bool case_insensitive){
    if (a == NULL && b == NULL) return 0;
    if (a == NULL) return -1;  
    if (b == NULL) return  1;

    while (*a && *b){
        char ca = *a;
        char cb = *b;
        if (case_insensitive){
            ca = tolower((unsigned char)ca);
            cb = tolower((unsigned char)cb);
        }
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    if (case_insensitive) return tolower((unsigned char)*a) - tolower((unsigned char)*b);
    
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp_case(const char *a, const char *b, bool case_insensitive, size_t max){
    if (a == NULL && b == NULL) return 0;
    if (a == NULL) return -1;  
    if (b == NULL) return  1;

    for (size_t i = 0; i < max && *a && *b; i++, a++, b++){
        char ca = *a;
        char cb = *b;
        if (case_insensitive){
            ca = tolower((unsigned char)ca);
            cb = tolower((unsigned char)cb);
        }
        if (ca != cb || i == max - 1) return ca - cb;
    }
    if (case_insensitive) return tolower((unsigned char)*a) - tolower((unsigned char)*b);
    
    return (unsigned char)*a - (unsigned char)*b;
}

int strstart_case(const char *a, const char *b, bool case_insensitive){
    int index = 0;
    if (!a || !b) return 0;
    while (*a && *b){
        char ca = *a;
        char cb = *b;

        if (case_insensitive){
            ca = tolower(ca);
            cb = tolower(cb);
        }

        if (ca != cb) return index;
        a++; b++; index++;
    }
    return index;
}

int strindex(const char *a, const char *b){
    for (int i = 0; a[i]; i++){
        int j = 0;
        while (b[j] && a[i + j] == b[j]) j++;
        if (!b[j]) return i;
    }
    return -1;
}

int strend_case(const char *a, const char *b, bool case_insensitive){
    while (*a && *b){
        char ca = case_insensitive ? tolower((unsigned char)*a) : *a;
        char cb = case_insensitive ? tolower((unsigned char)*b) : *b;

        if (ca == cb){
            const char *pa = a, *pb = b;
            while (1){
                char cpa = case_insensitive ? tolower((unsigned char)*pa) : *pa;
                char cpb = case_insensitive ? tolower((unsigned char)*pb) : *pb;

                if (!cpa) return cpb;
                if (cpa != cpb) break;

                pa++; pb++;
            }
        }
        a++;
    }
    return 1;
}

bool strcont(const char *a, const char *b){
    while (*a){
        const char *p = a, *q = b;
        while (*p && *q && *p == *q){
            p++; q++;
        }
        if (*q == 0) return 1;
        a++;
    }
    return 0;
}

int count_occurrences(const char* str, char c){
    int count = 0;
    while (*str) {
        if (*str == c) count++;
        str++;
    }
    return count;
}

bool utf16tochar(uint16_t* str_in, char* out_str, size_t max_len){
    size_t out_i = 0;
    for (size_t i = 0; i < max_len && str_in[i]; i++){
        uint16_t wc = str_in[i];
        out_str[out_i++] = (wc <= 0x7F) ? (char)(wc & 0xFF) : '?';
    }
    out_str[out_i++] = '\0';
    return true;
}

uint64_t parse_hex_u64(const char* str, size_t size){
    uint64_t result = 0;
    for (uint32_t i = 0; i < size; i++){
        char c = str[i];
        uint8_t digit = 0;
        if (i == 1 && (c == 'x' || c == 'X')) result = 0;
        else if (i == 0 && c == '#') result = 0;
        else if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        result = (result << 4) | digit;
    }
    return result;
}

uint64_t parse_int_u64(const char* str, size_t size){
    uint64_t result = 0;
    for (uint32_t i = 0; i < size; i++){
        char c = str[i];
        uint8_t digit = 0;
        if (c >= '0' && c <= '9') digit = c - '0';
        else break;
        result = (result * 10) + digit;
    }
    return result;
}

int64_t parse_int64(const char* str, size_t size){
    uint64_t result = 0;
    bool neg = false;
    for (uint32_t i = 0; i < size; i++){
        char c = str[i];
        uint8_t digit = 0;
        if (i == 0 && c == '-') neg = true;
        else if (c >= '0' && c <= '9') digit = c - '0';
        else break;
        result = (result * 10) + digit;
    }
    return neg ? -result : result;
}

string string_from_const(const char *lit)
{
    uint32_t len = strlen(lit);
    char* nlit = malloc(len+1);
    strncpy(nlit, lit, len+1);
    return (string){ nlit, len, len + 1};
}

string string_concat(string a, string b)
{
    uint32_t len = a.length + b.length;
    char *dst = (char *)malloc(len + 1);
    if (!dst) return (string){0};
    memcpy(dst, a.data, a.length);
    memcpy(dst + a.length, b.data, b.length);
    dst[len] = 0;
    return (string){ dst, len, len +1 };
}

void string_concat_inplace(string *dest, string src)
{
    if (!dest || !src.data) return;

    uint32_t new_len = dest->length + src.length;
    uint32_t new_cap = new_len + 1;

    char *dst = (char *)malloc(new_cap);
    if (!dst) return;

    if (dest->data && dest->length) {
        memcpy(dst, dest->data, dest->length);
    }
    memcpy(dst + dest->length, src.data, src.length);
    dst[new_len] = '\0';
    if (dest->data) {
        free_sized(dest->data, dest->mem_length);
    }
    dest->data = dst;
    dest->length = new_len;
    dest->mem_length = new_cap;
}

void string_append_bytes(string *dest, const void *buf, uint32_t len)
{
    if (!len) return;
    string tmp = { (char *)buf, len, len };
    string_concat_inplace(dest, tmp);
}

const char* seek_to(const char *string, char character){
    while (*string != character && *string != '\0')
        string++;
    if (*string == character) string++;
    return string;
}

char* strncpy(char* dst, const char* src, size_t cap){
    size_t i=0;
    if (!dst || !src || cap==0) return 0;
    while (i<cap-1 && src[i]!=0){ dst[i]=src[i]; i++; }
    dst[i]=0;
    return dst;
}

bool parse_uint32_dec(const char *s, uint32_t *out) {
    if (!s || !*s) return false;
    uint64_t v = parse_int_u64(s, UINT32_MAX);
    if (v == 0 && s[0] != '0') return false;
    if (v > UINT32_MAX) return false;
    *out = (uint32_t)v;
    return true;
}


char* strcasestr(const char* haystack, const char* needle) {
    if (!haystack) return 0;
    if (!needle) return (char*)haystack;
    if (!*needle) return (char*)haystack;

    for (const char* h = haystack; *h; h++) {
        const char* hp = h;
        const char* np = needle;

        while (*hp && *np) {
            char a = tolower(*hp);
            char b = tolower(*np);
            if (a != b) break;
            hp++;
            np++;
        }

        if (!*np) return (char*)h;
    }

    return 0;
}

void strcat_buf(const char *a, const char *b, char *dest){
    while (*a) *dest++ = *a++;
    while (*b) *dest++ = *b++;
    *dest = 0;
}

char* strcat_new(const char *a, const char *b){
    char* dest = (char*)malloc(strlen(a) + strlen(b) + 1);
    strcat_buf(a,b,dest);
    return dest;
}

string string_replace_character(char* original, char symbol, char *value){
    size_t fulllen = strlen(original);
    const char *next = seek_to(original, symbol);
    if (next == original+fulllen){
        return string_from_literal(original);
    }
    string_slice start = make_string_slice(original, 0, next-original-1);
    return string_format("%v%s%s",start, value, next);
}