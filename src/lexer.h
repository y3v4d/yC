#ifndef yc_lexer_c
#define yc_lexer_c

typedef enum {
    TOKEN_LEFT_PAREN, TOKEN_RIGHT_PAREN,
    TOKEN_LEFT_BRACE, TOKEN_RIGHT_BRACE,
    TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH,
    TOKEN_AMPERSAND, TOKEN_DOT, TOKEN_BANG,
    TOKEN_EQUAL, TOKEN_SEMICOLON, TOKEN_COMMA,
    TOKEN_NUMBER, TOKEN_TRUE, TOKEN_FALSE,
    TOKEN_IDENTIFIER,
    TOKEN_IF, TOKEN_ELSE, TOKEN_PRINT,
    TOKEN_STRUCT,
    TOKEN_CHAR_LITERAL, TOKEN_STRING_LITERAL,
    TOKEN_LBRACKET, TOKEN_RBRACKET,
    TOKEN_VOID, TOKEN_BOOL, TOKEN_I32, TOKEN_I64, TOKEN_F32, TOKEN_F64, TOKEN_CHAR,
    TOKEN_WHILE, TOKEN_FOR, TOKEN_RETURN,
    TOKEN_ERROR, TOKEN_EOF,
} tokentype_e;

typedef struct {
    const char *start;
    const char *current;

    int line;
} lexer_t;

typedef struct {
    tokentype_e type;
    const char* start;
    int length;
    int line;
} token_t;

void lexer_init(const char *source);
token_t lexer_token();
lexer_t lexer_snapshot();
void lexer_restore(lexer_t *lexer);

#endif
