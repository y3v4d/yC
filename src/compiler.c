#include "compiler.h"
#include "debug.h"
#include "lexer.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    token_t current;
    token_t previous;

    bool had_error;
} parser_t;

// 32-bit layout for type
// [0..6] - enum type
// [7] - pointer flag
// [8-31] - data (TYPE_STRUCT - index, other - unspecified)

typedef enum {
    TYPE_UNKNOWN = 0,
    TYPE_VOID,
    TYPE_BOOL,
    TYPE_I32,
    TYPE_I64,
    TYPE_F32,
    TYPE_F64,
    TYPE_STRUCT,

    TYPE_POINTER_FLAG = 0b10000000,

    TYPE_BASE_MASK = 0b01111111,
    TYPE_DATA_MASK = 0xFFFFFF00,

    TYPE_MAX_VALUE = INT32_MAX // 32-bit integer max value
} type_e;

typedef enum {
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_GET_VAR,
    OP_SET_VAR,
    OP_GET_FIELD,
    OP_SET_FIELD,
    OP_VAR_ADDRESS,
    OP_GET_ADDRESS_VALUE,
    OP_SET_ADDRESS_VALUE,
    OP_COPY_TEMP,
    OP_CONST,
    OP_CONVERT,
    OP_CALL,
    OP_IF,
    OP_ELSE,
    OP_END,
    OP_PRINT
} optype_e;

typedef struct {
    optype_e type;
    type_e value_type;

    union {
        struct {
            type_e from;
            type_e to;
        } convert;

        struct {
            int var_index;
            int field_index;
        } field;

        int temp_offset;
        int const_value;
        int var_index;
    } as;
} op_t;

typedef struct {
    token_t name;
    type_e type;

    int structdef_index;

    bool is_addressed;
    bool is_param;

    int frame_index;
    int depth;
} local_t;

typedef struct {
    token_t name;
    type_e type;
} fielddef_t;

typedef struct {
    token_t name;

    fielddef_t *fields;
    int field_count;
    int field_capacity;
} structdef_t;

typedef struct {
    op_t *ops;
    int op_count;
    int op_capcity;

    char *buffer;
    char *current;

    int temp_max;

    local_t locals[256];
    int local_count;

    structdef_t structdefs[256];
    int structdef_count;

    local_t to_hoist[256];
    int hoist;

    int scope_depth;
} compiler_t;

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT, // =
    PREC_TERM,       // + -
    PREC_FACTOR,     // * /
    PREC_UNARY,      // - (unary)
    PREC_CALL,       // ()
} prec_e;

typedef void (*parsefn_t)();

typedef struct {
    parsefn_t prefix;
    parsefn_t infix;
    prec_e precedence;
} parserule_t;

parser_t parser;
compiler_t compiler;

static parserule_t *get_rule(tokentype_e type);
static void declaration();

static void compiler_init() {
    compiler.ops = NULL;
    compiler.op_count = 0;
    compiler.op_capcity = 0;

    compiler.buffer = malloc(1024 * 1024);
    compiler.current = compiler.buffer;
    compiler.local_count = 0;
    compiler.scope_depth = 0;
    compiler.hoist = 0;
}

static void compiler_free() {
    if (compiler.ops != NULL)
        free(compiler.ops);

    if (compiler.buffer != NULL)
        free(compiler.buffer);
}

static void error(const char *message) {
    fprintf(stderr, "[line %d] Error: %s\n", parser.current.line, message);
    parser.had_error = true;
}

static token_t advance() {
    parser.previous = parser.current;

    while (true) {
        parser.current = lexer_token();
        if (parser.current.type != TOKEN_ERROR)
            break;

        parser.had_error = true;
    }

#ifdef DEBUG_OUTPUT
    print_token(parser.current);
#endif

    return parser.current;
}

static bool check(tokentype_e type) { return parser.current.type == type; }

static bool match(tokentype_e type) {
    if (parser.current.type != type)
        return false;

    advance();
    return true;
}

static void consume(tokentype_e type, const char *message) {
    if (parser.current.type == type) {
        advance();
        return;
    }

    error(message);
}

static type_e create_type(type_e base_type, bool is_pointer, int data) {
    type_e type = base_type & TYPE_BASE_MASK;

    if (is_pointer) {
        type |= TYPE_POINTER_FLAG;
    }

    type |= (data << 8) & TYPE_DATA_MASK;

    return type;
}

static void set_type_data(type_e *type, int data) {
    *type &= ~TYPE_DATA_MASK;              // Clear existing data
    *type |= (data << 8) & TYPE_DATA_MASK; // Set new data
}

static type_e token_to_type(tokentype_e type, bool is_pointer) {
    type_e base_type;

    switch (type) {
    case TOKEN_I32:
        base_type = TYPE_I32;
        break;
    case TOKEN_I64:
        base_type = TYPE_I64;
        break;
    case TOKEN_F32:
        base_type = TYPE_F32;
        break;
    case TOKEN_F64:
        base_type = TYPE_F64;
        break;
    case TOKEN_BOOL:
        base_type = TYPE_BOOL;
        break;
    case TOKEN_VOID:
        base_type = TYPE_VOID;
        break;
    default:
        return TYPE_UNKNOWN;
    }

    return create_type(base_type, is_pointer, 0);
}

static bool is_type(type_e type, type_e check) {
    return (type & TYPE_BASE_MASK) == check;
}

static bool is_pointer(type_e type) { return (type & TYPE_POINTER_FLAG); }

static int get_type_data(type_e type) { return (type & TYPE_DATA_MASK) >> 8; }

static int calculate_struct_size(structdef_t *structdef) {
    int size = 0;
    for (int i = 0; i < structdef->field_count; i++) {
        type_e field_type = structdef->fields[i].type;

        if (is_pointer(field_type)) {
            size += 4; // Assuming pointer size is 4 bytes
        } else {
            switch (field_type & TYPE_BASE_MASK) {
            case TYPE_I32:
                size += 4;
                break;
            case TYPE_I64:
                size += 8;
                break;
            case TYPE_F32:
                size += 4;
                break;
            case TYPE_F64:
                size += 8;
                break;
            case TYPE_BOOL:
                size += 4;
                break;
            case TYPE_STRUCT: {
                int structdef_index = get_type_data(field_type);
                structdef_t *nested_structdef =
                    &compiler.structdefs[structdef_index];
                size += calculate_struct_size(nested_structdef);

                break;
            }
            default:
                error("Unknown field type in struct.");
                return -1;
            }
        }
    }
    return size;
}

static type_e match_type() {
    token_t current = parser.current;
    bool matched_primitive = false;

    switch (current.type) {
    case TOKEN_VOID:
    case TOKEN_I32:
    case TOKEN_I64:
    case TOKEN_F32:
    case TOKEN_F64:
    case TOKEN_BOOL:
        matched_primitive = true;
        break;
    default:
        matched_primitive = false;
        break;
    }

    structdef_t *structdef = NULL;
    if (!matched_primitive) {
        for (int i = 0; i < compiler.structdef_count; i++) {
            if (compiler.structdefs[i].name.length == current.length &&
                strncmp(compiler.structdefs[i].name.start, current.start,
                        current.length) == 0) {
                structdef = &compiler.structdefs[i];
                break;
            }
        }

        if (structdef == NULL) {
            return TYPE_UNKNOWN;
        }
    }

    advance();

    bool is_pointer_type = match(TOKEN_STAR);
    return matched_primitive ? token_to_type(current.type, is_pointer_type)
                             : create_type(TYPE_STRUCT, is_pointer_type,
                                           structdef - compiler.structdefs);
}

static void write_op(op_t op) {
    if (compiler.op_count >= compiler.op_capcity) {
        compiler.op_capcity =
            compiler.op_capcity < 8 ? 8 : compiler.op_capcity * 2;
        compiler.ops =
            realloc(compiler.ops, sizeof(op_t) * compiler.op_capcity);
    }

    compiler.ops[compiler.op_count++] = op;
}

static void print_op(op_t op) {
    switch (op.type) {
    case OP_ADD:
        printf("OP_ADD\n");
        break;
    case OP_SUB:
        printf("OP_SUB\n");
        break;
    case OP_MUL:
        printf("OP_MUL\n");
        break;
    case OP_DIV:
        printf("OP_DIV\n");
        break;
    case OP_GET_VAR:
        printf("OP_GET_VAR %d (base_type=%d)\n", op.as.var_index,
               op.value_type & TYPE_BASE_MASK);
        break;
    case OP_GET_FIELD:
        printf("OP_GET_FIELD %d.%d\n", op.as.field.var_index,
               op.as.field.field_index);
        break;
    case OP_SET_FIELD:
        printf("OP_SET_FIELD %d.%d\n", op.as.field.var_index,
               op.as.field.field_index);
        break;
    case OP_SET_VAR:
        printf("OP_SET_VAR %d\n", op.as.var_index);
        break;
    case OP_VAR_ADDRESS:
        printf("OP_VAR_ADDRESS %d\n", op.as.var_index);
        break;
    case OP_CONST:
        printf("OP_CONST %d\n", op.as.const_value);
        break;
    case OP_CALL:
        printf("OP_CALL %d\n", op.as.var_index);
        break;
    case OP_IF:
        printf("OP_IF\n");
        break;
    case OP_ELSE:
        printf("OP_ELSE\n");
        break;
    case OP_END:
        printf("OP_END\n");
        break;
    case OP_PRINT:
        printf("OP_PRINT\n");
        break;
    case OP_GET_ADDRESS_VALUE:
        printf("OP_GET_ADDRESS_VALUE\n");
        break;
    case OP_SET_ADDRESS_VALUE:
        printf("OP_SET_ADDRESS_VALUE\n");
        break;
    case OP_CONVERT:
        printf("OP_CONVERT from %d to %d\n", op.as.convert.from,
               op.as.convert.to);
        break;
    case OP_COPY_TEMP:
        printf("OP_COPY_TEMP offset %d\n", op.as.temp_offset);
        break;
    default:
        printf("UNKNOWN_OP\n");
        break;
    }
}

static void write_string(const char *data) {
    char c;
    while ((c = *data++) != '\0') {
        *compiler.current++ = c;
    }
}

static void write_type(type_e type) {
    if (is_pointer(type)) {
        write_string("i32"); // Representing pointers as i32 in WebAssembly
        return;
    }

    switch (type) {
    case TYPE_I32:
        write_string("i32");
        break;
    case TYPE_I64:
        write_string("i64");
        break;
    case TYPE_F32:
        write_string("f32");
        break;
    case TYPE_F64:
        write_string("f64");
        break;
    case TYPE_BOOL:
        write_string("i32"); // Representing bool as i32 in WebAssembly
        break;
    case TYPE_STRUCT:
        write_string(
            "i32"); // Representing structs as i32 (pointer) in WebAssembly
        break;
    default:
        error("Expected a primitive type.");
        break;
    }
}

static void write_token(token_t token) {
    for (int i = 0; i < token.length; i++) {
        *compiler.current++ = token.start[i];
    }
}

static void expression(prec_e precedence) {
    advance();
    parsefn_t prefix_rule = get_rule(parser.previous.type)->prefix;
    if (prefix_rule == NULL) {
        error("Expected an expression.");
        return;
    }

    prefix_rule();

    while (precedence <= get_rule(parser.current.type)->precedence) {
        advance();

        parsefn_t infix_rule = get_rule(parser.previous.type)->infix;
        if (infix_rule == NULL) {
            error("Expected an infix operator.");
            return;
        }

        infix_rule();
    }
}

static void number() {
    write_op((op_t){.type = OP_CONST,
                    .value_type = TYPE_I32,
                    .as.const_value = atoi(parser.previous.start)});
}

static void boolean() {
    switch (parser.previous.type) {
    case TOKEN_TRUE:
        write_op((op_t){
            .type = OP_CONST, .value_type = TYPE_BOOL, .as.const_value = 1});
        break;
    case TOKEN_FALSE:
        write_op((op_t){
            .type = OP_CONST, .value_type = TYPE_BOOL, .as.const_value = 0});
        break;
    default:
        error("Expected a boolean literal.");
        break;
    }
}

static bool is_numeric_type(type_e type) {
    return type == TYPE_I32 || type == TYPE_I64 || type == TYPE_F32 ||
           type == TYPE_F64 || is_pointer(type);
}

static void binary() {
    tokentype_e op_type = parser.previous.type;
    prec_e precedence = get_rule(op_type)->precedence;

    int a_index = compiler.op_count - 1;
    expression((prec_e)(precedence + 1));

    op_t *a = &compiler.ops[a_index];
    op_t *b = &compiler.ops[compiler.op_count - 1];

    if (!is_numeric_type(a->value_type) || !is_numeric_type(b->value_type)) {
        error("Operands must be numeric types.");
        return;
    }

    type_e target_type = is_pointer(a->value_type) ? TYPE_I32 : a->value_type;
    type_e b_type = is_pointer(b->value_type) ? TYPE_I32 : b->value_type;

    if (target_type != b_type) {
        write_op((op_t){.type = OP_CONVERT,
                        .as.convert.from = b_type,
                        .as.convert.to = target_type});
    }

    switch (op_type) {
    case TOKEN_PLUS:
        write_op((op_t){.type = OP_ADD, .value_type = target_type});
        break;
    case TOKEN_MINUS:
        write_op((op_t){.type = OP_SUB, .value_type = target_type});
        break;
    case TOKEN_STAR:
        write_op((op_t){.type = OP_MUL, .value_type = target_type});
        break;
    case TOKEN_SLASH:
        write_op((op_t){.type = OP_DIV, .value_type = target_type});
        break;
    default:
        error("Expected a binary operator.");
        break;
    }
}

static void unary() {
    tokentype_e op_type = parser.previous.type;

    expression(PREC_UNARY);

    op_t *last_op = &compiler.ops[compiler.op_count - 1];
    if (!is_numeric_type(last_op->value_type)) {
        error("Operand must be a numeric type.");
        return;
    }

    switch (op_type) {
    case TOKEN_MINUS:
        write_op((op_t){.type = OP_CONST,
                        .value_type = last_op->value_type,
                        .as.const_value = -1});
        write_op((op_t){.type = OP_MUL, .value_type = last_op->value_type});
        break;
    default:
        error("Expected a unary operator.");
        break;
    }
}

static void group() {
    expression(PREC_ASSIGNMENT);
    if (!match(TOKEN_RIGHT_PAREN)) {
        error("Expected ')' after expression.");
    }
}

static void identifier() {
    local_t *found = NULL;
    for (int i = compiler.local_count - 1; i >= 0; --i) {
        if (compiler.locals[i].name.length == parser.previous.length &&
            strncmp(compiler.locals[i].name.start, parser.previous.start,
                    parser.previous.length) == 0) {
            found = &compiler.locals[i];
            break;
        }
    }

    if (found == NULL) {
        error("Undefined variable.");
        return;
    }

    if (check(TOKEN_DOT)) {
        if (found->type != TYPE_STRUCT) {
            error("Variable is not a struct.");
            return;
        }

        structdef_t *structdef = &compiler.structdefs[found->structdef_index];

        advance();
        consume(TOKEN_IDENTIFIER, "Expected field name after '.'.");

        token_t field_name = parser.previous;
        fielddef_t *field = NULL;

        for (int i = 0; i < structdef->field_count; i++) {
            if (structdef->fields[i].name.length == field_name.length &&
                strncmp(structdef->fields[i].name.start, field_name.start,
                        field_name.length) == 0) {
                field = &structdef->fields[i];
                break;
            }
        }

        if (field == NULL) {
            error("Undefined field in struct.");
            return;
        }

        if (check(TOKEN_EQUAL)) {
            advance();
            expression(PREC_ASSIGNMENT);

            op_t *last_op = &compiler.ops[compiler.op_count - 1];
            if (last_op->value_type != field->type) {
                write_op((op_t){.type = OP_CONVERT,
                                .as.convert.from = last_op->value_type,
                                .as.convert.to = field->type});
            }

            write_op((op_t){.type = OP_SET_FIELD,
                            .value_type = field->type,
                            .as.field.var_index = found - compiler.locals,
                            .as.field.field_index = field - structdef->fields});
        } else {
            write_op((op_t){.type = OP_GET_FIELD,
                            .value_type = field->type,
                            .as.field.var_index = found - compiler.locals,
                            .as.field.field_index = field - structdef->fields});
        }
    } else if (check(TOKEN_LEFT_PAREN)) { // function call
        advance();

        int temp_offset = 0;
        while (!check(TOKEN_RIGHT_PAREN) && parser.current.type != TOKEN_EOF) {
            expression(PREC_ASSIGNMENT);
            if (!check(TOKEN_RIGHT_PAREN)) {
                consume(TOKEN_COMMA, "Expected ',' between arguments.");
            }

            op_t *last_op = &compiler.ops[compiler.op_count - 1];
            if (is_type(last_op->value_type, TYPE_STRUCT) &&
                !is_pointer(last_op->value_type)) {
                int structdef_index = get_type_data(last_op->value_type);
                structdef_t *structdef = &compiler.structdefs[structdef_index];
                int size = calculate_struct_size(structdef);

                write_op((op_t){.type = OP_COPY_TEMP,
                                .value_type = last_op->value_type,
                                .as.temp_offset = temp_offset});

                temp_offset += size;
            }
        }

        if (temp_offset > compiler.temp_max) {
            compiler.temp_max = temp_offset;
        }

        write_op((op_t){.type = OP_CALL,
                        .value_type = found->type,
                        .as.var_index = found - compiler.locals});

        consume(TOKEN_RIGHT_PAREN, "Expected ')' after function call.");
    } else if (check(TOKEN_EQUAL)) { // variable assignment
        advance();
        expression(PREC_ASSIGNMENT);

        op_t *last_op = &compiler.ops[compiler.op_count - 1];
        if (last_op->value_type != found->type) {
            write_op((op_t){.type = OP_CONVERT,
                            .as.convert.from = last_op->value_type,
                            .as.convert.to = found->type});
        }

        write_op((op_t){.type = OP_SET_VAR,
                        .value_type = found->type,
                        .as.var_index = found - compiler.locals});
    } else { // variable access
        write_op((op_t){.type = OP_GET_VAR,
                        .value_type = found->type,
                        .as.var_index = found - compiler.locals});
    }
}

static void begin_scope() { compiler.scope_depth++; }

static void end_scope() { compiler.scope_depth--; }

static void block() {
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        declaration();
    }

    consume(TOKEN_RIGHT_BRACE, "Expected '}' after block.");
}

static void address() {
    consume(TOKEN_IDENTIFIER, "Expected variable name after '&'.");

    local_t *found = NULL;
    for (int i = compiler.local_count - 1; i >= 0; --i) {
        if (compiler.locals[i].name.length == parser.previous.length &&
            strncmp(compiler.locals[i].name.start, parser.previous.start,
                    parser.previous.length) == 0) {
            found = &compiler.locals[i];
            break;
        }
    }

    if (found == NULL) {
        error("Undefined variable.");
        return;
    }

    found->is_addressed = true;

    write_op((op_t){.type = OP_VAR_ADDRESS,
                    .value_type = found->type | TYPE_POINTER_FLAG,
                    .as.var_index = found - compiler.locals});
}

static void dereference() {
    consume(TOKEN_IDENTIFIER, "Expected variable name after '*'.");

    local_t *found = NULL;
    for (int i = compiler.local_count - 1; i >= 0; --i) {
        if (compiler.locals[i].name.length == parser.previous.length &&
            strncmp(compiler.locals[i].name.start, parser.previous.start,
                    parser.previous.length) == 0) {
            found = &compiler.locals[i];
            break;
        }
    }

    if (found == NULL) {
        error("Undefined variable.");
        return;
    }

    if (!is_pointer(found->type)) {
        error("Variable is not a pointer.");
        return;
    }

    if (match(TOKEN_EQUAL)) {
        write_op((op_t){.type = OP_GET_VAR,
                        .as.var_index = found - compiler.locals});

        expression(PREC_ASSIGNMENT);

        write_op((op_t){.type = OP_SET_ADDRESS_VALUE});
    } else {
        write_op((op_t){.type = OP_GET_VAR,
                        .value_type = found->type,
                        .as.var_index = found - compiler.locals});

        write_op((op_t){.type = OP_GET_ADDRESS_VALUE,
                        .value_type = found->type & ~TYPE_POINTER_FLAG});
    }
}

parserule_t rules[] = {
    [TOKEN_LEFT_PAREN] = {group, NULL, PREC_CALL},
    [TOKEN_RIGHT_PAREN] = {NULL, NULL, PREC_NONE},
    [TOKEN_NUMBER] = {number, NULL, PREC_NONE},
    [TOKEN_PLUS] = {NULL, binary, PREC_TERM},
    [TOKEN_MINUS] = {unary, binary, PREC_TERM},
    [TOKEN_STAR] = {dereference, binary, PREC_FACTOR},
    [TOKEN_SLASH] = {NULL, binary, PREC_FACTOR},
    [TOKEN_IDENTIFIER] = {identifier, NULL, PREC_NONE},
    [TOKEN_AMPERSAND] = {address, NULL, PREC_NONE},
    [TOKEN_I32] = {NULL, NULL, PREC_NONE},
    [TOKEN_I64] = {NULL, NULL, PREC_NONE},
    [TOKEN_F32] = {NULL, NULL, PREC_NONE},
    [TOKEN_F64] = {NULL, NULL, PREC_NONE},
    [TOKEN_IF] = {NULL, NULL, PREC_NONE},
    [TOKEN_ELSE] = {NULL, NULL, PREC_NONE},
    [TOKEN_LEFT_BRACE] = {NULL, NULL, PREC_NONE},
    [TOKEN_RIGHT_BRACE] = {NULL, NULL, PREC_NONE},
    [TOKEN_COMMA] = {NULL, NULL, PREC_NONE},
    [TOKEN_SEMICOLON] = {NULL, NULL, PREC_NONE},
    [TOKEN_PRINT] = {NULL, NULL, PREC_NONE},
    [TOKEN_EQUAL] = {NULL, NULL, PREC_NONE},
    [TOKEN_ERROR] = {NULL, NULL, PREC_NONE},
    [TOKEN_BOOL] = {NULL, NULL, PREC_NONE},
    [TOKEN_TRUE] = {boolean, NULL, PREC_NONE},
    [TOKEN_FALSE] = {boolean, NULL, PREC_NONE},
    [TOKEN_EOF] = {NULL, NULL, PREC_NONE},
    [TOKEN_STRUCT] = {NULL, NULL, PREC_NONE},
    [TOKEN_DOT] = {NULL, NULL, PREC_NONE},
};

static parserule_t *get_rule(tokentype_e type) { return &rules[type]; }

static void print_statement() {
    expression(PREC_ASSIGNMENT);
    consume(TOKEN_SEMICOLON, "Expected ';' after declaration.");

    op_t *last_op = &compiler.ops[compiler.op_count - 1];
    write_op((op_t){.type = OP_PRINT, .value_type = last_op->value_type});
}

static void if_statement() {
    consume(TOKEN_LEFT_PAREN, "Expected '(' after 'if'.");
    expression(PREC_ASSIGNMENT);
    consume(TOKEN_RIGHT_PAREN, "Expected ')' after condition.");

    write_op((op_t){.type = OP_IF});

    consume(TOKEN_LEFT_BRACE, "Expected '{' after 'if' condition.");

    begin_scope();
    block();
    end_scope();

    write_op((op_t){.type = OP_END});

    if (match(TOKEN_ELSE)) {
        consume(TOKEN_LEFT_BRACE, "Expected '{' after 'else'.");
        write_op((op_t){.type = OP_ELSE});

        begin_scope();
        block();
        end_scope();

        write_op((op_t){.type = OP_END});
    }

    write_op((op_t){.type = OP_END});
}

static void named_var(type_e type) {
    token_t name = parser.previous;

    local_t *local = &compiler.locals[compiler.local_count];
    local->name = name;
    local->type = type;
    local->is_addressed = false;
    local->is_param = false;
    local->depth = compiler.scope_depth;

    compiler.local_count++;

    if (match(TOKEN_EQUAL)) {
        expression(PREC_ASSIGNMENT);

        op_t *last_op = &compiler.ops[compiler.op_count - 1];

        if (last_op->value_type != type) {
            write_op((op_t){.type = OP_CONVERT,
                            .as.convert.from = last_op->value_type,
                            .as.convert.to = type});
        }

        write_op((op_t){.type = OP_SET_VAR,
                        .as.var_index = local - compiler.locals});
    }
}

static void structdef_var(int def_index) {
    consume(TOKEN_IDENTIFIER, "Expected field name in struct definition.");

    token_t field_name = parser.previous;
    consume(TOKEN_SEMICOLON, "Expected ';' after field name.");

    local_t *local = &compiler.locals[compiler.local_count];
    local->name = field_name;
    local->type = TYPE_STRUCT;
    local->structdef_index = def_index;

    local->is_addressed = false;
    local->is_param = false;
    local->depth = compiler.scope_depth;

    compiler.local_count++;
}

static void function(type_e return_type) {
    if (compiler.scope_depth > 0) {
        error("Functions cannot be declared inside other functions or blocks.");
        return;
    }

    token_t name = parser.previous;
    consume(TOKEN_LEFT_PAREN, "Expected '(' after function name.");

    local_t *local = &compiler.locals[compiler.local_count];
    local->name = name;
    local->type = return_type;
    local->is_addressed = false;
    local->depth = compiler.scope_depth;
    compiler.local_count++;

    write_string("(func $");
    write_token(name);
    write_string(" ");

    compiler.temp_max = 0;
    int frame_ptr = compiler.local_count;

    // Parse parameters
    while (!check(TOKEN_RIGHT_PAREN) && !check(TOKEN_EOF)) {
        type_e param_type;
        if ((param_type = match_type())) {
            consume(TOKEN_IDENTIFIER, "Expected parameter name.");
            token_t param_name = parser.previous;

            write_string("(param $");
            write_token(param_name);
            write_string(" ");
            write_type(param_type);
            write_string(")");

            local_t *local = &compiler.locals[compiler.local_count];
            local->name = param_name;
            local->type = param_type;
            local->is_addressed = false;
            local->is_param = true;

            compiler.local_count++;
        } else {
            error("Expected a type for parameter.");
        }

        if (!check(TOKEN_RIGHT_PAREN)) {
            consume(TOKEN_COMMA, "Expected ',' between parameters.");
        }
    }

    if (return_type != TYPE_VOID) {
        write_string(" (result ");
        write_type(return_type);
        write_string(")");
    }

    write_string("\n");

    consume(TOKEN_RIGHT_PAREN, "Expected ')' after function parameters.");
    consume(TOKEN_LEFT_BRACE, "Expected '{' before function body.");

    begin_scope();
    block();
    end_scope();

    int stack_length = 0;
    for (int i = compiler.local_count - 1; i >= frame_ptr; --i) {
        local_t *local = &compiler.locals[i];
        if (local->is_addressed) {
            local->frame_index = stack_length;
            stack_length += 4;
        } else if (!local->is_param) {
            if (local->type == TYPE_STRUCT) {
                int size = 0;
                structdef_t *structdef =
                    &compiler.structdefs[local->structdef_index];
                for (int j = 0; j < structdef->field_count; j++) {
                    fielddef_t *field = &structdef->fields[j];

                    if (is_pointer(field->type)) {
                        size += 4;
                    } else if (field->type == TYPE_I32 ||
                               field->type == TYPE_F32 ||
                               field->type == TYPE_BOOL) {
                        size += 4;
                    } else if (field->type == TYPE_I64 ||
                               field->type == TYPE_F64) {
                        size += 8;
                    } else {
                        error("Unsupported field type in struct.");
                    }
                }

                local->frame_index = stack_length;
                stack_length += size;
            } else {
                write_string("(local $");
                write_token(local->name);
                write_string(" ");
                write_type(local->type);
                write_string(")\n");
            }
        }
    }

    int temp_base = stack_length;
    stack_length += compiler.temp_max;

    write_string("(local $_scratch i32)\n");
    write_string("(local $_scratch_64 i64)\n");

    if (stack_length > 0) {
        write_string("global.get $__sp\n");
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "i32.const %d\n", stack_length);
        write_string(buffer);
        write_string("i32.sub\n");
        write_string("global.set $__sp\n");
    }

    for (int i = 0; i < compiler.op_count; i++) {
        print_op(compiler.ops[i]);
        switch (compiler.ops[i].type) {
        case OP_COPY_TEMP: { // pops last value on wasm-stack, copied to temp
                             // and pushes address of temp on wasm-stack
            op_t *op = &compiler.ops[i];
            if (is_type(op->value_type, TYPE_STRUCT) &&
                !is_pointer(op->value_type)) {
                int structdef_index = get_type_data(op->value_type);
                structdef_t *structdef = &compiler.structdefs[structdef_index];
                int size = calculate_struct_size(structdef);

                int target_offset = temp_base + op->as.temp_offset;
                write_string(";; Copy struct to temp\n");
                write_string(
                    "local.set $_scratch\n"); // store struct address in scratch
                write_string("global.get $__sp\n");
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "i32.const %d\n",
                         target_offset);
                write_string(buffer);
                write_string("i32.add\n");
                write_string("local.get $_scratch\n");
                write_string("i32.const ");
                snprintf(buffer, sizeof(buffer), "%d\n", size);
                write_string(buffer);
                write_string("memory.copy\n");
                write_string("global.get $__sp\n");
                snprintf(buffer, sizeof(buffer), "i32.const %d\n",
                         target_offset);
                write_string(buffer);
                write_string("i32.add\n");
            } else {
                error("OP_COPY_TEMP is only supported for struct types.");
            }

            break;
        }
        case OP_ADD:
            if (compiler.ops[i].value_type == TYPE_I64) {
                write_string("i64.add\n");
            } else if (compiler.ops[i].value_type == TYPE_I32) {
                write_string("i32.add\n");
            } else {
                error("Unsupported type for addition.");
            }

            break;
        case OP_SUB:
            if (compiler.ops[i].value_type == TYPE_I64) {
                write_string("i64.sub\n");
            } else if (compiler.ops[i].value_type == TYPE_I32) {
                write_string("i32.sub\n");
            } else {
                error("Unsupported type for subtraction.");
            }

            break;
        case OP_MUL:
            if (compiler.ops[i].value_type == TYPE_I64) {
                write_string("i64.mul\n");
            } else if (compiler.ops[i].value_type == TYPE_I32) {
                write_string("i32.mul\n");
            } else {
                error("Unsupported type for multiplication.");
            }

            break;
        case OP_DIV:
            if (compiler.ops[i].value_type == TYPE_I64) {
                write_string("i64.div_s\n");
            } else if (compiler.ops[i].value_type == TYPE_I32) {
                write_string("i32.div_s\n");
            } else {
                error("Unsupported type for division.");
            }

            break;
        case OP_SET_FIELD: {
            op_t *op = &compiler.ops[i];
            local_t *local = &compiler.locals[op->as.field.var_index];
            structdef_t *structdef =
                &compiler.structdefs[local->structdef_index];

            int byte_offset = 0;
            for (int j = 0; j < op->as.field.field_index; j++) {
                fielddef_t *prev_field = &structdef->fields[j];
                if (is_pointer(prev_field->type)) {
                    byte_offset += 4;
                } else if (prev_field->type == TYPE_I32 ||
                           prev_field->type == TYPE_F32 ||
                           prev_field->type == TYPE_BOOL) {
                    byte_offset += 4;
                } else if (prev_field->type == TYPE_I64 ||
                           prev_field->type == TYPE_F64) {
                    byte_offset += 8;
                } else {
                    error("Unsupported field type in struct.");
                }
            }

            if (op->value_type == TYPE_I64)
                write_string("local.set $_scratch_64\n");
            else
                write_string("local.set $_scratch\n");

            if (local->is_param) {
                write_string("local.get $");
                write_token(local->name);
                write_string("\n");
                write_string("i32.const ");
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d\n", byte_offset);
                write_string(buffer);
                write_string("i32.add\n");
            } else {
                write_string("global.get $__sp\n");
                write_string("i32.const ");
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d\n",
                         local->frame_index + byte_offset);
                write_string(buffer);
                write_string("i32.add\n");
            }

            if (op->value_type == TYPE_I64)
                write_string("local.get $_scratch_64\n");
            else
                write_string("local.get $_scratch\n");

            write_type(op->value_type);
            write_string(".store\n");

            break;
        }
        case OP_GET_FIELD: {
            local_t *local =
                &compiler.locals[compiler.ops[i].as.field.var_index];
            structdef_t *structdef =
                &compiler.structdefs[local->structdef_index];

            int byte_offset = 0;
            for (int j = 0; j < compiler.ops[i].as.field.field_index; j++) {
                fielddef_t *prev_field = &structdef->fields[j];
                if (is_pointer(prev_field->type)) {
                    byte_offset += 4;
                } else if (prev_field->type == TYPE_I32 ||
                           prev_field->type == TYPE_F32 ||
                           prev_field->type == TYPE_BOOL) {
                    byte_offset += 4;
                } else if (prev_field->type == TYPE_I64 ||
                           prev_field->type == TYPE_F64) {
                    byte_offset += 8;
                } else {
                    error("Unsupported field type in struct.");
                }
            }

            if (local->is_param) {
                write_string("local.get $");
                write_token(local->name);
                write_string("\n");
                write_string("i32.const ");
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d\n", byte_offset);
                write_string(buffer);
                write_string("i32.add\n");
                write_type(compiler.ops[i].value_type);
                write_string(".load\n");
            } else {
                write_string("global.get $__sp\n");
                write_string("i32.const ");
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d\n",
                         local->frame_index + byte_offset);
                write_string(buffer);
                write_string("i32.add\n");
                write_type(compiler.ops[i].value_type);
                write_string(".load\n");
            }

            break;
        }
        case OP_GET_VAR: {
            local_t *local = &compiler.locals[compiler.ops[i].as.var_index];
            if (is_type(local->type, TYPE_STRUCT)) {
                write_string(";; Load struct variable from stack\n");
                write_string("global.get $__sp\n");
                write_string("i32.const ");
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d\n", local->frame_index);
                write_string(buffer);
                write_string("i32.add\n");
            } else if (local->is_addressed) {
                write_string(";; Load variable from stack\n");
                write_string("global.get $__sp\n");
                write_string("i32.const ");
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d\n", local->frame_index);
                write_string(buffer);
                write_string("i32.add\n");
                write_string("i32.load\n");
            } else {
                write_string("local.get $");
                write_token(local->name);
                write_string("\n");
            }
            break;
        }
        case OP_SET_VAR: {
            local_t *local = &compiler.locals[compiler.ops[i].as.var_index];
            if (local->is_addressed) {
                write_string(";; Store variable to stack\n");
                write_string("local.set $_scratch\n");

                write_string("global.get $__sp\n");
                write_string("i32.const ");
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d\n", local->frame_index);
                write_string(buffer);
                write_string("i32.add\n");
                write_string("local.get $_scratch\n");

                write_string("i32.store\n");
            } else {
                write_string("local.set $");
                write_token(local->name);
                write_string("\n");
            }
            break;
        }
        case OP_VAR_ADDRESS: {
            local_t *local = &compiler.locals[compiler.ops[i].as.var_index];
            if (local->is_addressed) {
                write_string(";; Get address of variable on stack\n");
                write_string("global.get $__sp\n");
                write_string("i32.const ");
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d\n", local->frame_index);
                write_string(buffer);
                write_string("i32.add\n");
            } else {
                error("Variable is not addressed.");
            }
            break;
        }
        case OP_GET_ADDRESS_VALUE:
            write_string("i32.load\n");
            break;
        case OP_SET_ADDRESS_VALUE: {
            write_string("i32.store\n");
            break;
        }
        case OP_CONST:
            write_string("i32.const ");
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d",
                     compiler.ops[i].as.const_value);
            write_string(buffer);
            write_string("\n");
            break;
        case OP_CONVERT: {
            op_t *op = &compiler.ops[i];
            if (op->as.convert.from == TYPE_I32 &&
                op->as.convert.to == TYPE_I64) {
                write_string("i64.extend_i32_s\n");
            } else if (op->as.convert.from == TYPE_I64 &&
                       op->as.convert.to == TYPE_I32) {
                write_string("i32.wrap_i64\n");
            } else {
                error("Unsupported type conversion.");
            }

            break;
        }
        case OP_CALL:
            write_string("call $");
            write_token(compiler.locals[compiler.ops[i].as.var_index].name);
            write_string("\n");
            break;
        case OP_IF:
            write_string("(if\n");
            write_string("(then\n");
            break;
        case OP_ELSE:
            write_string("(else\n");
            break;
        case OP_END:
            write_string(")\n");
            break;
        case OP_PRINT: {
            op_t *op = &compiler.ops[i];
            if (op->value_type == TYPE_I32) {
                write_string("call $print_i32\n");
            } else if (op->value_type == TYPE_I64) {
                write_string("call $print_i64\n");
            } else {
                error("Unsupported type for print.");
            }

            break;
        }
        default:
            error("Unknown operation.");
            break;
        }
    }

    if (stack_length > 0) {
        write_string("global.get $__sp\n");
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "i32.const %d\n", stack_length);
        write_string(buffer);
        write_string("i32.add\n");
        write_string("global.set $__sp\n");
    }

    compiler.op_count = 0;
    compiler.local_count = frame_ptr;
    compiler.temp_max = 0;

    write_string(")\n");
}

static void typed_declaration(type_e decl_type) {
    consume(TOKEN_IDENTIFIER, "Expected variable name.");

    if (check(TOKEN_LEFT_PAREN)) {
        function(decl_type);
    } else {
        named_var(decl_type);
        consume(TOKEN_SEMICOLON, "Expected ';' after declaration.");
    }
}

static void struct_declaration() {
    if (compiler.scope_depth > 0) {
        error("Structs cannot be declared inside functions or blocks.");
        return;
    }

    consume(TOKEN_IDENTIFIER, "Expected struct name.");
    token_t struct_name = parser.previous;

    structdef_t *structdef = &compiler.structdefs[compiler.structdef_count++];
    structdef->name = struct_name;
    structdef->fields = NULL;
    structdef->field_count = 0;
    structdef->field_capacity = 0;

    consume(TOKEN_LEFT_BRACE, "Expected '{' after struct name.");

    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        type_e field_type;
        if ((field_type = match_type())) {
            consume(TOKEN_IDENTIFIER, "Expected field name.");

            if (structdef->field_count >= structdef->field_capacity) {
                structdef->field_capacity = structdef->field_capacity < 8
                                                ? 8
                                                : structdef->field_capacity * 2;
                structdef->fields =
                    realloc(structdef->fields,
                            sizeof(fielddef_t) * structdef->field_capacity);
            }

            token_t field_name = parser.previous;

            fielddef_t *field = &structdef->fields[structdef->field_count++];
            field->name = field_name;
            field->type = field_type;
        } else {
            error("Expected a type for struct field.");
        }

        consume(TOKEN_SEMICOLON, "Expected ';' after struct field.");
    }

    consume(TOKEN_RIGHT_BRACE, "Expected '}' after struct fields.");
}

static void declaration() {
    type_e decl_type;

    if (check(TOKEN_IDENTIFIER)) {
        int found = -1;
        for (int i = 0; i < compiler.structdef_count; ++i) {
            if (compiler.structdefs[i].name.length == parser.current.length &&
                strncmp(compiler.structdefs[i].name.start, parser.current.start,
                        parser.current.length) == 0) {
                found = i;
                break;
            }
        }

        if (found != -1) {
            advance();
            structdef_var(found);

            return;
        }
    }

    if (match(TOKEN_STRUCT)) {
        struct_declaration();
    } else if ((decl_type = match_type())) {
        typed_declaration(decl_type);
    } else {
        if (match(TOKEN_PRINT)) {
            print_statement();
        } else if (match(TOKEN_IF)) {
            if_statement();
        } else {
            expression(PREC_ASSIGNMENT);
            consume(TOKEN_SEMICOLON, "Expected ';' after declaration.");
        }
    }
}

static void module() {
    write_string("(module\n");
    write_string("(import \"core\" \"print\" (func $print_i32 (param i32)))\n");
    write_string("(import \"core\" \"print\" (func $print_i64 (param i64)))\n");
    write_string("(global $__sp (mut i32) (i32.const 4096))\n");
    write_string("(memory $0 1)\n");

    while (parser.current.type != TOKEN_EOF) {
        declaration();
    }

    write_string("(start $main)\n");
    write_string(")");
}

char *compile(const char *source) {
    compiler_free();

    lexer_init(source);
    compiler_init();

    parser.had_error = false;

    advance();
    module();

    *compiler.current = '\0';
    return parser.had_error ? NULL : compiler.buffer;
}
