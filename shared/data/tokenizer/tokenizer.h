#pragma once
#include "types.h"
#include "data/scanner/scanner.h"
#include "std/string_slice.h"

typedef enum {
    TOK_EOF = 0,
    TOK_INVALID,

    TOK_IDENTIFIER,
    TOK_NUMBER,
    TOK_STRING,
    TOK_CONST,

    TOK_OPERATOR,

    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,

    TOK_COMMA,
    TOK_COLON,
    TOK_SEMICOLON,
    TOK_DOT, 
    
    TOK_SYMBOL,
    
    TOK_NEWLINE
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *start;
    uint32_t length;
    uint32_t pos;
} Token;

typedef enum {
    TOKENIZER_ERR_NONE = 0,
    TOKENIZER_ERR_INVALID_CHAR,
    TOKENIZER_ERR_INVALID_NUMBER,
    TOKENIZER_ERR_UNTERMINATED_STRING,
    TOKENIZER_ERR_UNTERMINATED_COMMENT
} TokenizerError;

typedef enum {
    TOKENIZER_COMMENT_TYPE_SLASH,
    TOKENIZER_COMMENT_TYPE_HASH,
} TokenizerComment;

typedef struct {
    Scanner *s;
    bool failed;
    TokenizerError err;
    uint32_t err_pos;
    bool skip_type_check;
    bool parse_newline;
    TokenizerComment comment_type;
} Tokenizer;

static inline Tokenizer tokenizer_make(Scanner *s) {
    Tokenizer t = {};
    t.s = s;
    t.failed = false;
    t.err = TOKENIZER_ERR_NONE;
    t.err_pos = 0;
    return t;
}

static inline bool tokenizer_ok(const Tokenizer *t) {
    return !t->failed;
}

bool tokenizer_next(Tokenizer *t, Token *out);

static inline string_slice token_to_slice(Token t){
    return (string_slice){ .data = (char*)t.start, .length = t.length };
}

char* token_name(TokenKind kind);
