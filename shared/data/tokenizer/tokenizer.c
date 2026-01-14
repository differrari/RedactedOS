#include "data/tokenizer/tokenizer.h"

static void tokenizer_fail(Tokenizer *t, TokenizerError err) {
    if (t->failed) return;

    t->failed = true;
    t->err = err;
    t->err_pos = t->s->pos;
}

static bool skip_ws_and_comments(Tokenizer *t, Token *out) {
    Scanner *s = t-> s;
    for (;;) {
        scan_skip_ws(s, !t->parse_newline);
        if (scan_eof(s)) return false;

        uint32_t pos =s->pos;

        if (scan_match(s, '\n')){
            out->kind = TOK_NEWLINE;
            out->start = s->buf + s->pos;
            out->length = 1;
            out->pos = s->pos;
            return true;
        }
        if (t->comment_type == TOKENIZER_COMMENT_TYPE_SLASH){
            if (scan_match(s, '/')) {
                if (scan_match(s, '/')) {
                    while (!scan_eof(s)) {
                        char c = scan_next(s);
                        if (c == '\n' || c == '\r') break;
                    }
                    continue;
                } else if (scan_match(s, '*')) {
                    int found = 0;
                    while (!scan_eof(s)) {
                        char c = scan_next(s);
                        if (c == '*' && !scan_eof(s) && scan_peek(s) == '/') {
                            scan_next(s);
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        tokenizer_fail(t, TOKENIZER_ERR_UNTERMINATED_COMMENT);
                        return false;
                    }
                    continue;
                } else {
                    s->pos = pos;
                }
            }
        } else if (t->comment_type == TOKENIZER_COMMENT_TYPE_HASH){
            if (scan_match(s, '#')) {
                while (!scan_eof(s)) {
                    char c = scan_next(s);
                    if (c == '\n' || c == '\r') break;
                }
                continue;
            }
        }

        break;
    }
    return false;
}

static void read_identifier(Scanner *s, Token *tok) {
    uint32_t start = s->pos;
    scan_next(s);

    while (!scan_eof(s)) {
        char c = scan_peek(s);
        if (!(is_alnum(c) || c == '_')) break;
        scan_next(s);
    }

    tok->kind = TOK_IDENTIFIER;
    tok->start = s->buf + start;
    tok->length = s->pos - start;
    tok->pos = start;
}

static bool read_number(Tokenizer *t, Token *tok) {
    Scanner *s = t->s;
    uint32_t start = s->pos;
    const char *buf = s->buf;
    uint32_t len = s->len;
    uint32_t pos = start;

    if (pos >= len || !(buf[pos] >= '0' && buf[pos] <= '9')) return false;

    if (buf[pos] == '0' && pos + 1 < len) {
        char p = buf[pos + 1];

        if (p == 'x' || p == 'X') {
            pos += 2;
            if (pos >= len) return false;

            int ok = 0;
            while (pos < len) {
                char c = buf[pos];
                if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
                    pos++;
                    ok = 1;
                } else break;
            }
            if (!ok) return false;

            s->pos = pos;
            tok->kind = t->skip_type_check ? TOK_CONST : TOK_NUMBER;
            tok->start = buf + start;
            tok->length = pos - start;
            tok->pos = start;

            return true;
        }

        if (p == 'b' || p == 'B') {
            pos += 2;
            if (pos >= len) return false;

            int ok = 0;
            while (pos < len) {
                char c = buf[pos];
                if (c == '0' || c == '1') {
                    pos++;
                    ok = 1;
                } else break;
            }
            if (!ok) return false;

            s->pos = pos;
            tok->kind = t->skip_type_check ? TOK_CONST : TOK_NUMBER;
            tok->start = buf + start;
            tok->length = pos - start;
            tok->pos = start;

            return true;
        }

        if (p == 'o' || p == 'O') {
            pos += 2;
            if (pos >= len) return false;

            int ok = 0;
            while (pos < len) {

                char c = buf[pos];
                if (c >= '0' && c <= '7') {
                    pos++;
                    ok = 1;
                } else break;
            }
            if (!ok) return false;

            s->pos = pos;
            tok->kind = t->skip_type_check ? TOK_CONST : TOK_NUMBER;
            tok->start = buf + start;
            tok->length = pos - start;
            tok->pos= start;

            return true;
        }
    }

    while (pos <len && buf[pos] >= '0' && buf[pos] <= '9') pos++;

    uint32_t int_end = pos;

    if (pos < len && buf[pos] == '.') {
        uint32_t p2 = pos + 1;
        if (p2 < len && buf[p2] >= '0' && buf[p2] <= '9') {
            pos = p2;
            while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') pos++;
        } else {
            pos = int_end;
        }
    }

    uint32_t mant_end = pos;

    if (pos < len && (buf[pos] == 'e' || buf[pos] == 'E')) {
        uint32_t exp_start = pos;
        pos++;
        if (pos < len && (buf[pos] == '+' || buf[pos] == '-')) pos++;
        if (pos < len && (buf[pos] >= '0' && buf[pos] <= '9')) {
            while (pos < len && (buf[pos] >= '0' && buf[pos] <= '9')) pos++;
            mant_end = pos;
        } else {
            pos = exp_start;
        }
    }

    s->pos = mant_end;
    tok->kind = t->skip_type_check ? TOK_CONST : TOK_NUMBER;
    tok->start = buf + start;
    tok->length = mant_end - start;
    tok->pos = start;
    return true;
}

static bool read_string(Tokenizer *t, Token *tok) {
    Scanner *s = t->s;
    uint32_t start = s->pos;

    if (!scan_match(s, '"')) return false;

    while (!scan_eof(s)) {
        char c = scan_next(s);
        if (c == '"') {
            tok->kind = t->skip_type_check ? TOK_CONST : TOK_STRING;
            tok->start = s->buf + start;
            tok->length = s->pos - start;
            tok->pos = start;
            return true;
        }

        if (c == '\\') {
            if (scan_eof(s)) {
                tokenizer_fail(t, TOKENIZER_ERR_UNTERMINATED_STRING);
                return false;
            }
            char e = scan_next(s);
            if (e == 'u') {
                for (int i = 0; i < 4; i++) {
                    if (scan_eof(s)) {
                        tokenizer_fail(t, TOKENIZER_ERR_UNTERMINATED_STRING);
                        return false;
                    }
                    char h = scan_next(s);
                    if (!((h >= '0' && h <= '9') ||
                          (h >= 'a' && h <= 'f') ||
                          (h >= 'A' && h <= 'F'))) {
                        tokenizer_fail(t, TOKENIZER_ERR_UNTERMINATED_STRING);
                        return false;
                    }
                }
            } else if (!(e == '"' || e == '\\' || e == '/' ||
                         e == 'b' || e == 'f' || e == 'n' ||
                         e == 'r' || e == 't')) {
                tokenizer_fail(t, TOKENIZER_ERR_UNTERMINATED_STRING);
                return false;
            }
        }
    }

    tokenizer_fail(t, TOKENIZER_ERR_UNTERMINATED_STRING);
    return false;
}

static const char *ops3[] = {">>>", "<<=", ">>=", "===", 0};
static const char *ops2[] = {"==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "::", "->", 0};
static const char ops1[] = "+-*/%=<>!&|^~?";

static bool read_operator(Scanner *s, Token *tok) {
    const char *buf = s->buf;
    uint32_t len = s->len;
    uint32_t pos = s->pos;

    for (int i = 0; ops3[i]; i++) {
        const char *op = ops3[i];
        uint32_t n = 3;
        if (pos + n <= len) {
            uint32_t k = 0;
            while (k < n && buf[pos + k] == op[k]) k++;

            if (k == n) {
                s->pos = pos + n;
                tok->kind = TOK_OPERATOR;
                tok->start = buf + pos;
                tok->length = n;
                tok->pos = pos;

                return true;
            }
        }
    }

    for (int i = 0; ops2[i]; i++) {
        const char *op = ops2[i];
        uint32_t n = 0;
        while (op[n]) n++;

        if (pos + n <= len) {
            uint32_t k = 0;
            while (k < n && buf[pos + k] == op[k]) k++;
            if (k == n) {
                s->pos = pos + n;
                tok->kind = TOK_OPERATOR;
                tok->start = buf + pos;
                tok->length = n;
                tok->pos = pos;

                return true;
            }
        }
    }

    if (pos >= len) return false;
    char c = buf[pos];

    for (int i = 0; ops1[i]; i++) {
        if (c == ops1[i]) {
            s->pos = pos + 1;
            tok->kind = TOK_OPERATOR;
            tok->start = buf + pos;
            tok->length = 1;
            tok->pos = pos;

            return true;
        }
    }

    return false;
}

static bool read_delim(Scanner *s, Token *tok) {
    if (scan_eof(s))return false;

    uint32_t pos = s->pos;
    char c = scan_peek(s);

    if (c == '(') { scan_next(s); tok->kind = TOK_LPAREN; }
    else if (c == ')'){ scan_next(s); tok->kind = TOK_RPAREN;}
    else if (c == '{'){ scan_next(s); tok->kind = TOK_LBRACE; }
    else if (c == '}'){ scan_next(s); tok->kind = TOK_RBRACE; }
    else if (c == '['){ scan_next(s); tok->kind = TOK_LBRACKET; }
    else if (c == ']'){ scan_next(s); tok->kind = TOK_RBRACKET; }
    else if (c == ','){ scan_next(s); tok->kind = TOK_COMMA; }
    else if (c == ':'){ scan_next(s); tok->kind = TOK_COLON; }
    else if (c == ';'){ scan_next(s); tok->kind = TOK_SEMICOLON; }
    else if (c == '.'){ scan_next(s); tok->kind = TOK_DOT; }
    else return false;

    tok->start = s->buf + pos;
    tok->length = 1;
    tok->pos =pos;

    return true;
}

bool tokenizer_next(Tokenizer *t, Token *out) {
    if (t->failed) {
        out->kind = TOK_INVALID;
        out->start = 0;
        out->length = 0;
        out->pos = t->s->pos;
        return false;
    }

    if (skip_ws_and_comments(t, out)){
        return true;
    }
    if (t->failed) {
        out->kind = TOK_INVALID;
        out->start = 0;
        out->length = 0;
        out->pos = t->err_pos;
        return false;
    }

    Scanner *s = t->s;

    if (scan_eof(s)) {
        out->kind = TOK_EOF;
        out->start = s->buf + s->pos;
        out->length = 0;
        out->pos = s->pos;
        return true;
    }

    char c = scan_peek(s);

    if (is_alpha(c) || c == '_') {
        read_identifier(s, out);
        return true;
    }

    if (c >= '0' && c <= '9') {
        uint32_t pos_before = s->pos;
        if (read_number(t, out)) return true;

        tokenizer_fail(t, TOKENIZER_ERR_INVALID_NUMBER);
        out->kind = TOK_INVALID;
        out->start = s->buf + pos_before;
        out->length = 0;
        out->pos = pos_before;
        return false;
    }

    if (c == '"') {
        uint32_t pos_before = s->pos;
        if (read_string(t, out)) return true;
        
        tokenizer_fail(t, TOKENIZER_ERR_UNTERMINATED_STRING);
        out->kind = TOK_INVALID;
        out->start = s->buf + pos_before;
        out->length = 0;
        out->pos = pos_before;
        return false;
    }

    if (read_delim(s, out)) return true;

    if (read_operator(s, out)) return true;

    out->kind = TOK_SYMBOL;
    out->start = s->buf + s->pos;
    out->length = 1;
    out->pos = s->pos;
    s->pos++;
    return true;
}

char* token_name(TokenKind kind){
    switch (kind) {
    case TOK_EOF: return "eof";
    case TOK_INVALID: return "invalid";
    case TOK_IDENTIFIER: return "identifier";
    case TOK_NUMBER: return "number";
    case TOK_STRING: return "string";
    case TOK_CONST: return "const";
    case TOK_OPERATOR: return "operator";
    case TOK_LPAREN: return "lparen";
    case TOK_RPAREN: return "rparen";
    case TOK_LBRACE: return "lbrace";
    case TOK_RBRACE: return "rbrace";
    case TOK_LBRACKET: return "lbracket";
    case TOK_RBRACKET: return "rbracket";
    case TOK_COMMA: return "comma";
    case TOK_COLON: return "colon";
    case TOK_SEMICOLON: return "semicolon";
    case TOK_DOT: return "dot";
    case TOK_SYMBOL: return "symbol";
    case TOK_NEWLINE: return "newline";
      default: return "";
    }
}