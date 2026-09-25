#include "lexer.h"
#include <stdbool.h>
#include <string.h>

lexer_t lexer;

void lexer_init(const char *source) {
    lexer.start = source;
    lexer.current = source;
    lexer.line = 1;
}

lexer_t lexer_snapshot() { return lexer; }
void lexer_restore(lexer_t *snapshot) { lexer = *snapshot; }

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool is_at_end() { return *lexer.current == '\0'; }
static char advance() { return *lexer.current++; }
static char peek() { return *lexer.current; }
static char peek_next() {
    if (is_at_end())
        return '\0';

    return *(lexer.current + 1);
}

static tokentype_e check_keyword(int start, int length, const char *rest,
                                 tokentype_e type) {
    if (lexer.current - lexer.start == start + length &&
        memcmp(lexer.start + start, rest, length) == 0) {
        return type;
    }

    return TOKEN_IDENTIFIER;
}

static void skip_whitespace() {
    while (true) {
        char c = peek();
        switch (c) {
        case ' ':
        case '\r':
        case '\t':
            advance();
            break;
        case '\n':
            lexer.line++;
            advance();
            break;
        default:
            return;
        }
    }
}

static token_t token_create(tokentype_e type) {
    token_t token;

    token.type = type;
    token.start = lexer.start;
    token.length = (int)(lexer.current - lexer.start);
    token.line = lexer.line;

    return token;
}

static token_t token_number() {
    if (peek() == '-') {
        advance();
    }

    while (is_digit(peek())) {
        advance();
    }

    if (peek() == '.' && is_digit(peek_next())) {
        advance(); // consume the '.'

        while (is_digit(peek())) {
            advance();
        }
    }

    return token_create(TOKEN_NUMBER);
}

static token_t token_character() {
    if (peek() == '\\') {
        advance(); // consume the '\'
        if (peek() == 'n' || peek() == 't' || peek() == '\\' ||
            peek() == '\'') {
            advance(); // consume the escape character
        } else {
            return token_create(TOKEN_ERROR); // invalid escape sequence
        }
    } else {
        advance(); // consume the character
    }

    advance(); // consume the closing '
    return token_create(TOKEN_CHAR_LITERAL);
}

static token_t token_string() {
    while (peek() != '"' && !is_at_end()) {
        if (peek() == '\n') {
            lexer.line++;
        }
        advance();
    }

    if (is_at_end()) {
        return token_create(TOKEN_ERROR); // Unterminated string
    }

    advance(); // Consume the closing "
    return token_create(TOKEN_STRING_LITERAL);
}

static tokentype_e identifier_type() {
    switch (lexer.start[0]) {
    case 'i':
        if (lexer.current - lexer.start > 1) {
            switch (lexer.start[1]) {
            case 'f':
                return check_keyword(2, 0, "", TOKEN_IF);
            case '3':
                return check_keyword(2, 1, "2", TOKEN_I32);
            case '6':
                return check_keyword(2, 1, "4", TOKEN_I64);
            }
        }
    case 'f':
        if (lexer.current - lexer.start > 1) {
            switch (lexer.start[1]) {
            case 'a':
                return check_keyword(2, 3, "lse", TOKEN_FALSE);
            case '3':
                return check_keyword(2, 1, "2", TOKEN_F32);
            case '6':
                return check_keyword(2, 1, "4", TOKEN_F64);
            case 'o':
                return check_keyword(2, 1, "r", TOKEN_FOR);
            }
        }

        return TOKEN_IDENTIFIER;
    case 't':
        return check_keyword(1, 3, "rue", TOKEN_TRUE);
    case 'p':
        return check_keyword(1, 4, "rint", TOKEN_PRINT);
    case 'e':
        return check_keyword(1, 3, "lse", TOKEN_ELSE);
    case 'b':
        return check_keyword(1, 3, "ool", TOKEN_BOOL);
    case 's':
        if (lexer.current - lexer.start > 1) {
            switch (lexer.start[1]) {
            case 't':
                return check_keyword(2, 4, "ruct", TOKEN_STRUCT);
            case 'i':
                return check_keyword(2, 4, "zeof", TOKEN_SIZEOF);
            }
        }

        return TOKEN_IDENTIFIER;
    case 'v':
        return check_keyword(1, 3, "oid", TOKEN_VOID);
    case 'c':
        return check_keyword(1, 3, "har", TOKEN_CHAR);
    case 'w':
        return check_keyword(1, 4, "hile", TOKEN_WHILE);
    case 'r':
        return check_keyword(1, 5, "eturn", TOKEN_RETURN);
    default:
        return TOKEN_IDENTIFIER;
    }
}

static token_t token_identifier() {
    while (is_alpha(peek()) || is_digit(peek())) {
        advance();
    }

    return token_create(identifier_type());
}

token_t lexer_token() {
    skip_whitespace();
    lexer.start = lexer.current;

    if (is_at_end())
        return token_create(TOKEN_EOF);

    char c = advance();
    if (is_digit(c)) {
        return token_number();
    } else if (is_alpha(c)) {
        return token_identifier();
    }

    switch (c) {
    case '+':
        return token_create(TOKEN_PLUS);
    case '-':
        if (is_digit(peek())) {
            token_t n = token_number();
            return n;
        }

        return token_create(TOKEN_MINUS);
    case '*':
        return token_create(TOKEN_STAR);
    case '/': {
        if (peek() == '/') {
            while (peek() != '\n' && !is_at_end()) {
                advance();
            }

            return lexer_token();
        }

        if (peek() == '*') {
            advance(); // consume the '*'
            while (true) {
                if (is_at_end()) {
                    return token_create(TOKEN_ERROR);
                }

                if (peek() == '*' && peek_next() == '/') {
                    advance(); // consume the '*'
                    advance(); // consume the '/'
                    break;
                }

                if (peek() == '\n') {
                    lexer.line++;
                }

                advance();
            }

            return lexer_token();
        }

        return token_create(TOKEN_SLASH);
    }
    case '(':
        return token_create(TOKEN_LEFT_PAREN);
    case ')':
        return token_create(TOKEN_RIGHT_PAREN);
    case '=':
        return token_create(TOKEN_EQUAL);
    case ';':
        return token_create(TOKEN_SEMICOLON);
    case '{':
        return token_create(TOKEN_LEFT_BRACE);
    case '}':
        return token_create(TOKEN_RIGHT_BRACE);
    case ',':
        return token_create(TOKEN_COMMA);
    case '&':
        return token_create(TOKEN_AMPERSAND);
    case '.':
        return token_create(TOKEN_DOT);
    case '[':
        return token_create(TOKEN_LBRACKET);
    case ']':
        return token_create(TOKEN_RBRACKET);
    case '!':
        return token_create(TOKEN_BANG);
    case '"':
        return token_string();
    case '\'':
        return token_character();
    default:
        return token_create(TOKEN_ERROR);
    }
}
