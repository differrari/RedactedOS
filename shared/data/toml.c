#include "toml.h"
#include "data/scanner/scanner.h"
#include "data/helpers/token_stream.h"

void read_toml_value(TokenStream *ts, Token key, toml_handler on_kvp, void *context){
    Token t;
    ts_next(ts, &t);
    string s = string_from_literal_length(key.start, key.length);
    
    switch (t.kind){
        case TOK_IDENTIFIER: {

            on_kvp(token_to_slice(key), token_to_slice(t), context);
        }
        break;
        case TOK_STRING: {

            on_kvp(token_to_slice(key), make_string_slice(t.start+1, 0, t.length-2), context);
        }
        break;
        case TOK_NUMBER: {

            on_kvp(token_to_slice(key), token_to_slice(t), context);
        }
        break;
        
        case TOK_LBRACKET: {
            int depth = 0;
            Token t2;
            while (ts_next(ts, &t2)) {
                if (!t2.kind) return;
                if (t2.kind == TOK_LBRACKET) depth++;
                if (t2.kind == TOK_RBRACKET) { 
                    if (depth) 
                        depth--; 
                    else {
                        on_kvp(token_to_slice(key), make_string_slice((char*)t.start, 1, t2.start - t.start - 1), 0);
                        break;
                    }
                }
            }
        }
        break;
        
        default: 
            
        break;
    }
    
    string_free(s);
    
}

void read_toml(char *info, toml_handler on_kvp, void *context){
    Scanner s = scanner_make(info, strlen(info));
    Tokenizer tk = tokenizer_make(&s);
    tk.comment_type = TOKENIZER_COMMENT_TYPE_HASH;
    tk.skip_type_check = false;
    TokenStream ts;
    ts_init(&ts, &tk);
    
    Token t;
    while (ts_next(&ts, &t)) {
        if (!t.kind) break;
        
        switch (t.kind) {
            case TOK_IDENTIFIER:{
                Token op;
                if (!ts_expect(&ts, TOK_OPERATOR, &op) || !slice_lit_match(token_to_slice(op), "=", false)){
                    return;
                }
                read_toml_value(&ts, t, on_kvp, context);
            }
            break;
            case TOK_LBRACKET://TODO: in real TOML, these have meaning
            {       
                Token t2;
                while (ts_next(&ts, &t2)) {
                    if (!t2.kind) return;
                    if (t2.kind == TOK_RBRACKET) break;
                }
            }
            break;
            default: 
                
            break;
        }
    }

    return;
    
}