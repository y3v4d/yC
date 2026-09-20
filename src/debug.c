#include "debug.h"
#include "lexer.h"

#include <stdio.h>
#include <string.h>

static void print_token_type(tokentype_e type) {
    switch (type) {
    case TOKEN_PLUS:
        printf("TOKEN_PLUS");
        break;
    case TOKEN_MINUS:
        printf("TOKEN_MINUS");
        break;
    case TOKEN_STAR:
        printf("TOKEN_STAR");
        break;
    case TOKEN_SLASH:
        printf("TOKEN_SLASH");
        break;
    case TOKEN_NUMBER:
        printf("TOKEN_NUMBER");
        break;
    case TOKEN_ERROR:
        printf("TOKEN_ERROR");
        break;
    case TOKEN_EOF:
        printf("TOKEN_EOF");
        break;
    case TOKEN_LEFT_PAREN:
        printf("TOKEN_LEFT_PAREN");
        break;
    case TOKEN_RIGHT_PAREN:
        printf("TOKEN_RIGHT_PAREN");
        break;
    case TOKEN_I32:
        printf("TOKEN_I32");
        break;
    case TOKEN_I64:
        printf("TOKEN_I64");
        break;
    case TOKEN_F32:
        printf("TOKEN_F32");
        break;
    case TOKEN_F64:
        printf("TOKEN_F64");
        break;
    case TOKEN_IDENTIFIER:
        printf("TOKEN_IDENTIFIER");
        break;
    case TOKEN_EQUAL:
        printf("TOKEN_EQUAL");
        break;
    case TOKEN_SEMICOLON:
        printf("TOKEN_SEMICOLON");
        break;
    case TOKEN_PRINT:
        printf("TOKEN_PRINT");
        break;
    case TOKEN_LEFT_BRACE:
        printf("TOKEN_LEFT_BRACE");
        break;
    case TOKEN_RIGHT_BRACE:
        printf("TOKEN_RIGHT_BRACE");
        break;
    case TOKEN_COMMA:
        printf("TOKEN_COMMA");
        break;
    case TOKEN_IF:
        printf("TOKEN_IF");
        break;
    case TOKEN_ELSE:
        printf("TOKEN_ELSE");
        break;
    case TOKEN_BOOL:
        printf("TOKEN_BOOL");
        break;
    case TOKEN_TRUE:
        printf("TOKEN_TRUE");
        break;
    case TOKEN_FALSE:
        printf("TOKEN_FALSE");
        break;
    case TOKEN_AMPERSAND:
        printf("TOKEN_AMPERSAND");
        break;
    case TOKEN_STRUCT:
        printf("TOKEN_STRUCT");
        break;
    case TOKEN_DOT:
        printf("TOKEN_DOT");
        break;
    case TOKEN_CHAR:
        printf("TOKEN_CHAR");
        break;
    case TOKEN_VOID:
        printf("TOKEN_VOID");
        break;
    case TOKEN_CHAR_LITERAL:
        printf("TOKEN_CHAR_LITERAL");
        break;
    case TOKEN_STRING_LITERAL:
        printf("TOKEN_STRING_LITERAL");
        break;
    case TOKEN_LBRACKET:
        printf("TOKEN_LBRACKET");
        break;
    case TOKEN_RBRACKET:
        printf("TOKEN_RBRACKET");
        break;
    default:
        printf("UNKNOWN_TOKEN");
        break;
    }
}

void print_token(token_t token) {
    print_token_type(token.type);
    printf(" (line %d): '", token.line);
    for (int i = 0; i < token.length; i++) {
        putchar(token.start[i]);
    }

    printf("'\n");
}
