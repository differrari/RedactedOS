#include "csv.h"
#include "scanner/scanner.h"
#include "tokenizer/tokenizer.h"
#include "helpers/token_stream.h"

void read_csv(string_slice slice, csv_handler on_val){
    Scanner s = scanner_make(slice.data, slice.length);
    Tokenizer tk = tokenizer_make(&s);
    tk.comment_type = TOKENIZER_COMMENT_TYPE_HASH;
    tk.skip_type_check = false;
    TokenStream ts;
    ts_init(&ts, &tk);
    
    Token t;
    while (ts_next(&ts, &t)) {
        if (!t.kind) break;
        
        switch (t.kind) {
            case TOK_IDENTIFIER:
            case TOK_STRING:
            case TOK_CONST:
            case TOK_NUMBER:
            {
                on_val(token_to_slice(t));
            } break;
            case TOK_COMMA: {
                
            } break;
            default: {
                print("Wrong symbol %v of type %i",token_to_slice(t),t.kind);
                return;
            }
        }
    }
}