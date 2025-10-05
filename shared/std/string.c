#include "std/string.h"
#include "syscalls/syscalls.h"
#include "std/memory.h"
#include "types.h"

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

static inline uint32_t u64_to_dec(char *tmp, uint64_t v) {
    uint32_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && n < 21);
    memreverse(tmp, n);
    return n;
}

static inline uint32_t i64_to_dec(char *tmp, int64_t vv, int *neg) {
    *neg = (vv < 0);
    uint64_t v = (uint64_t)(*neg ? -vv : vv);
    return u64_to_dec(tmp, v);
}

static inline uint32_t u64_to_base(char *tmp, uint64_t v, unsigned base, int upper) {
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    uint32_t n = 0;
    do {
        tmp[n++] = digs[v % base];
        v /= base;
    } while (v && n < 65);
    memreverse(tmp, n);
    return n;
}

uint32_t strlen(const char *s, uint32_t max_length){
    if (s == NULL) return 0;
    
    uint32_t len = 0;
    while ((max_length == 0 || len < max_length) && s[len] != '\0') len++;
    
    return len;
}

string string_from_literal(const char *literal){
    if (literal == NULL) return (string){ .data = NULL, .length = 0, .mem_length = 0};
    
    uint32_t len = strlen(literal, 0);
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

    uint32_t len = strlen(array, 0);
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

    uint32_t len = strlen(array, max_length);
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
    uint32_t len = parse_bin(value, buf);
    return (string){ .data = buf, .length = len, .mem_length = 66 };
}

bool string_equals(string a, string b){
    return strcmp(a.data,b.data, false) == 0;
}

string string_replace(const char *str, char orig, char repl){
    size_t str_size = strlen(str, 0);
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
//TODO unifdy %s and %c handling with the generic padding
size_t string_format_va_buf(const char *fmt, char *out, size_t cap, va_list args) {
    char *p = out;
    size_t rem = cap ? cap : 1;
    int truncated_all = 0;

    for (uint32_t i = 0; fmt && fmt[i] && rem > 1;) {
        if (fmt[i] != '%') {
            append_char(&p, &rem, fmt[i++], &truncated_all);
            continue;
        }

        i++;

        int flag_minus = 0, flag_plus = 0, flag_space = 0, flag_zero = 0, flag_hash = 0;
        while (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' || fmt[i] == '0' || fmt[i] == '#') {
            if (fmt[i] == '-') flag_minus = 1;
            else if (fmt[i] == '+') flag_plus = 1;
            else if (fmt[i] == ' ') flag_space = 1;
            else if (fmt[i] == '0') flag_zero = 1;
            else flag_hash = 1;
            i++;
        }

        int width = 0;
        int width_star = 0;
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
        if (precision_set && precision < 0) {
            precision_set = 0;
        }
        int had_precision = precision_set;

        enum { LEN_DEF, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_Z, LEN_T, LEN_J } len = LEN_DEF;
        if (fmt[i] == 'h') { if (fmt[i + 1] == 'h') { len = LEN_HH; i += 2; } else { len = LEN_H; i++; } }
        else if (fmt[i] == 'l') { if (fmt[i + 1] == 'l') { len = LEN_LL; i += 2; } else { len = LEN_L; i++; } }
        else if (fmt[i] == 'z') { len = LEN_Z; i++; }
        else if (fmt[i] == 't') { len = LEN_T; i++; }
        else if (fmt[i] == 'j') { len = LEN_J; i++; }

        if (width_star) {
            width = va_arg(args, int);
            if (width < 0) { flag_minus = 1; width = -width; }
        }
        if (!fmt[i]) {
            append_char(&p, &rem, '%', &truncated_all);
            break;
        }
        char spec = fmt[i++];

        char numtmp[66];
        char outtmp[256];
        uint32_t outlen = 0;
        int is_num = 0;
        int negative = 0;

        if (spec == '%') {
            uint32_t padlen = (width > 1) ? (uint32_t)(width - 1) : 0;
            char padc = (flag_zero && !had_precision) ? '0' : ' ';
            if (!flag_minus) {
                for (uint32_t z = 0; z < padlen; z++) append_char(&p, &rem, padc, &truncated_all);
                append_char(&p, &rem, '%', &truncated_all);
            } else {
                append_char(&p, &rem, '%', &truncated_all);
                for (uint32_t z = 0; z < padlen; z++) append_char(&p, &rem, ' ', &truncated_all);
            }
            continue;
        } else if (spec == 'c') {
            int ch = va_arg(args, int);
            outtmp[0] = (char)ch;
            outlen = 1;
        } else if (spec == 's') {
            const char *s = va_arg(args, char *);
            if (!s) s = "(null)";
            uint32_t sl = strlen(s, 0);
            if (precision_set && (uint32_t)precision < sl) sl = (uint32_t)precision;

            uint32_t pad = (width > (int)sl) ? (uint32_t)width - sl : 0;
            if (!flag_minus) {
                for (uint32_t z = 0; z < pad; z++) append_char(&p, &rem, ' ', &truncated_all);
                append_strn(&p, &rem, s, sl, &truncated_all);
            } else {
                append_strn(&p, &rem, s, sl, &truncated_all);
                for (uint32_t z = 0; z < pad; z++) append_char(&p, &rem, ' ', &truncated_all);
            }
            continue;
        } else if (spec == 'S') {
            const string *sp = va_arg(args, const string *);
            string sv = sp ? *sp : (string){ .data = NULL, .length = 0, .mem_length = 0};
            const char *s = sv.data ? sv.data : "(null)";
            uint32_t sl = sv.data ? sv.length : 6;
            if (precision_set && (uint32_t)precision < sl) sl = (uint32_t)precision;

            uint32_t pad = (width > (int)sl) ? (uint32_t)width - sl : 0;
            if (!flag_minus) {
                for (uint32_t z = 0; z < pad; z++) append_char(&p, &rem, ' ', &truncated_all);
                append_strn(&p, &rem, s, sl, &truncated_all);
            } else {
                append_strn(&p, &rem, s, sl, &truncated_all);
                for (uint32_t z = 0; z < pad; z++) append_char(&p, &rem, ' ', &truncated_all);
            }
            continue;
        } else if (spec == 'p') {
            uintptr_t v = (uintptr_t)va_arg(args, void *);
            outtmp[outlen++] = '0';
            outtmp[outlen++] = 'x';
            uint32_t n = u64_to_base(numtmp, (uint64_t)v, 16, 0);
            for (uint32_t k = 0; k < n && outlen < sizeof(outtmp); k++) outtmp[outlen++] = numtmp[k];
            is_num = 1;
        } else if (spec == 'b') {
            uint64_t v = va_arg(args, uint64_t);
            if (precision_set && precision == 0 && v == 0) {
                outlen = 0;
            } else {
                if (flag_hash) { outtmp[outlen++] = '0'; outtmp[outlen++] = 'b'; }
                uint32_t n = u64_to_base(numtmp, v, 2, 0);
                for (uint32_t k = 0; k < n && outlen < sizeof(outtmp); k++) outtmp[outlen++] = numtmp[k];
            }
            is_num = 1;
        } else if (spec == 'o' || spec == 'u' || spec == 'x' || spec == 'X' || spec == 'd' || spec == 'i') {
            int base = (spec == 'o') ? 8 : ((spec == 'x' || spec == 'X') ? 16 : 10);
            int upper = (spec == 'X');
            int is_signed = (spec == 'd' || spec == 'i');

            uint64_t u = 0;

            if (is_signed) {
                int64_t sv = 0;
                if (len == LEN_HH) sv = (signed char)va_arg(args, int);
                else if (len == LEN_H) sv = (short)va_arg(args, int);
                else if (len == LEN_L) sv = va_arg(args, long);
                else if (len == LEN_LL) sv = va_arg(args, long long);
                else if (len == LEN_Z) sv = (long long)va_arg(args, size_t);
                else if (len == LEN_T) sv = (long long)va_arg(args, intptr_t);
                else if (len == LEN_J) sv = va_arg(args, int64_t);
                else sv = va_arg(args, int);

                if (base == 10) {
                    char dtmp[32];
                    int neg = 0;
                    uint32_t dn = i64_to_dec(dtmp, sv, &neg);
                    for (uint32_t k = 0; k < dn && k < sizeof(outtmp); k++) outtmp[k] = dtmp[k];
                    outlen = dn;
                    negative = neg;
                } else {
                    u = (uint64_t)sv;
                    uint32_t n = u64_to_base(numtmp, u, (unsigned)base, upper);
                    for (uint32_t k = 0; k < n && outlen < sizeof(outtmp); k++) outtmp[outlen++] = numtmp[k];
                    negative = (sv < 0);
                }
            } else {
                if (len == LEN_HH) u = (unsigned char)va_arg(args, int);
                else if (len == LEN_H) u = (unsigned short)va_arg(args, int);
                else if (len == LEN_L) u = va_arg(args, unsigned long);
                else if (len == LEN_LL) u = va_arg(args, unsigned long long);
                else if (len == LEN_Z) u = (uint64_t)va_arg(args, size_t);
                else if (len == LEN_T) u = (uint64_t)va_arg(args, uintptr_t);
                else if (len == LEN_J) u = (uint64_t)va_arg(args, uint64_t);
                else u = va_arg(args, unsigned int);

                if (base == 10) {
                    char dtmp[32];
                    uint32_t dn = u64_to_dec(dtmp, u);
                    for (uint32_t k = 0; k < dn && k < sizeof(outtmp); k++) outtmp[k] = dtmp[k];
                    outlen = dn;
                } else {
                    uint32_t n = u64_to_base(numtmp, u, (unsigned)base, upper);
                    for (uint32_t k = 0; k < n && outlen < sizeof(outtmp); k++) outtmp[outlen++] = numtmp[k];
                }
            }

            if (precision_set) {
                if ((uint32_t)precision == 0 && outlen == 1 && outtmp[0] == '0') {
                    outlen = 0;
                } else if ((uint32_t)precision > outlen) {
                    uint32_t pad = (uint32_t)precision - outlen;
                    if (pad + outlen < sizeof(outtmp)) {
                        for (uint32_t k = outlen; k > 0; k--) outtmp[k + pad - 1] = outtmp[k - 1];
                        for (uint32_t z = 0; z < pad; z++) outtmp[z] = '0';
                        outlen += pad;
                    }
                }
            }
            if (spec == 'o' && flag_hash && outlen == 0) {
                outtmp[0] = '0';
                outlen = 1;
            }

            if (flag_hash && outlen) {
                if (spec == 'o') {
                    if (outtmp[0] != '0') {
                        if (outlen + 1 < sizeof(outtmp)) {
                            for (uint32_t k = outlen; k > 0; k--) outtmp[k] = outtmp[k - 1];
                            outtmp[0] = '0';
                            outlen++;
                        }
                    }
                } else if (spec == 'x' || spec == 'X') {
                    int add_prefix = !((outlen == 1) && (outtmp[0] == '0'));
                    if (add_prefix && outlen + 2 < sizeof(outtmp)) {
                        for (uint32_t k = outlen + 1; k > 1; k--) outtmp[k] = outtmp[k - 2];
                        outtmp[0] = '0';
                        outtmp[1] = (spec == 'x') ? 'x' : 'X';
                        outlen += 2;
                    }
                }
            }
            is_num = 1;
        } else if (spec == 'f' || spec == 'F' || spec == 'e' || spec == 'E' || spec == 'g' || spec == 'G' || spec == 'a' || spec == 'A') {
            double dv = va_arg(args, double);
            int upper = (spec == 'F' || spec == 'E' || spec == 'G' || spec == 'A');

            uint64_t bits; memcpy(&bits, &dv, sizeof(bits));
            int is_nan = ((bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) && (bits & 0x000FFFFFFFFFFFFFULL);
            int is_inf = ((bits & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL);
            int signbit = (int)((bits >> 63) & 1);

            if (is_nan) {
                if (signbit) negative = 1;
                outtmp[0] = 'n'; outtmp[1] = 'a'; outtmp[2] = 'n';
                outlen = 3;
                is_num = 1;
            } else if (is_inf) {
                negative = signbit;
                outtmp[0] = 'i'; outtmp[1] = 'n'; outtmp[2] = 'f';
                outlen = 3;
                is_num = 1;
            } else {
                if (!precision_set) precision = 6;
                if (precision < 0) precision = 0;

                if (spec == 'a' || spec == 'A') {
                    int neg_local = (int)((bits >> 63) & 1);
                    uint64_t exp = (bits >> 52) & 0x7FFULL;
                    uint64_t frac = (bits & 0xFFFFFFFFFFFFFULL);

                    negative = neg_local;

                    if ((exp | frac) == 0) {
                        outtmp[outlen++] = '0';
                        outtmp[outlen++] = 'x';
                        outtmp[outlen++] = '0';
                        if (precision > 0) {
                            outtmp[outlen++] = '.';
                            for (int z = 0; z < precision && outlen < sizeof(outtmp); z++) outtmp[outlen++] = '0';
                        } else if (flag_hash) outtmp[outlen++] = '.';
                        outtmp[outlen++] = upper ? 'P' : 'p';
                        outtmp[outlen++] = '+';
                        outtmp[outlen++] = '0';
                    } else {
                        int e = 0;
                        uint64_t mant = 0;
                        if (exp == 0) {
                            e = -1022;
                            mant = frac;
                            while (mant && (mant & (1ULL << 52)) == 0) { mant <<= 1; e--; }
                        } else {
                            e = (int)exp - 1023;
                            mant = (1ULL << 52) | frac;
                        }

                        uint64_t hex_frac = mant & ((1ULL << 52) - 1);
                        outtmp[outlen++] = '0';
                        outtmp[outlen++] = 'x';
                        outtmp[outlen++] = '1';
                        if (precision > 0) {
                            outtmp[outlen++] = '.';
                            int hex_digits = precision;
                            for (int d = 0; d < hex_digits && outlen < sizeof(outtmp); d++) {
                                uint8_t nib = (uint8_t)((hex_frac >> (52 - 4 - 4*d)) & 0xF);
                                outtmp[outlen++] = upper ? "0123456789ABCDEF"[nib] : "0123456789abcdef"[nib];
                            }
                        } else if (flag_hash) outtmp[outlen++] = '.';
                        outtmp[outlen++] = upper ? 'P' : 'p';
                        outtmp[outlen++] = (e >= 0) ? '+' : '-';
                        char etmp[32];
                        uint32_t en = u64_to_dec(etmp, (uint64_t)(e >= 0 ? e : -e));
                        for (uint32_t k = 0; k < en; k++) outtmp[outlen++] = etmp[k];
                    }
                    is_num = 1;
                } else if (spec == 'f' || spec == 'F') {
                    if (dv < 0) {
                        negative = 1; dv = -dv;
                    }

                    uint64_t whole = (uint64_t)dv;
                    double frac = dv - (double)whole;

                    int prec = precision;
                    if (prec > 9) prec = 9;

                    uint64_t scale = 1;
                    for (int d = 0; d < prec; d++) scale *= 10ull;

                    uint64_t frac_scaled = (prec > 0) ? (uint64_t)(frac * (double)scale + 0.5) : 0ull;
                    if (prec > 0 && frac_scaled >= scale) { whole += 1; frac_scaled -= scale; }

                    char dtmp[32];
                    uint32_t dn = u64_to_dec(dtmp, whole);
                    for (uint32_t k = 0; k < dn && k < sizeof(outtmp); k++) outtmp[k] = dtmp[k];
                    outlen = dn;

                    if (precision > 0 && outlen < sizeof(outtmp)) outtmp[outlen++] = '.';
                    else if (flag_hash && outlen < sizeof(outtmp)) outtmp[outlen++] = '.';
                    if (precision > 0) {
                        char ftmp[16];
                        uint32_t fn = 0;
                        uint64_t t = frac_scaled;
                        for (int d = 0; d < prec; d++) { ftmp[prec - 1 - d] = (char)('0' + (t % 10)); t /= 10; }
                        fn = (uint32_t)prec;
                        for (uint32_t k = 0; k < fn && outlen < sizeof(outtmp); k++) outtmp[outlen++] = ftmp[k];
                    }
                    is_num = 1;
                } else if (spec == 'e' || spec == 'E') {
                    int exp10 = 0;
                    double v = dv;
                    if (v == 0.0) {
                        exp10 = 0;
                    }else {
                        double av = v < 0 ? -v : v;
                        while (av >= 10.0) { av /= 10.0; exp10++; }
                        while (av < 1.0) { av *= 10.0; exp10--; }
                    }
                    double m = dv;
                    for (int k = 0; k < (exp10 > 0 ? exp10 : -exp10); k++) {
                        if (exp10 > 0) m /= 10.0; 
                        else m *= 10.0;
                    }
                    if (m < 0) { negative = 1; m = -m; }

                    int prec = precision; if (prec > 9) prec = 9;

                    uint64_t whole = (uint64_t)m;
                    double frac = m - (double)whole;

                    uint64_t scale = 1;
                    for (int d = 0; d < prec; d++) scale *= 10ull;

                    uint64_t frac_scaled = (prec > 0) ? (uint64_t)(frac * (double)scale + 0.5) : 0ull;
                    if (prec > 0 && frac_scaled >= scale) { whole += 1; frac_scaled -= scale; if (whole >= 10) { whole = 1; exp10++; } }

                    char dtmp[32];
                    uint32_t dn = u64_to_dec(dtmp, whole);
                    for (uint32_t k = 0; k < dn && k < sizeof(outtmp); k++) outtmp[k] = dtmp[k];
                    outlen = dn;

                    if (precision > 0 && outlen < sizeof(outtmp)) outtmp[outlen++] = '.';
                    else if (flag_hash && outlen < sizeof(outtmp)) outtmp[outlen++] = '.';

                    if (precision > 0) {
                        char ftmp[16];
                        for (int d = prec - 1; d >= 0; d--) { ftmp[d] = (char)('0' + (frac_scaled % 10)); frac_scaled /= 10; }
                        for (int d = 0; d < prec && outlen < sizeof(outtmp); d++) outtmp[outlen++] = ftmp[d];
                    }

                    outtmp[outlen++] = upper ? 'E' : 'e';
                    outtmp[outlen++] = (exp10 >= 0) ? '+' : '-';
                    uint32_t aexp = (uint32_t)(exp10 >= 0 ? exp10 : -exp10);
                    char etmp[32];
                    uint32_t en = u64_to_dec(etmp, aexp);
                    if (en < 2) outtmp[outlen++] = '0';
                    for (uint32_t k = 0; k < en; k++) outtmp[outlen++] = etmp[k];
                    is_num = 1;
                } else {
                    int use_e = 0;
                    double av = dv < 0 ? -dv : dv;
                    int exp10 = 0;
                    if (av != 0.0) {
                        double t = av;
                        while (t >= 10.0) { t /= 10.0; exp10++; }
                        while (t < 1.0) { t *= 10.0; exp10--; }
                    }
                    int p = precision == 0 ? 1 : precision;
                    if (exp10 < -4 || exp10 >= p) use_e = 1;

                    if (use_e) {
                        char tmp[256];
                        char *tp = tmp;
                        size_t trem = sizeof(tmp);
                        va_list ap_copy;
                        va_copy(ap_copy, args);
                        double dv_copy = dv;
                        int prec_local = p - 1;
                        if (!precision_set) prec_local = 6 - 1;
                        if (prec_local < 0) prec_local = 0;
                        int prc = prec_local;
                        int loc_upper = (spec == 'G');
                        double m = dv_copy;
                        uint64_t b2;
                        memcpy(&b2, &m, sizeof(b2));
                        int exp10l = 0;
                        if (m == 0.0) exp10l = 0;
                        else {
                            double av2 = m < 0 ? -m : m;
                            while (av2 >= 10.0) { av2 /= 10.0; exp10l++; }
                            while (av2 < 1.0) { av2 *= 10.0; exp10l--; }
                        }
                        double mm = m;
                        for (int k = 0; k < (exp10l > 0 ? exp10l : -exp10l); k++) {
                            if (exp10l > 0) mm /= 10.0; 
                            else mm *= 10.0;
                        }
                        if (mm < 0) mm = -mm;
                        if (prc > 9) prc = 9;
                        uint64_t whole = (uint64_t)mm;
                        double frac = mm - (double)whole;
                        uint64_t scale = 1;
                        for (int d = 0; d < prc; d++) scale *= 10ull;
                        uint64_t frac_scaled = (prc > 0) ? (uint64_t)(frac * (double)scale + 0.5) : 0ull;
                        if (prc > 0 && frac_scaled >= scale) {
                            whole += 1;
                            frac_scaled -= scale;
                            if (whole >= 10) { whole = 1; exp10l++; } 
                        }
                        char dtmp2[32]; uint32_t dn2 = u64_to_dec(dtmp2, whole);
                        for (uint32_t k = 0; k < dn2 && trem > 1; k++) { *tp++ = dtmp2[k]; trem--; }
                        if (prc > 0 && trem > 1) {
                            *tp++ = '.';
                            trem--;
                        } else if (flag_hash && trem > 1) {
                            *tp++ = '.';
                            trem--;
                        }
                        for (int d = prc - 1; d >= 0 && trem > 1; d--) {
                            *tp++ = (char)('0' + (frac_scaled % 10));
                            frac_scaled /= 10;
                            trem--;
                        }
                        *tp++ = loc_upper ? 'E' : 'e'; trem--;
                        *tp++ = (exp10l >= 0) ? '+' : '-'; trem--;
                        uint32_t aexp = (uint32_t)(exp10l >= 0 ? exp10l : -exp10l);
                        char etmp2[32]; uint32_t en2 = u64_to_dec(etmp2, aexp);
                        if (en2 < 2 && trem > 2) { *tp++ = '0'; trem--; }
                        for (uint32_t k = 0; k < en2 && trem > 1; k++) { *tp++ = etmp2[k]; trem--; }
                        *tp = 0;
                        uint32_t tl = strlen(tmp, 0);
                        uint32_t cut = tl;
                        if (!flag_hash) {
                            int dotpos = -1, ep = -1;
                            for (uint32_t k = 0; k < tl; k++) { if (tmp[k] == '.' && dotpos < 0) dotpos = (int)k; if (tmp[k] == 'e' || tmp[k] == 'E') { ep = (int)k; break; } }
                            if (dotpos >= 0 && ep > dotpos) {
                                int end = ep - 1;
                                while (end > dotpos && tmp[end] == '0') end--;
                                if (tmp[end] == '.') end--;
                                uint32_t nout = (uint32_t)(end + 1);
                                for (uint32_t k = 0; k < (uint32_t)ep - nout; k++) tmp[nout + k] = tmp[ep + k];
                                cut = nout + (uint32_t)(tl - ep);
                            }
                        }
                        for (uint32_t k = 0; k < cut && k < sizeof(outtmp); k++) outtmp[k] = (spec == 'G' ? (tmp[k] == 'e' ? 'E' : tmp[k]) : tmp[k]);
                        outlen = cut;
                        is_num = 1;
                    } else {
                        char tmp[256];
                        double m = dv;
                        int neg = 0;
                        if (m < 0) { neg = 1; m = -m; }
                        int pr = p - 1 - exp10;
                        if (pr < 0) pr = 0;
                        if (pr > 9) pr = 9;
                        if (!precision_set) pr = 6;
                        if (pr > 9) pr = 9;

                        uint64_t whole = (uint64_t)m;
                        double frac = m - (double)whole;

                        uint64_t scale = 1;
                        for (int d = 0; d < pr; d++) scale *= 10ull;

                        uint64_t frac_scaled = (pr > 0) ? (uint64_t)(frac * (double)scale + 0.5) : 0ull;
                        if (pr > 0 && frac_scaled >= scale) { whole += 1; frac_scaled -= scale; }

                        char dtmp[32]; uint32_t dn = u64_to_dec(dtmp, whole);
                        uint32_t tp = 0;
                        for (uint32_t k = 0; k < dn && tp < sizeof(tmp); k++) tmp[tp++] = dtmp[k];
                        if (pr > 0 && tp < sizeof(tmp)) tmp[tp++] = '.';
                        else if (flag_hash && tp < sizeof(tmp)) tmp[tp++] = '.';
                        for (int d = pr - 1; d >= 0 && tp < sizeof(tmp); d--) { tmp[tp + d] = (char)('0' + (frac_scaled % 10)); frac_scaled /= 10; }
                        tp += (uint32_t)pr;
                        if (!flag_hash && pr > 0) {
                            while (tp > 0 && tmp[tp - 1] == '0') tp--;
                            if (tp > 0 && tmp[tp - 1] == '.') tp--;
                        }
                        for (uint32_t k = 0; k < tp && k < sizeof(outtmp); k++) outtmp[k] = tmp[k];
                        outlen = tp;
                        negative = neg;
                        is_num = 1;
                    }
                }
            }

            if ((spec == 'F' || spec == 'E' || spec == 'G' || spec == 'A') && outlen) {
                for (uint32_t ui = 0; ui < outlen; ui++) {
                    if (outtmp[ui] >= 'a' && outtmp[ui] <= 'z') outtmp[ui] -= ('a' - 'A');
                }
            }
        } else {
            append_char(&p, &rem, '%', &truncated_all);
            append_char(&p, &rem, spec, &truncated_all);
            continue;
       }

        uint32_t padlen = 0;
        int need_sign = 0;
        char signch = 0;

        int allow_sign = (spec == 'd' || spec == 'i' || spec == 'f' || spec == 'F' || spec == 'e' || spec == 'E' || spec == 'g' || spec == 'G' ||spec == 'a' || spec == 'A');
        if (is_num && allow_sign) {
            if (negative) { need_sign = 1; signch = '-'; }
            else if (flag_plus) { need_sign = 1; signch = '+';}
            else if (flag_space) { need_sign = 1; signch = ' '; }
        }

        if (need_sign) {
            if (outlen + 1 < sizeof(outtmp)) {
                for (uint32_t k = outlen; k > 0; k--) outtmp[k] = outtmp[k - 1];
                outtmp[0] = signch;
                outlen++;
            }
        }

        if (width > (int)outlen) padlen = (uint32_t)width - outlen;

        if (!flag_minus) {
            if ((spec == 'd' || spec == 'i' || spec == 'u' || spec == 'o' || spec == 'x' || spec == 'X' || spec == 'b') && had_precision) flag_zero = 0;
            char padc = (flag_zero && !had_precision) ? '0' : ' ';
            if ((spec == 'f' || spec == 'F' || spec == 'e' || spec == 'E' || spec == 'g' || spec == 'G' || spec == 'a' || spec == 'A') && flag_zero) padc = '0';
            if (padc == '0' && padlen && outlen) {
                uint32_t prelen = 0;
                if (outtmp[0] == '+' || outtmp[0] == '-' || outtmp[0] == ' ') prelen = 1;
                if (outlen >= prelen + 2 && outtmp[prelen] == '0' && (outtmp[prelen + 1] == 'x' || outtmp[prelen + 1] == 'X' || outtmp[prelen + 1] == 'b')) prelen += 2;

                append_strn(&p, &rem, outtmp, prelen, &truncated_all);
                for (uint32_t z = 0; z < padlen; z++) append_char(&p, &rem, '0', &truncated_all);
                append_strn(&p, &rem, outtmp + prelen, outlen - prelen, &truncated_all);
            } else {
                for (uint32_t z = 0; z < padlen; z++) append_char(&p, &rem, ' ', &truncated_all);
                append_strn(&p, &rem, outtmp, outlen, &truncated_all);
            }
        } else {
            append_strn(&p, &rem, outtmp, outlen, &truncated_all);
            for (uint32_t z = 0; z < padlen; z++) append_char(&p, &rem, ' ', &truncated_all);
        }

        if (truncated_all) break;
    }

    if (truncated_all) {
        size_t w = (size_t)(p - out);
        const char *m = TRUNC_MARKER;
        size_t ml = strlen(m, 0);
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

char tolower(char c){
    if (c >= 'A' && c <= 'Z') return c + 'a' - 'A';
    return c;
}

char toupper(char c){
    if (c >= 'a' && c <= 'z') return c - ('a' - 'A');
    return c;
}

int strcmp(const char *a, const char *b, bool case_insensitive){
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

int strncmp(const char *a, const char *b, bool case_insensitive, int max){
    if (a == NULL && b == NULL) return 0;
    if (a == NULL) return -1;  
    if (b == NULL) return  1;

    for (int i = 0; i < max && *a && *b; i++, a++, b++){
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

int strstart(const char *a, const char *b, bool case_insensitive){
    int index = 0;
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

int strend(const char *a, const char *b, bool case_insensitive){
    while (*a && *b){
        char ca = case_insensitive ? tolower((unsigned char)*a) : *a;
        char cb = case_insensitive ? tolower((unsigned char)*b) : *b;

        if (ca == cb){
            const char *pa = a, *pb = b;
            while (1){
                char cpa = case_insensitive ? tolower((unsigned char)*pa) : *pa;
                char cpb = case_insensitive ? tolower((unsigned char)*pb) : *pb;

                if (!cpa) return 0;
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
        if (c >= '0' && c <= '9') digit = c - '0';
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

string string_from_const(const char *lit)
{
    uint32_t len = strlen(lit, 0);
    return (string){ (char *)lit, len, len };
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
        free(dest->data, dest->mem_length);
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
    string++;
    return string;
}