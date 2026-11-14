#include "compiler.h"
#include "debug.h"
#include "lexer.h"

#include <limits.h>
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
// [0..5] - enum type
// [6] - pointer flag
// [7] - absolute flag - frame_index is absolute instead of relative to current
// frame pointer
// [8-31] - data (TYPE_STRUCT - index, other - unspecified)

typedef enum {
    TYPE_UNKNOWN = 0,
    TYPE_VOID,
    TYPE_BOOL,
    TYPE_I8,
    TYPE_I16,
    TYPE_I32,
    TYPE_I64,
    TYPE_F32,
    TYPE_F64,
    TYPE_CHAR,
    TYPE_STRUCT,
    TYPE_FUN,

    TYPE_POINTER_FLAG = 0b10000000,

    TYPE_BASE_MASK = 0b01111111,
    TYPE_DATA_MASK = 0xFFFFFF00,

    TYPE_MAX_VALUE = INT_MAX // 32-bit integer max value
} type_e;

typedef union {
    int8_t i8;
    int16_t i16;
    int32_t i32;
    int64_t i64;
    float f32;
    double f64;
} const_u;

typedef struct {
    token_t name;
    type_e type;

    bool is_absolute;
    bool is_on_stack;
    bool is_global;
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
    token_t name;

    fielddef_t *params;
    int param_count;

    type_e return_type;
} fundef_t;

typedef enum {
    VTYPE_UNKNOWN = 0,
    VTYPE_CONST = 0b0001,
    VTYPE_LOCAL = 0b0010,
    VTYPE_STACK = 0b0100,
    VTYPE_LVALUE = 0b1000,

    VTYPE_STORAGE = VTYPE_CONST | VTYPE_LOCAL | VTYPE_STACK
} stackvtype_e;

typedef struct {
    stackvtype_e type;
    type_e ctype;

    int offset;

    union {
        const_u c;
        local_t *local;
    } as;
} stackv_t;

typedef struct {
    type_e type;
    bool is_local;
    int offset;
    int size;

    union {
        const_u c;
        const char *string;
        local_t *local;
    } as;
} const_t;

typedef struct {
    char *buffer;
    char *current;

    stackv_t stack[256];
    int stack_count;

    local_t locals[256];
    int local_count;

    structdef_t structdefs[256];
    int structdef_count;

    fundef_t fundefs[256];
    int fundef_count;

    const_t consts[256];
    int const_count;

    int data_offset;
    local_t *frame_ptr;
    int sstack_count;
    int sstack_atleast;

    int scope_depth;
    int loop_counter;
    bool no_op;
    bool used_fp;
} compiler_t;

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT, // =
    PREC_AND,        // &&
    PREC_EQUALITY,   // ==
    PREC_COMPARISON, // < > <= >=
    PREC_TERM,       // + -
    PREC_FACTOR,     // * /
    PREC_UNARY,      // - & (unary)
    PREC_ACCESS,     // . (field access)
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
    compiler.frame_ptr = compiler.locals;

    compiler.buffer = malloc(1024 * 1024);
    compiler.current = compiler.buffer;
    compiler.local_count = 0;
    compiler.stack_count = 0;
    compiler.scope_depth = 0;
    compiler.no_op = false;
    compiler.used_fp = false;

    compiler.const_count = 0;
    compiler.data_offset = 0;
    compiler.loop_counter = 0;
    compiler.sstack_count = 0;

    compiler.structdef_count = 0;
    compiler.fundef_count = 0;
}

static void compiler_free() {
    if (compiler.buffer != NULL)
        free(compiler.buffer);
}

static void error(const char *message) {
    fprintf(stderr, "[line %d] Error: %s\n", parser.current.line, message);
    parser.had_error = true;
}

static void advance() {
    parser.previous = parser.current;

    while (true) {
        parser.current = lexer_token();
        if (parser.current.type != TOKEN_ERROR)
            break;

        parser.had_error = true;
    }

#ifdef DEBUG_OUTPUT_TOKEN
    print_token(parser.previous);
#endif
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

static type_e token_to_type(tokentype_e type, bool is_pointer) {
    type_e base_type;

    switch (type) {
    case TOKEN_I8:
        base_type = TYPE_I8;
        break;
    case TOKEN_I16:
        base_type = TYPE_I16;
        break;
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
    case TOKEN_CHAR:
        base_type = TYPE_CHAR;
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

static bool is_struct(type_e type) {
    return is_type(type, TYPE_STRUCT) && !is_pointer(type);
}

static bool isv_local(stackvtype_e type) { return (type & VTYPE_LOCAL); }
static bool isv_const(stackvtype_e type) { return (type & VTYPE_CONST); }
static bool isv_stack(stackvtype_e type) { return (type & VTYPE_STACK); }
static bool isv_lvalue(stackvtype_e type) { return (type & VTYPE_LVALUE); }

static int get_type_data(type_e type) { return (type & TYPE_DATA_MASK) >> 8; }

static int calculate_struct_size(structdef_t *structdef);

static int get_type_size(type_e type) {
    if (is_pointer(type)) {
        return 4; // Assuming pointer size is 4 bytes
    }

    switch (type & TYPE_BASE_MASK) {
    case TYPE_I8:
        return 1;
    case TYPE_I16:
        return 2;
    case TYPE_I32:
        return 4;
    case TYPE_I64:
        return 8;
    case TYPE_F32:
        return 4;
    case TYPE_F64:
        return 8;
    case TYPE_CHAR:
        return 1;
    case TYPE_BOOL:
        return 1;
    case TYPE_STRUCT: {
        int structdef_index = get_type_data(type);
        structdef_t *structdef = &compiler.structdefs[structdef_index];
        return calculate_struct_size(structdef);
    }
    default:
        error("Unknown type.");
        return -1;
    }
}

static int calculate_struct_size(structdef_t *structdef) {
    int size = 0;
    for (int i = 0; i < structdef->field_count; i++) {
        type_e field_type = structdef->fields[i].type;
        size += get_type_size(field_type);
    }

    return size;
}

static type_e match_type() {
    token_t current = parser.current;
    bool matched_primitive = false;

    switch (current.type) {
    case TOKEN_VOID:
    case TOKEN_I8:
    case TOKEN_I16:
    case TOKEN_I32:
    case TOKEN_I64:
    case TOKEN_F32:
    case TOKEN_F64:
    case TOKEN_BOOL:
    case TOKEN_CHAR:
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

static void print_const(type_e type, const_u c) {
    if (is_pointer(type)) {
        printf("%d", c.i32);
        return;
    }

    switch (type & TYPE_BASE_MASK) {
    case TYPE_I8:
        printf("%d", c.i8);
        break;
    case TYPE_I16:
        printf("%d", c.i16);
        break;
    case TYPE_BOOL:
    case TYPE_CHAR:
    case TYPE_I32:
        printf("%d", c.i32);
        break;
    case TYPE_I64:
        printf("%lld", c.i64);
        break;
    case TYPE_F32:
        printf("%f", c.f32);
        break;
    case TYPE_F64:
        printf("%f", c.f64);
        break;
    default:
        printf("Unknown constant type\n");
        break;
    }
}

static const_u cast_const(const_u self, type_e from, type_e to) {
    const_u result = {0};

    if (is_pointer(to) && is_pointer(from)) {
        result.i32 = self.i32;
    } else if (is_pointer(to)) {
        if (is_type(from, TYPE_I32) || is_type(from, TYPE_CHAR) ||
            is_type(from, TYPE_BOOL)) {
            result.i32 = self.i32;
        } else if (is_type(from, TYPE_I64)) {
            result.i32 = (int)self.i64;
        } else if (is_type(from, TYPE_I8)) {
            result.i32 = (int)self.i8;
        } else if (is_type(from, TYPE_I16)) {
            result.i32 = (int)self.i16;
        } else if (is_type(from, TYPE_F32)) {
            error("Cannot cast from f32 to pointer.");
        } else if (is_type(from, TYPE_F64)) {
            error("Cannot cast from f64 to pointer.");
        } else {
            error("Unsupported type for cast_const.");
        }
    } else if (is_pointer(from)) {
        if (is_type(to, TYPE_I32)) {
            result.i32 = self.i32;
        } else if (is_type(to, TYPE_I64)) {
            result.i64 = (int64_t)self.i32;
        } else if (is_type(to, TYPE_I8)) {
            result.i8 = (int8_t)self.i32;
        } else if (is_type(to, TYPE_I16)) {
            result.i16 = (int16_t)self.i16;
        } else if (is_type(to, TYPE_CHAR)) {
            result.i32 = (char)self.i32;
        } else if (is_type(to, TYPE_BOOL)) {
            result.i32 = self.i32 != 0;
        } else if (is_type(to, TYPE_F32)) {
            error("Cannot cast from pointer to f32.");
        } else if (is_type(to, TYPE_F64)) {
            error("Cannot cast from pointer to f64.");
        } else {
            error("Unsupported type for cast_const.");
        }
    } else if (is_type(to, TYPE_I32)) {
        if (is_type(from, TYPE_I32) || is_type(from, TYPE_CHAR) ||
            is_type(from, TYPE_BOOL)) {
            result.i32 = self.i32;
        } else if (is_type(from, TYPE_I64)) {
            result.i32 = (int)self.i64;
        } else if (is_type(from, TYPE_I8)) {
            result.i32 = (int)self.i8;
        } else if (is_type(from, TYPE_I16)) {
            result.i32 = (int)self.i16;
        } else if (is_type(from, TYPE_F32)) {
            result.i32 = (int)self.f32;
        } else if (is_type(from, TYPE_F64)) {
            result.i32 = (int)self.f64;
        } else {
            error("Unsupported type for cast_const.");
        }
    } else if (is_type(to, TYPE_I64)) {
        if (is_type(from, TYPE_I32) || is_type(from, TYPE_CHAR) ||
            is_type(from, TYPE_BOOL)) {
            result.i64 = (int64_t)self.i32;
        } else if (is_type(from, TYPE_I8)) {
            result.i64 = (int64_t)self.i8;
        } else if (is_type(from, TYPE_I16)) {
            result.i64 = (int64_t)self.i16;
        } else if (is_type(from, TYPE_F32)) {
            result.i64 = (int64_t)self.f32;
        } else if (is_type(from, TYPE_F64)) {
            result.i64 = (int64_t)self.f64;
        } else {
            error("Unsupported type for cast_const.");
        }
    } else if (is_type(to, TYPE_I8)) {
        if (is_type(from, TYPE_I32) || is_type(from, TYPE_CHAR) ||
            is_type(from, TYPE_BOOL)) {
            result.i8 = (int8_t)self.i32;
        } else if (is_type(from, TYPE_I8)) {
            result.i8 = self.i8;
        } else if (is_type(from, TYPE_I16)) {
            result.i8 = (int8_t)self.i16;
        } else if (is_type(from, TYPE_F32)) {
            result.i8 = (int8_t)self.f32;
        } else if (is_type(from, TYPE_F64)) {
            result.i8 = (int8_t)self.f64;
        } else {
            error("Unsupported type for cast_const.");
        }
    } else if (is_type(to, TYPE_I16)) {
        if (is_type(from, TYPE_I32) || is_type(from, TYPE_CHAR) ||
            is_type(from, TYPE_BOOL)) {
            result.i16 = (int16_t)self.i32;
        } else if (is_type(from, TYPE_I8)) {
            result.i16 = (int16_t)self.i8;
        } else if (is_type(from, TYPE_I16)) {
            result.i16 = self.i16;
        } else if (is_type(from, TYPE_F32)) {
            result.i16 = (int16_t)self.f32;
        } else if (is_type(from, TYPE_F64)) {
            result.i16 = (int16_t)self.f64;
        } else {
            error("Unsupported type for cast_const.");
        }
    } else if (is_type(to, TYPE_F32)) {
        if (is_type(from, TYPE_I32) || is_type(from, TYPE_CHAR) ||
            is_type(from, TYPE_BOOL)) {
            result.f32 = (float)self.i32;
        } else if (is_type(from, TYPE_I64)) {
            result.f32 = (float)self.i64;
        } else if (is_type(from, TYPE_I8)) {
            result.f32 = (float)self.i8;
        } else if (is_type(from, TYPE_I16)) {
            result.f32 = (float)self.i16;
        } else if (is_type(from, TYPE_F32)) {
            result.f32 = self.f32;
        } else if (is_type(from, TYPE_F64)) {
            result.f32 = (float)self.f64;
        } else {
            error("Unsupported type for cast_const.");
        }
    } else if (is_type(to, TYPE_F64)) {
        if (is_type(from, TYPE_I32) || is_type(from, TYPE_CHAR) ||
            is_type(from, TYPE_BOOL)) {
            result.f64 = (double)self.i32;
        } else if (is_type(from, TYPE_I64)) {
            result.f64 = (double)self.i64;
        } else if (is_type(from, TYPE_I8)) {
            result.f64 = (double)self.i8;
        } else if (is_type(from, TYPE_I16)) {
            result.f64 = (double)self.i16;
        } else if (is_type(from, TYPE_F32)) {
            result.f64 = (double)self.f32;
        } else if (is_type(from, TYPE_F64)) {
            result.f64 = self.f64;
        } else {
            error("Unsupported type for cast_const.");
        }
    } else if (is_type(to, TYPE_CHAR)) {
        if (is_type(from, TYPE_CHAR)) {
            result.i32 = self.i32;
        } else if (is_type(from, TYPE_I32) || is_type(from, TYPE_BOOL)) {
            result.i32 = (char)self.i32;
        } else if (is_type(from, TYPE_I64)) {
            result.i32 = (char)self.i64;
        } else if (is_type(from, TYPE_I8)) {
            result.i32 = (char)self.i8;
        } else if (is_type(from, TYPE_I16)) {
            result.i32 = (char)self.i16;
        } else if (is_type(from, TYPE_F32)) {
            result.i32 = (char)self.f32;
        } else if (is_type(from, TYPE_F64)) {
            result.i32 = (char)self.f64;
        } else {
            error("Unsupported type for cast_const.");
        }
    } else if (is_type(to, TYPE_BOOL)) {
        if (is_type(from, TYPE_BOOL)) {
            result.i32 = self.i32;
        } else if (is_type(from, TYPE_I32) || is_type(from, TYPE_CHAR)) {
            result.i32 = self.i32 != 0;
        } else if (is_type(from, TYPE_I64)) {
            result.i32 = self.i64 != 0;
        } else if (is_type(from, TYPE_I8)) {
            result.i32 = self.i8 != 0;
        } else if (is_type(from, TYPE_I16)) {
            result.i32 = self.i16 != 0;
        } else if (is_type(from, TYPE_F32)) {
            result.i32 = self.f32 != 0.0f;
        } else if (is_type(from, TYPE_F64)) {
            result.i32 = self.f64 != 0.0;
        } else {
            error("Unsupported type for cast_const.");
        }
    } else {
        error("Unsupported type for cast_const.");
    }

    return result;
}

static void write_string(const char *data) {
    char c;

    while ((c = *data++) != '\0') {
        *compiler.current++ = c;
    }
}

static void write_string_length(const char *data, int length) {
    for (int i = 0; i < length; i++) {
        *compiler.current++ = data[i];
    }
}

static void write_type(type_e type) {
    if (is_pointer(type)) {
        write_string("i32"); // Representing pointers as i32 in WebAssembly
        return;
    }

    switch (type & TYPE_BASE_MASK) {
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_CHAR:
    case TYPE_BOOL:
    case TYPE_STRUCT:
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
    default:
        error("Expected a type.");
        break;
    }
}

static void write_token(token_t token) {
    for (int i = 0; i < token.length; i++) {
        *compiler.current++ = token.start[i];
    }
}

static void write_int(int value) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%d", value);
    write_string(buffer);
}

static void write_long(long value) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%ld", value);
    write_string(buffer);
}

static void write_float(float value) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%f", value);
    write_string(buffer);
}

static void write_double(double value) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%f", value);
    write_string(buffer);
}

static void write_const(const_u c, type_e type) {
    if (is_pointer(type)) {
        write_int(c.i32);
        return;
    }

    switch (type & TYPE_BASE_MASK) {
    case TYPE_BOOL:
    case TYPE_CHAR:
    case TYPE_I32:
        write_int(c.i32);
        break;
    case TYPE_I64:
        write_long(c.i64);
        break;
    case TYPE_I8:
        write_int(c.i8);
        break;
    case TYPE_I16:
        write_int(c.i16);
        break;
    case TYPE_F32:
        write_float(c.f32);
        break;
    case TYPE_F64:
        write_double(c.f64);
        break;
    default:
        error("Expected a primitive type.");
        break;
    }
}

static void write_i32_const(int value) {
    write_string("i32.const ");
    write_int(value);
    write_string("\n");
}

static void iwrite_const(type_e type, const_u c) {
    write_type(type);
    write_string(".const ");
    write_const(c, type);
    write_string("\n");
}

static void iwrite_load(type_e type) {
    if (is_struct(type)) {
        // structs are represented as addresses, since address is
        // expected to be on the stack at this point, we don't need to
        // do anything here
        return;
    }

    write_type(type);
    if (is_type(type, TYPE_CHAR) || is_type(type, TYPE_BOOL)) {
        write_string(".load8_u\n");
    } else if (is_type(type, TYPE_I8)) {
        write_string(".load8_s\n");
    } else if (is_type(type, TYPE_I16)) {
        write_string(".load16_s\n");
    } else {
        write_string(".load\n");
    }
}

static void iwrite_store(type_e type) {
    if (is_struct(type)) {
        write_i32_const(
            calculate_struct_size(&compiler.structdefs[get_type_data(type)]));
        write_string("memory.copy\n");
    } else {
        write_type(type);

        if (is_type(type, TYPE_CHAR) || is_type(type, TYPE_I8) ||
            is_type(type, TYPE_BOOL)) {
            write_string(".store8\n");
        } else if (is_type(type, TYPE_I16)) {
            write_string(".store16\n");
        } else {
            write_string(".store\n");
        }
    }
}

static void iwrite_add(type_e type) {
    write_type(type);
    write_string(".add\n");
}

static void iwrite_sub(type_e type) {
    write_type(type);
    write_string(".sub\n");
}

static void iwrite_mul(type_e type) {
    write_type(type);
    write_string(".mul\n");
}

static void iwrite_div(type_e type) {
    write_type(type);

    if (is_type(type, TYPE_I32) || is_type(type, TYPE_I64) ||
        is_type(type, TYPE_I8) || is_type(type, TYPE_I16)) {
        write_string(".div_s\n");
    } else if (is_type(type, TYPE_F32) || is_type(type, TYPE_F64)) {
        write_string(".div\n");
    } else {
        error("Unsupported type for division.");
    }
}

static void iwrite_negate() { write_string("i32.eqz\n"); }

static void iwrite_call(token_t name) {
    write_string("call $");
    write_token(name);
    write_string("\n");
}

static void iwrite_if() { write_string("(if\n(then\n"); }

static void iwrite_ifr(type_e type) {
    write_string("(if (result ");
    write_type(type);
    write_string(")\n(then\n");
}

static void iwrite_else() { write_string("(else\n"); }

static void iwrite_end() { write_string(")\n"); }

static void iwrite_loop(int index) {
    write_string("(loop $label");
    write_int(index);
    write_string("\n");
}

static void iwrite_block(int index) {
    write_string("(block $label");
    write_int(index);
    write_string("\n");
}

static void iwrite_jmp(int index) {
    write_string("br $label");
    write_int(index);
    write_string("\n");
}

static void iwrite_jmpif(int index) {
    write_string("br_if $label");
    write_int(index);
    write_string("\n");
}

static void iwrite_equal() { write_string("i32.eq\n"); }

static void iwrite_nequal() { write_string("i32.ne\n"); }

static void iwrite_greater() { write_string("i32.gt_s\n"); }

static void iwrite_less() { write_string("i32.lt_s\n"); }

static void iwrite_greater_equal() { write_string("i32.ge_s\n"); }

static void iwrite_less_equal() { write_string("i32.le_s\n"); }

static void iwrite_eqz() { write_string("i32.eqz\n"); }

static void iwrite_return() { write_string("return\n"); }

static bool write_convert(type_e from, type_e to) {
    if (is_pointer(from) && is_pointer(to)) {
        // no conversion needed
    } else if (is_pointer(from)) {
        if (is_type(to, TYPE_I32)) {
            // no conversion needed
        } else if (is_type(to, TYPE_I64)) {
            write_string("i64.extend_i32_s\n");
        } else if (is_type(to, TYPE_I8)) {
            write_string("i32.extend8_s\n");
        } else if (is_type(to, TYPE_I16)) {
            write_string("i32.extend16_s\n");
        } else if (is_type(to, TYPE_F32)) {
            error("Cannot convert pointer to f32.");
        } else if (is_type(to, TYPE_F64)) {
            error("Cannot convert pointer to f64.");
        } else if (is_type(to, TYPE_CHAR)) {
            write_string("i32.extend8_s\n");
        } else if (is_type(to, TYPE_BOOL)) {
            write_string("i32.const 0\n");
            write_string("i32.ne\n");
        } else {
            error("Unsupported type conversion.");
            return false;
        }
    } else if (is_pointer(to)) {
        if (is_type(from, TYPE_I32) || is_type(from, TYPE_I8) ||
            is_type(from, TYPE_I16) || is_type(from, TYPE_BOOL) ||
            is_type(from, TYPE_CHAR)) {
            // no conversion needed
        } else if (is_type(from, TYPE_I64)) {
            write_string("i32.wrap_i64\n");
        } else if (is_type(from, TYPE_F32)) {
            error("Cannot convert f32 to pointer.");
            return false;
        } else if (is_type(from, TYPE_F64)) {
            error("Cannot convert f64 to pointer.");
            return false;
        } else {
            error("Unsupported type conversion.");
            return false;
        }
    } else if (is_type(to, TYPE_I8)) {
        if (is_type(from, TYPE_I32) || is_type(from, TYPE_I16)) {
            write_string("i32.extend8_s\n");
        } else if (is_type(from, TYPE_I64)) {
            write_string("i32.wrap_i64\n");
            write_string("i32.extend8_s\n");
        } else if (is_type(from, TYPE_F32)) {
            write_string("i32.trunc_f32_s\n");
            write_string("i32.extend8_s\n");
        } else if (is_type(from, TYPE_F64)) {
            write_string("i32.trunc_f64_s\n");
            write_string("i32.extend8_s\n");
        } else if (is_type(from, TYPE_CHAR) || is_type(from, TYPE_BOOL) ||
                   is_type(from, TYPE_I8)) {
            // no conversion needed
        } else {
            error("Unsupported type conversion.");
            return false;
        }
    } else if (is_type(to, TYPE_I16)) {
        if (is_type(from, TYPE_I32)) {
            write_string("i32.extend16_s\n");
        } else if (is_type(from, TYPE_I64)) {
            write_string("i32.wrap_i64\n");
            write_string("i32.extend16_s\n");
        } else if (is_type(from, TYPE_F32)) {
            write_string("i32.trunc_f32_s\n");
            write_string("i32.extend16_s\n");
        } else if (is_type(from, TYPE_F64)) {
            write_string("i32.trunc_f64_s\n");
            write_string("i32.extend16_s\n");
        } else if (is_type(from, TYPE_CHAR) || is_type(from, TYPE_BOOL) ||
                   is_type(from, TYPE_I8) || is_type(from, TYPE_I16)) {
            // no conversion needed
        } else {
            error("Unsupported type conversion.");
            return false;
        }
    } else if (is_type(to, TYPE_I32)) {
        if (is_type(from, TYPE_I32) || is_type(from, TYPE_I8) ||
            is_type(from, TYPE_I16) || is_type(from, TYPE_BOOL) ||
            is_type(from, TYPE_CHAR)) {
            // no conversion needed
        } else if (is_type(from, TYPE_I64)) {
            write_string("i32.wrap_i64\n");
        } else if (is_type(from, TYPE_F32)) {
            write_string("i32.trunc_f32_s\n");
        } else if (is_type(from, TYPE_F64)) {
            write_string("i32.trunc_f64_s\n");
        } else {
            error("Unsupported type conversion.");
            return false;
        }
    } else if (is_type(to, TYPE_I64)) {
        if (is_type(from, TYPE_I32) || is_type(from, TYPE_I8) ||
            is_type(from, TYPE_I16) || is_type(from, TYPE_BOOL) ||
            is_type(from, TYPE_CHAR)) {
            write_string("i64.extend_i32_s\n");
        } else if (is_type(from, TYPE_I64)) {
            // no conversion needed
        } else if (is_type(from, TYPE_F32)) {
            write_string("i64.trunc_f32_s\n");
        } else if (is_type(from, TYPE_F64)) {
            write_string("i64.trunc_f64_s\n");
        } else {
            error("Unsupported type conversion.");
            return false;
        }
    } else if (is_type(to, TYPE_F32)) {
        if (is_type(from, TYPE_I32) || is_type(from, TYPE_I8) ||
            is_type(from, TYPE_I16) || is_type(from, TYPE_BOOL) ||
            is_type(from, TYPE_CHAR)) {
            write_string("f32.convert_i32_s\n");
        } else if (is_type(from, TYPE_I64)) {
            write_string("f32.convert_i64_s\n");
        } else if (is_type(from, TYPE_F32)) {
            // no conversion needed
        } else if (is_type(from, TYPE_F64)) {
            write_string("f32.demote_f64\n");
        } else {
            error("Unsupported type conversion.");
            return false;
        }
    } else if (is_type(to, TYPE_F64)) {
        if (is_type(from, TYPE_I32) || is_type(from, TYPE_I8) ||
            is_type(from, TYPE_I16) || is_type(from, TYPE_BOOL) ||
            is_type(from, TYPE_CHAR)) {
            write_string("f64.convert_i32_s\n");
        } else if (is_type(from, TYPE_I64)) {
            write_string("f64.convert_i64_s\n");
        } else if (is_type(from, TYPE_F32)) {
            write_string("f64.promote_f32\n");
        } else if (is_type(from, TYPE_F64)) {
            // no conversion needed
        } else {
            error("Unsupported type conversion.");
            return false;
        }
    } else if (is_type(to, TYPE_BOOL)) {
        if (is_type(from, TYPE_I32) || is_type(from, TYPE_I8) ||
            is_type(from, TYPE_I16) || is_type(from, TYPE_CHAR)) {
            write_string("i32.const 0\n");
            write_string("i32.ne\n");
        } else if (is_type(from, TYPE_I64)) {
            write_string("i64.const 0\n");
            write_string("i64.ne\n");
        } else if (is_type(from, TYPE_F32)) {
            write_string("f32.const 0.0\n");
            write_string("f32.ne\n");
        } else if (is_type(from, TYPE_F64)) {
            write_string("f64.const 0.0\n");
            write_string("f64.ne\n");
        } else if (is_type(from, TYPE_BOOL)) {
            // no conversion needed
        } else {
            error("Unsupported type conversion.");
            return false;
        }
    } else if (is_type(to, TYPE_CHAR)) {
        if (is_type(from, TYPE_I32) || is_type(from, TYPE_I16)) {
            write_string("i32.extend8_s\n");
        } else if (is_type(from, TYPE_I64)) {
            write_string("i32.wrap_i64\n");
            write_string("i32.extend8_s\n");
        } else if (is_type(from, TYPE_F32)) {
            write_string("i32.trunc_f32_s\n");
            write_string("i32.extend8_s\n");
        } else if (is_type(from, TYPE_F64)) {
            write_string("i32.trunc_f64_s\n");
            write_string("i32.extend8_s\n");
        } else if (is_type(from, TYPE_CHAR) || is_type(from, TYPE_BOOL) ||
                   is_type(from, TYPE_I8)) {
            // no conversion needed
        } else {
            error("Unsupported type conversion.");
            return false;
        }
    } else {
        error("Unsupported type conversion.");
        return false;
    }

    return true;
}

static void write_print(type_e type) {
    if (is_pointer(type)) {
        write_string("call $print_i32\n");
    } else if (is_type(type, TYPE_I32) || is_type(type, TYPE_I8) ||
               is_type(type, TYPE_I16) || is_type(type, TYPE_BOOL) ||
               is_type(type, TYPE_CHAR)) {
        write_string("call $print_i32\n");
    } else if (is_type(type, TYPE_I64)) {
        write_string("call $print_i64\n");
    } else if (is_type(type, TYPE_F32)) {
        write_string("call $print_f32\n");
    } else if (is_type(type, TYPE_F64)) {
        write_string("call $print_f64\n");
    } else {
        error("Unsupported type for print.");
    }
}

static void write_address_local(local_t *local, int offset) {
    if (local->is_global) { // for now, only wasm global is __heap_base which
                            // value is an address so let's just get it
        write_string("global.get $");
        write_token(local->name);
        write_string("\n");
    } else if (local->is_absolute) {
        write_i32_const(local->frame_index);
    } else if (local->is_on_stack) {
        write_string("local.get $fp\n");
        offset += local->frame_index;
        compiler.used_fp = true;
    } else if (is_struct(local->type)) {
        write_string("local.get $");
        write_int(local - compiler.frame_ptr);
        write_string("\n");
    } else {
        return;
    }

    if (offset > 0) {
        write_i32_const(offset);
        iwrite_add(TYPE_I32);
    }
}

static void write_load_local(local_t *local, type_e type) {
    bool is_local_struct =
        is_type(local->type, TYPE_STRUCT) && !is_pointer(local->type);
    bool is_op_struct = is_type(type, TYPE_STRUCT) && !is_pointer(type);

    if (is_op_struct) {
        return;
    }

    if (is_local_struct || local->is_on_stack || local->is_global) {
        iwrite_load(type);
    } else {
        write_string("local.get $");
        write_int(local - compiler.frame_ptr);
        write_string("\n");
    }
}

static void write_store_local(local_t *local, type_e type) {
    bool is_local_struct =
        is_type(local->type, TYPE_STRUCT) && !is_pointer(local->type);
    bool is_op_struct = is_type(type, TYPE_STRUCT) && !is_pointer(type);

    if (!local->is_on_stack && !is_local_struct && !is_op_struct) {
        write_string("local.set $");
        write_int(local - compiler.frame_ptr);
        write_string("\n");
    } else {
        iwrite_store(type);
    }
}

// materialize vstack value on the wasm stack
static void materialize_vstack(stackv_t *stackv) {
    if (isv_lvalue(stackv->type)) {
        if (isv_local(stackv->type)) {
            write_address_local(stackv->as.local, stackv->offset);
            write_load_local(stackv->as.local, stackv->ctype);

            stackv->type &= ~VTYPE_STORAGE;
            stackv->type |= VTYPE_STACK;
        } else if (isv_const(stackv->type)) {
            iwrite_const(stackv->ctype, stackv->as.c);
            iwrite_load(stackv->ctype);
        } else if (isv_stack(stackv->type)) {
            if (stackv->offset > 0) {
                iwrite_const(TYPE_I32, (const_u){.i32 = stackv->offset});
                iwrite_add(TYPE_I32);
            }

            // for struct types the address it the value, we just leave the
            // address on the stack
            if (!is_struct(stackv->ctype)) {
                iwrite_load(stackv->ctype);
            }
        }

        stackv->type &= ~VTYPE_LVALUE;
    } else {
        if (isv_const(stackv->type)) {
            iwrite_const(stackv->ctype, stackv->as.c);

            stackv->type &= ~VTYPE_STORAGE;
            stackv->type |= VTYPE_STACK;
        } else if (isv_local(stackv->type)) {
            write_address_local(stackv->as.local, stackv->offset);
        } else if (isv_stack(stackv->type)) {
            if (stackv->offset > 0) {
                iwrite_const(TYPE_I32, (const_u){.i32 = stackv->offset});
                iwrite_add(TYPE_I32);
            }
        }
    }
}

// store vstack value
static void store_vstack(stackv_t *stackv) {
    if (isv_stack(stackv->type)) {
        iwrite_store(stackv->ctype);
    } else {
        write_store_local(stackv->as.local, stackv->ctype);
    }
}

static bool is_int_type(type_e type) {
    return type == TYPE_I8 || type == TYPE_I16 || type == TYPE_I32 ||
           type == TYPE_I64;
}

static bool is_float_type(type_e type) {
    return type == TYPE_F32 || type == TYPE_F64;
}

static bool is_numeric_type(type_e type) {
    return is_int_type(type) || is_float_type(type) || is_pointer(type);
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
    stackv_t *stackv = &compiler.stack[compiler.stack_count++];
    stackv->type = VTYPE_CONST;
    stackv->offset = 0;

    bool has_decimal = false;
    for (int i = 0; i < parser.previous.length; i++) {
        if (parser.previous.start[i] == '.') {
            has_decimal = true;
            break;
        }
    }

    if (!has_decimal) {
        stackv->ctype = TYPE_I32;
        stackv->as.c.i32 = atoi(parser.previous.start);
    } else {
        stackv->ctype = TYPE_F32;
        stackv->as.c.f32 = atof(parser.previous.start);
    }
}

static void boolean() {
    stackv_t *stackv = &compiler.stack[compiler.stack_count++];
    stackv->type = VTYPE_CONST;
    stackv->ctype = TYPE_BOOL;
    stackv->offset = 0;

    switch (parser.previous.type) {
    case TOKEN_TRUE:
        stackv->as.c.i32 = 1;
        break;
    case TOKEN_FALSE:
        stackv->as.c.i32 = 0;
        break;
    default:
        error("Expected a boolean literal.");
        break;
    }
}

static void binary() {
    tokentype_e op_type = parser.previous.type;
    prec_e precedence = get_rule(op_type)->precedence;

    stackv_t *a = &compiler.stack[compiler.stack_count - 1];
    materialize_vstack(a);

    expression((prec_e)(precedence + 1));

    stackv_t *b = &compiler.stack[compiler.stack_count - 1];
    compiler.stack_count -= 2;

    if (!is_numeric_type(a->ctype) || !is_numeric_type(b->ctype)) {
        error("Binary operator requires numeric types.");
        return;
    }

    if (is_pointer(a->ctype) || is_pointer(b->ctype)) {
        if (op_type != TOKEN_PLUS && op_type != TOKEN_MINUS) {
            error("Binary operator can only be applied to pointer types with "
                  "'+' or '-'.");
            return;
        }

        type_e other_type = is_pointer(a->ctype) ? b->ctype : a->ctype;
        if (is_pointer(other_type)) {
            type_e atype = a->ctype & ~TYPE_POINTER_FLAG;
            type_e btype = b->ctype & ~TYPE_POINTER_FLAG;
            if (atype != btype) {
                error("Binary operator cannot be applied to two different "
                      "pointer types.");
                return;
            }
        } else if (!is_int_type(other_type)) {
            error(
                "Pointer arithmetic can only be performed with integer types.");
            return;
        }
    }

    materialize_vstack(b);

    if (!is_pointer(a->ctype) && !is_pointer(b->ctype) &&
        a->ctype != b->ctype) {
        if (isv_const(b->type)) {
            b->as.c = cast_const(b->as.c, b->ctype, a->ctype);
        } else {
            write_convert(b->ctype, a->ctype);
        }
    }

    type_e target_type = is_pointer(b->ctype) ? b->ctype : a->ctype;

    stackv_t *result = &compiler.stack[compiler.stack_count++];
    result->type = VTYPE_STACK;
    result->ctype = target_type;
    result->offset = 0;

    switch (op_type) {
    case TOKEN_PLUS:
        iwrite_add(target_type);
        break;
    case TOKEN_MINUS:
        iwrite_sub(target_type);
        break;
    case TOKEN_STAR:
        iwrite_mul(target_type);
        break;
    case TOKEN_SLASH:
        iwrite_div(target_type);
        break;
    default:
        error("Expected a binary operator.");
        break;
    }
}

static void unary() {
    tokentype_e op_type = parser.previous.type;

    expression(PREC_UNARY);

    stackv_t last_stackv = compiler.stack[--compiler.stack_count];

    stackv_t *result = &compiler.stack[compiler.stack_count++];
    result->type = VTYPE_STACK;
    result->ctype = last_stackv.ctype;
    result->offset = 0;

    switch (op_type) {
    case TOKEN_MINUS:
        if (!is_numeric_type(last_stackv.ctype)) {
            error("Unary '-' operator requires a numeric type.");
            return;
        }

        if (is_pointer(last_stackv.ctype)) {
            error("Unary '-' operator cannot be applied to pointer types.");
            return;
        }

        const_u c = {0};
        if (is_type(last_stackv.ctype, TYPE_I32)) {
            c.i32 = -1;
        } else if (is_type(last_stackv.ctype, TYPE_I64)) {
            c.i64 = -1;
        } else if (is_type(last_stackv.ctype, TYPE_I8)) {
            c.i8 = -1;
        } else if (is_type(last_stackv.ctype, TYPE_I16)) {
            c.i16 = -1.0f;
        } else if (is_type(last_stackv.ctype, TYPE_F32)) {
            c.f32 = -1.0f;
        } else if (is_type(last_stackv.ctype, TYPE_F64)) {
            c.f64 = -1.0;
        } else {
            error("Unsupported type for unary '-' operator.");
            return;
        }

        materialize_vstack(&last_stackv);

        iwrite_const(last_stackv.ctype, c);
        iwrite_mul(last_stackv.ctype);
        break;
    case TOKEN_BANG:
        if (!is_type(last_stackv.ctype, TYPE_BOOL)) {
            if (isv_const(last_stackv.type)) {
                last_stackv.as.c =
                    cast_const(last_stackv.as.c, last_stackv.ctype, TYPE_BOOL);
            } else {
                write_convert(last_stackv.ctype, TYPE_BOOL);
            }
        }

        materialize_vstack(&last_stackv);
        result->ctype = TYPE_BOOL;

        iwrite_negate();
        break;
    default:
        error("Expected a unary operator.");
        break;
    }
}

static void group() {
    type_e type;
    if ((type = match_type())) { // explicit type casting
        consume(TOKEN_RIGHT_PAREN, "Expected ')' after type.");

        expression(PREC_UNARY);

        stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
        if (isv_const(last_stackv->type)) {
            last_stackv->as.c =
                cast_const(last_stackv->as.c, last_stackv->ctype, type);
        } else {
            materialize_vstack(last_stackv);
            write_convert(last_stackv->ctype, type);
        }

        last_stackv->ctype = type;
        return;
    }

    expression(PREC_ASSIGNMENT);
    if (!match(TOKEN_RIGHT_PAREN)) {
        error("Expected ')' after expression.");
    }
}

static void dot() {
    stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
    if (!isv_lvalue(last_stackv->type)) {
        error("Cannot access fields of a non-lvalue.");
        return;
    }

    if (!is_type(last_stackv->ctype, TYPE_STRUCT)) {
        error("Expected a struct type before '.'.");
        return;
    }

    if (is_pointer(last_stackv->ctype)) {
        error("Cannot access fields of a pointer to a struct.");
        return;
    }

    consume(TOKEN_IDENTIFIER, "Expected field name after '.'.");
    token_t field_name = parser.previous;

    structdef_t *structdef =
        &compiler.structdefs[get_type_data(last_stackv->ctype)];
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

    int field_index = field - structdef->fields;
    int byte_offset = 0;
    for (int j = 0; j < field_index; j++) {
        fielddef_t *prev_field = &structdef->fields[j];
        int size = get_type_size(prev_field->type);

        byte_offset += size;
    }

    last_stackv->ctype = field->type;
    last_stackv->offset += byte_offset;
}

static void arrow() {
    stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
    if (!is_pointer(last_stackv->ctype)) {
        error("Cannot dereference a non-pointer type.");
        return;
    }

    if (isv_lvalue(last_stackv->type)) {
        materialize_vstack(last_stackv);
    }

    last_stackv->ctype &= ~TYPE_POINTER_FLAG;
    last_stackv->type |= VTYPE_LVALUE;

    dot();
}

static void call() {
    stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
    if (!isv_local(last_stackv->type)) {
        error("Cannot call a non-local variable.");
        return;
    }

    if (!is_type(last_stackv->ctype, TYPE_FUN)) {
        error("Cannot call a non-function variable.");
        return;
    }

    if (!isv_lvalue(last_stackv->type)) {
        error("Cannot call a non-lvalue function.");
        return;
    }

    if (is_pointer(last_stackv->ctype)) {
        error("Cannot call a pointer to a function.");
        return;
    }

    fundef_t *fundef = &compiler.fundefs[get_type_data(last_stackv->ctype)];

    int current_param = 0;
    int additional_stack = 0;
    while (!check(TOKEN_RIGHT_PAREN) && parser.current.type != TOKEN_EOF) {
        if (current_param >= fundef->param_count) {
            error("Too many arguments in function call.");
            return;
        }

        fielddef_t *fielddef = &fundef->params[current_param];
        int size = get_type_size(fielddef->type);
        int offset = 0;

        if (is_type(fielddef->type, TYPE_STRUCT) &&
            !is_pointer(fielddef->type)) { // prepare for copy
            offset = compiler.sstack_count + additional_stack;

            write_string("local.get $fp\n");
            compiler.used_fp = true;
            if (offset > 0) {
                write_i32_const(offset);
                iwrite_add(TYPE_I32);
            }
        }

        expression(PREC_ASSIGNMENT);
        if (!check(TOKEN_RIGHT_PAREN)) {
            consume(TOKEN_COMMA, "Expected ',' between arguments.");
        }

        stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
        materialize_vstack(last_stackv);

        if (last_stackv->ctype != fielddef->type) {
            if (!write_convert(last_stackv->ctype, fielddef->type)) {
                return;
            }

            last_stackv->ctype = fielddef->type;
        }

        if (is_type(last_stackv->ctype, TYPE_STRUCT) &&
            !is_pointer(last_stackv->ctype)) {
            write_i32_const(size);
            write_string("memory.copy\n");

            write_string("local.get $fp\n");
            if (offset > 0) {
                write_i32_const(offset);
                iwrite_add(TYPE_I32);
            }

            additional_stack += size;
        }

        current_param++;
    }

    if (current_param < fundef->param_count) {
        error("Too few arguments in function call.");
        return;
    }

    if (additional_stack > 0) {
        int new_atleast = compiler.sstack_count + additional_stack;
        if (new_atleast > compiler.sstack_atleast) {
            compiler.sstack_atleast = new_atleast;
        }
    }

    iwrite_call(last_stackv->as.local->name);
    consume(TOKEN_RIGHT_PAREN, "Expected ')' after function call.");
}

static void identifier() {
    local_t *found = NULL;
    for (int i = compiler.local_count - 1; i >= 0; --i) {
        if (compiler.locals[i].depth > compiler.scope_depth) {
            continue; // Skip locals that are out of scope
        }

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

    stackv_t *stackv = &compiler.stack[compiler.stack_count++];
    stackv->type = VTYPE_LOCAL | VTYPE_LVALUE;
    stackv->ctype = found->type;
    stackv->offset = 0;
    stackv->as.local = found;
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
    expression(PREC_UNARY);

    stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
    if (!isv_lvalue(last_stackv->type)) {
        error("Cannot take the address of a non-lvalue.");
        return;
    }

    if (isv_local(last_stackv->type)) {
        local_t *local = last_stackv->as.local;
        if (!local->is_on_stack && !is_pointer(local->type)) {
            error(
                "Cannot take the address of a local variable that is not on "
                "the "
                "stack. Try to add 'mem' keyword to the variable declaration.");
            return;
        }
    }

    last_stackv->type &= ~VTYPE_LVALUE;
    last_stackv->ctype |= TYPE_POINTER_FLAG;
}

static void dereference() {
    expression(PREC_UNARY);

    stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
    if (!is_pointer(last_stackv->ctype)) {
        error("Cannot dereference a non-pointer type.");
        return;
    }

    if (isv_lvalue(last_stackv->type)) {
        materialize_vstack(last_stackv);
    }

    last_stackv->ctype &= ~TYPE_POINTER_FLAG;
    last_stackv->type |= VTYPE_LVALUE;
}

static void assignment() {
    stackv_t *vtarget = &compiler.stack[compiler.stack_count - 1];
    if (!isv_lvalue(vtarget->type)) {
        error("Invalid assignment target.");
        return;
    }

    vtarget->type &= ~VTYPE_LVALUE;
    materialize_vstack(vtarget);

    expression(PREC_ASSIGNMENT);

    stackv_t *vvalue = &compiler.stack[compiler.stack_count - 1];
    compiler.stack_count -= 2; // Pop both the lvalue and the value

    if (vvalue->ctype != vtarget->ctype) {
        if (isv_const(vvalue->type)) {
            vvalue->as.c =
                cast_const(vvalue->as.c, vvalue->ctype, vtarget->ctype);
            vvalue->ctype = vtarget->ctype;
        }
    }

    materialize_vstack(vvalue);

    if (vvalue->ctype != vtarget->ctype) {
        write_convert(vvalue->ctype, vtarget->ctype);
    }

    store_vstack(vtarget);
}

static void character() {
    stackv_t *stackv = &compiler.stack[compiler.stack_count++];
    stackv->type = VTYPE_CONST;
    stackv->ctype = TYPE_CHAR;
    stackv->offset = 0;

    char c = parser.previous.start[1];
    stackv->as.c.i32 = (int)c;
}

static void string() {
    stackv_t *stackv = &compiler.stack[compiler.stack_count++];
    stackv->type = VTYPE_CONST;
    stackv->ctype = TYPE_CHAR | TYPE_POINTER_FLAG;
    stackv->offset = 0;

    const_t *string_const = &compiler.consts[compiler.const_count++];

    string_const->is_local = false;
    string_const->type = TYPE_CHAR | TYPE_POINTER_FLAG;
    string_const->offset = compiler.data_offset;
    string_const->size = parser.previous.length - 2;
    string_const->as.string = parser.previous.start + 1;

    stackv->as.c.i32 = 16384 + compiler.data_offset;
    compiler.data_offset += string_const->size + 1;
}

static void bracket() {
    stackv_t *last_stackv = &compiler.stack[--compiler.stack_count];
    type_e element_type = last_stackv->ctype;
    if (!is_pointer(last_stackv->ctype)) {
        error("Cannot index a non-pointer type.");
        return;
    }

    materialize_vstack(last_stackv);

    expression(PREC_ASSIGNMENT);
    consume(TOKEN_RBRACKET, "Expected ']' after index expression.");

    stackv_t *index_stackv = &compiler.stack[compiler.stack_count - 1];
    if (!is_int_type(index_stackv->ctype)) {
        error("Index must be an integer type.");
        return;
    }

    materialize_vstack(index_stackv);
    write_i32_const(get_type_size(element_type & ~TYPE_POINTER_FLAG));
    iwrite_mul(TYPE_I32);
    iwrite_add(TYPE_I32);

    index_stackv->ctype = element_type & ~TYPE_POINTER_FLAG;
    index_stackv->type |= VTYPE_LVALUE;
}

static void sizeof_prefix() {
    consume(TOKEN_LEFT_PAREN, "Expected '(' after 'sizeof'.");

    type_e type = match_type();
    if (type == TYPE_UNKNOWN) {
        bool last_no_op = compiler.no_op;

        compiler.no_op = true;
        expression(PREC_ASSIGNMENT);
        compiler.no_op = last_no_op;

        stackv_t *last_stackv = &compiler.stack[--compiler.stack_count];
        type = last_stackv->ctype;
    }

    int size = get_type_size(type);

    stackv_t *stackv = &compiler.stack[compiler.stack_count++];
    stackv->type = VTYPE_CONST;
    stackv->ctype = TYPE_I32;
    stackv->offset = 0;
    stackv->as.c.i32 = size;

    consume(TOKEN_RIGHT_PAREN, "Expected ')' after 'sizeof'.");
}

static void equality() {
    tokentype_e op_type = parser.previous.type;

    stackv_t *a = &compiler.stack[compiler.stack_count - 1];
    materialize_vstack(a);

    expression(PREC_EQUALITY + 1);

    stackv_t *b = &compiler.stack[compiler.stack_count - 1];
    compiler.stack_count -= 2;

    materialize_vstack(b);

    stackv_t *result = &compiler.stack[compiler.stack_count++];
    result->type = VTYPE_STACK;
    result->ctype = TYPE_BOOL;
    result->offset = 0;

    switch (op_type) {
    case TOKEN_EQUAL_EQUAL:
        iwrite_equal();
        break;
    case TOKEN_BANG_EQUAL:
        iwrite_nequal();
        break;
    case TOKEN_GREATER:
        iwrite_greater();
        break;
    case TOKEN_LESS:
        iwrite_less();
        break;
    case TOKEN_GREATER_EQUAL:
        iwrite_greater_equal();
        break;
    case TOKEN_LESS_EQUAL:
        iwrite_less_equal();
        break;
    default:
        error("Expected an equality operator.");
        break;
    }
}

static void and() {
    stackv_t *a = &compiler.stack[compiler.stack_count - 1];
    materialize_vstack(a);

    iwrite_ifr(TYPE_BOOL);
    expression(PREC_AND + 1);

    stackv_t *b = &compiler.stack[compiler.stack_count - 1];
    materialize_vstack(b);
    iwrite_end();

    iwrite_else();
    iwrite_const(TYPE_BOOL, (const_u){.i32 = 0});
    iwrite_end();

    iwrite_end();

    compiler.stack_count -= 2;

    stackv_t *result = &compiler.stack[compiler.stack_count++];
    result->type = VTYPE_STACK;
    result->ctype = TYPE_BOOL;
    result->offset = 0;
}

static void or() {
    stackv_t *a = &compiler.stack[compiler.stack_count - 1];
    materialize_vstack(a);

    iwrite_ifr(TYPE_BOOL);
    iwrite_const(TYPE_BOOL, (const_u){.i32 = 1});
    iwrite_end();

    iwrite_else();

    expression(PREC_AND + 1);

    stackv_t *b = &compiler.stack[compiler.stack_count - 1];
    materialize_vstack(b);

    iwrite_end();
    iwrite_end();

    compiler.stack_count -= 2;

    stackv_t *result = &compiler.stack[compiler.stack_count++];
    result->type = VTYPE_STACK;
    result->ctype = TYPE_BOOL;
    result->offset = 0;
}

parserule_t rules[] = {
    [TOKEN_LEFT_PAREN] = {group, call, PREC_CALL},
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
    [TOKEN_EQUAL] = {NULL, assignment, PREC_ASSIGNMENT},
    [TOKEN_ERROR] = {NULL, NULL, PREC_NONE},
    [TOKEN_BOOL] = {NULL, NULL, PREC_NONE},
    [TOKEN_TRUE] = {boolean, NULL, PREC_NONE},
    [TOKEN_FALSE] = {boolean, NULL, PREC_NONE},
    [TOKEN_EOF] = {NULL, NULL, PREC_NONE},
    [TOKEN_STRUCT] = {NULL, NULL, PREC_NONE},
    [TOKEN_DOT] = {NULL, dot, PREC_ACCESS},
    [TOKEN_ARROW] = {NULL, arrow, PREC_ACCESS},
    [TOKEN_CHAR_LITERAL] = {character, NULL, PREC_NONE},
    [TOKEN_STRING_LITERAL] = {string, NULL, PREC_NONE},
    [TOKEN_VOID] = {NULL, NULL, PREC_NONE},
    [TOKEN_LBRACKET] = {NULL, bracket, PREC_ACCESS},
    [TOKEN_RBRACKET] = {NULL, NULL, PREC_NONE},
    [TOKEN_BANG] = {unary, NULL, PREC_UNARY},
    [TOKEN_SIZEOF] = {sizeof_prefix, NULL, PREC_NONE},
    [TOKEN_EQUAL_EQUAL] = {NULL, equality, PREC_EQUALITY},
    [TOKEN_BANG_EQUAL] = {NULL, equality, PREC_EQUALITY},
    [TOKEN_GREATER] = {NULL, equality, PREC_COMPARISON},
    [TOKEN_LESS] = {NULL, equality, PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL] = {NULL, equality, PREC_COMPARISON},
    [TOKEN_LESS_EQUAL] = {NULL, equality, PREC_COMPARISON},
    [TOKEN_AND] = {NULL, and, PREC_AND},
    [TOKEN_OR] = {NULL, or, PREC_AND}};

static parserule_t *get_rule(tokentype_e type) { return &rules[type]; }

static void print_statement() {
    expression(PREC_ASSIGNMENT);
    consume(TOKEN_SEMICOLON, "Expected ';' after declaration.");

    stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
    compiler.stack_count--; // Pop the last stack value

    materialize_vstack(last_stackv);
    write_print(last_stackv->ctype);
}

static void if_statement() {
    consume(TOKEN_LEFT_PAREN, "Expected '(' after 'if'.");
    expression(PREC_ASSIGNMENT);
    consume(TOKEN_RIGHT_PAREN, "Expected ')' after condition.");

    materialize_vstack(&compiler.stack[--compiler.stack_count]);

    iwrite_if();

    consume(TOKEN_LEFT_BRACE, "Expected '{' after 'if' condition.");

    begin_scope();
    block();
    end_scope();

    iwrite_end();

    if (match(TOKEN_ELSE)) {
        consume(TOKEN_LEFT_BRACE, "Expected '{' after 'else'.");
        iwrite_else();

        begin_scope();
        block();
        end_scope();

        iwrite_end();
    }

    iwrite_end();
}

static void while_statement() {
    int loop_label = compiler.loop_counter++;
    int block_label = compiler.loop_counter++;

    iwrite_loop(loop_label);
    iwrite_block(block_label);

    consume(TOKEN_LEFT_PAREN, "Expected '(' after 'while'.");
    expression(PREC_ASSIGNMENT);
    consume(TOKEN_RIGHT_PAREN, "Expected ')' after condition.");

    materialize_vstack(&compiler.stack[--compiler.stack_count]);

    iwrite_negate();
    iwrite_jmpif(block_label);

    consume(TOKEN_LEFT_BRACE, "Expected '{' after 'while' condition.");

    begin_scope();
    block();
    end_scope();

    iwrite_jmp(loop_label);
    iwrite_end();
    iwrite_end();
}

static void for_statement() {
    begin_scope();
    consume(TOKEN_LEFT_PAREN, "Expected '(' after 'for'.");

    if (!check(TOKEN_SEMICOLON)) {
        declaration();
    } else {
        advance(); // Consume the semicolon
    }

    int loop_label = compiler.loop_counter++;
    int block_label = compiler.loop_counter++;

    iwrite_loop(loop_label);
    iwrite_block(block_label);

    if (!check(TOKEN_SEMICOLON)) {
        expression(PREC_ASSIGNMENT);
    }

    stackv_t *last_stackv = &compiler.stack[--compiler.stack_count];
    materialize_vstack(last_stackv);

    iwrite_negate();
    iwrite_jmpif(block_label);

    consume(TOKEN_SEMICOLON, "Expected ';' after loop condition.");

    char *inc_code = NULL;
    int inc_code_length = 0;

    if (!check(TOKEN_RIGHT_PAREN)) {
        char *checkpoint = compiler.current;
        expression(PREC_ASSIGNMENT);

        inc_code_length = compiler.current - checkpoint;
        inc_code = (char *)malloc(inc_code_length);
        if (inc_code == NULL) {
            error("Memory allocation failed for increment code.");
            return;
        }

        memcpy(inc_code, checkpoint, inc_code_length);
        compiler.current = checkpoint;
    }
    consume(TOKEN_RIGHT_PAREN, "Expected ')' after 'for' clauses.");

    consume(TOKEN_LEFT_BRACE, "Expected '{' after 'for' clauses.");
    begin_scope();
    block();
    end_scope();

    if (inc_code_length > 0) {
        memcpy(compiler.current, inc_code, inc_code_length);
        compiler.current += inc_code_length;

        free(inc_code);
    }

    iwrite_jmp(loop_label);
    iwrite_end();
    iwrite_end();

    end_scope();
}

static void named_var(type_e type, bool is_mem) {
    token_t name = parser.previous;
    bool is_top = compiler.scope_depth == 0;

    local_t *local = &compiler.locals[compiler.local_count];
    local->name = name;
    local->type = type;
    local->is_on_stack =
        is_mem || (is_type(type, TYPE_STRUCT) && !is_pointer(type));
    local->is_param = false;
    local->is_absolute = false;
    local->is_global = false;
    local->depth = compiler.scope_depth;

    if (local->is_on_stack && !is_top) {
        local->frame_index = compiler.sstack_count;
        compiler.sstack_count += get_type_size(type);
    }

    compiler.local_count++;

    if (is_top) {
        local->is_absolute = true;
        local->is_on_stack = true;
        local->frame_index = 16384 + compiler.data_offset;

        const_t *global_const = &compiler.consts[compiler.const_count++];
        global_const->type = type;
        global_const->is_local = false;
        global_const->offset = compiler.data_offset;
        global_const->size = get_type_size(type);

        compiler.data_offset += global_const->size;

        if (match(TOKEN_EQUAL)) {
            expression(PREC_ASSIGNMENT);

            stackv_t *last_stackv = &compiler.stack[compiler.stack_count - 1];
            compiler.stack_count--; // Pop the last stack value

            if (isv_lvalue(last_stackv->type)) {
                error("Cannot assign an lvalue to a global variable.");
                return;
            }

            if (isv_local(last_stackv->type)) {
                global_const->is_local = true;
                global_const->as.local = last_stackv->as.local;
            } else if (isv_const(last_stackv->type)) {
                if (last_stackv->ctype != type) {
                    global_const->as.c =
                        cast_const(last_stackv->as.c, last_stackv->ctype, type);
                } else {
                    global_const->as.c = last_stackv->as.c;
                }
            } else {
                error(
                    "Cannot assign a non-constant value to a global variable.");
                return;
            }
        }

        return;
    }

    if (match(TOKEN_EQUAL)) {
        write_address_local(local, 0);

        expression(PREC_ASSIGNMENT);

        stackv_t *last_stackv = &compiler.stack[--compiler.stack_count];

        if (local->type != last_stackv->ctype) {
            if (isv_const(last_stackv->type)) {
                last_stackv->as.c = cast_const(last_stackv->as.c,
                                               last_stackv->ctype, local->type);
                last_stackv->ctype = local->type;
            }
        }

        materialize_vstack(last_stackv);
        if (local->type != last_stackv->ctype) {
            if (!isv_const(last_stackv->type)) {
                write_convert(last_stackv->ctype, local->type);
            }
        }

        write_store_local(local, local->type);
    }
}

static void function(type_e return_type) {
    if (compiler.scope_depth > 0) {
        error("Functions cannot be declared inside other functions or blocks.");
        return;
    }

    token_t name = parser.previous;
    consume(TOKEN_LEFT_PAREN, "Expected '(' after function name.");

    local_t *local = &compiler.locals[compiler.local_count++];
    local->name = name;
    local->type = create_type(TYPE_FUN, false, compiler.fundef_count);
    local->is_on_stack = false;
    local->is_absolute = false;
    local->is_global = false;
    local->is_param = false;
    local->depth = compiler.scope_depth;

    if (!compiler.no_op) {
        write_string("(func $");
        write_token(name);
        write_string(" ");
    }

    compiler.sstack_count = 0;
    compiler.sstack_atleast = 0;
    compiler.used_fp = false;

    int frame_ptr = compiler.local_count;
    local_t *last_frame_ptr = compiler.frame_ptr;

    compiler.frame_ptr = &compiler.locals[frame_ptr];

    fundef_t *fundef = &compiler.fundefs[compiler.fundef_count++];
    fundef->name = name;
    fundef->return_type = return_type;
    fundef->param_count = 0;

    // Parse parameters
    while (!check(TOKEN_RIGHT_PAREN) && !check(TOKEN_EOF)) {
        type_e param_type;
        bool is_mem = false;

        if (match(TOKEN_MEM)) {
            is_mem = true;
        }

        if ((param_type = match_type())) {
            consume(TOKEN_IDENTIFIER, "Expected parameter name.");
            token_t param_name = parser.previous;

            if (!compiler.no_op) {
                write_string("(param $");
                write_int(compiler.local_count - frame_ptr);
                write_string(" ");
                write_type(param_type);
                write_string(")");
            }

            local_t *local = &compiler.locals[compiler.local_count];
            local->name = param_name;
            local->type = param_type;
            local->is_on_stack = is_mem;
            local->is_absolute = false;
            local->is_global = false;
            local->is_param = true;
            local->depth = compiler.scope_depth;

            if (is_mem) {
                local->frame_index = compiler.sstack_count;
                compiler.sstack_count += get_type_size(param_type);
            }

            compiler.local_count++;
        } else {
            error("Expected a type for parameter.");
        }

        if (!check(TOKEN_RIGHT_PAREN)) {
            consume(TOKEN_COMMA, "Expected ',' between parameters.");
        }
    }

    fundef->param_count = compiler.local_count - frame_ptr;
    fundef->params =
        (fielddef_t *)malloc(sizeof(fielddef_t) * fundef->param_count);

    for (int i = 0; i < fundef->param_count; i++) {
        local_t *param_local = &compiler.locals[frame_ptr + i];

        fundef->params[i].name = param_local->name;
        fundef->params[i].type = param_local->type;
    }

    if (!compiler.no_op) {
        if (return_type != TYPE_VOID) {
            write_string(" (result ");
            write_type(return_type);
            write_string(")");
        }

        write_string("\n");
    }

    consume(TOKEN_RIGHT_PAREN, "Expected ')' after function parameters.");
    consume(TOKEN_LEFT_BRACE, "Expected '{' before function body.");

    char *checkpoint = compiler.current;

    begin_scope();
    block();
    end_scope();

    if (compiler.no_op) {
        compiler.current = checkpoint;
        return;
    }

    int frame_size = compiler.sstack_count > compiler.sstack_atleast
                         ? compiler.sstack_count
                         : compiler.sstack_atleast;

    int body_count = compiler.current - checkpoint;
    char *scratch = malloc(body_count);
    memcpy(scratch, checkpoint, body_count);

    compiler.current = checkpoint;

    if (frame_size > 0) {
        compiler.used_fp = true;
    }

    if (compiler.used_fp) {
        write_string("(local $fp i32)\n");
    }

    for (int i = frame_ptr; i < compiler.local_count; i++) {
        local_t *local = &compiler.locals[i];

        if (!local->is_on_stack && !local->is_param) {
            write_string("(local $");
            write_int(i - frame_ptr);
            write_string(" ");
            write_type(local->type);
            write_string(")\n");
        }
    }

    if (frame_size > 0) {
        write_string("global.get $__sp\n");
        write_i32_const(frame_size);
        write_string("i32.sub\n");
        write_string("local.tee $fp\n");
        write_string("global.set $__sp\n");
    }

    for (int i = compiler.local_count - 1; i >= frame_ptr; --i) {
        local_t *local = &compiler.locals[i];
        if (local->is_on_stack && local->is_param) {
            write_string("local.get $fp\n");
            write_i32_const(local->frame_index);
            write_string("i32.add\n");
            write_string("local.get $");
            write_int(i - frame_ptr);
            write_string("\n");
            write_type(local->type);
            write_string(".store\n");
        }
    }

    memcpy(compiler.current, scratch, body_count);
    free(scratch);

    compiler.current += body_count;

    if (frame_size > 0) {
        write_string("local.get $fp\n");
        write_i32_const(frame_size);
        write_string("i32.add\n");
        write_string("global.set $__sp\n");
    }

    compiler.local_count = frame_ptr;
    compiler.frame_ptr = last_frame_ptr;
    compiler.used_fp = false;

    write_string(")\n");
}

static void typed_declaration(type_e decl_type, bool is_mem) {
    consume(TOKEN_IDENTIFIER, "Expected variable name.");

    if (check(TOKEN_LEFT_PAREN)) {
        if (is_mem) {
            error("Memory declarations cannot be functions.");
            return;
        }

        function(decl_type);
    } else {
        named_var(decl_type, is_mem);
        consume(TOKEN_SEMICOLON, "Expected ';' after declaration.");
    }
}

static void mem_declaration() {
    type_e decl_type = match_type();
    if (!decl_type) {
        error("Expected a type for memory declaration.");
        return;
    }

    typed_declaration(decl_type, true);
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

static void return_statement() {
    if (compiler.scope_depth == 0) {
        error("Return statement cannot be used outside of a function.");
        return;
    }

    if (match(TOKEN_SEMICOLON)) {
        iwrite_return();
        return;
    }

    expression(PREC_ASSIGNMENT);
    consume(TOKEN_SEMICOLON, "Expected ';' after return statement.");

    stackv_t *last_stackv = &compiler.stack[--compiler.stack_count];
    materialize_vstack(last_stackv);
}

static void declaration() {
    type_e decl_type;

    if (match(TOKEN_STRUCT)) {
        struct_declaration();
    } else if (match(TOKEN_MEM)) {
        mem_declaration();
    } else if ((decl_type = match_type())) {
        typed_declaration(decl_type, false);
    } else {
        if (match(TOKEN_PRINT)) {
            print_statement();
        } else if (match(TOKEN_IF)) {
            if_statement();
        } else if (match(TOKEN_WHILE)) {
            while_statement();
        } else if (match(TOKEN_FOR)) {
            for_statement();
        } else if (match(TOKEN_RETURN)) {
            return_statement();
        } else {
            expression(PREC_ASSIGNMENT);
            consume(TOKEN_SEMICOLON, "Expected ';' after declaration.");
        }
    }
}

static void add_import_fn(const char *module, const char *name,
                          type_e return_type, int param_count,
                          type_e *param_types) {
    local_t *local = &compiler.locals[compiler.local_count++];
    local->name.start = name;
    local->name.length = strlen(name);
    local->type = create_type(TYPE_FUN, false, compiler.fundef_count);
    local->is_on_stack = false;
    local->is_absolute = false;
    local->is_global = false;
    local->is_param = false;
    local->depth = 0;

    fundef_t *fundef = &compiler.fundefs[compiler.fundef_count++];
    fundef->name.start = name;
    fundef->name.length = strlen(name);
    fundef->return_type = return_type;
    fundef->param_count = param_count;
    fundef->params = (fielddef_t *)malloc(sizeof(fielddef_t) * param_count);

    for (int i = 0; i < param_count; i++) {
        fundef->params[i].name.start = NULL; // No parameter names for imports
        fundef->params[i].name.length = 0;
        fundef->params[i].type = param_types[i];
    }

    write_string("(import \"");
    write_string(module);
    write_string("\" \"");
    write_string(name);
    write_string("\" (func $");
    write_string(name);
    write_string(" ");

    if (param_count > 0) {
        write_string("(param");
        for (int i = 0; i < param_count; i++) {
            write_string(" ");
            write_type(param_types[i]);
        }

        write_string(")");
    }

    if (return_type != TYPE_VOID) {
        write_string(" (result ");
        write_type(return_type);
        write_string(")");
    }

    write_string("))\n");
}

static void module() {
    write_string("(module\n");
    write_string("(import \"core\" \"print\" (func $print_i32 (param i32)))\n");
    write_string("(import \"core\" \"print\" (func $print_i64 (param i64)))\n");
    write_string("(import \"core\" \"print\" (func $print_f32 (param f32)))\n");
    write_string("(import \"core\" \"print\" (func $print_f64 (param f64)))\n");

    add_import_fn("core", "putc", TYPE_VOID, 1, (type_e[]){TYPE_I32});
    add_import_fn("core", "puts", TYPE_VOID, 1, (type_e[]){TYPE_I32});
    add_import_fn("core", "rand", TYPE_I32, 2, (type_e[]){TYPE_I32, TYPE_I32});
    add_import_fn("core", "get_key", TYPE_BOOL, 1, (type_e[]){TYPE_I32});
    add_import_fn("ctx", "ctx_put_pixel", TYPE_VOID, 3,
                  (type_e[]){TYPE_I32, TYPE_I32, TYPE_I32});
    add_import_fn("ctx", "ctx_fill", TYPE_VOID, 1, (type_e[]){TYPE_I32});

    write_string("(memory $0 1)\n");

    local_t *heap_base_local = &compiler.locals[compiler.local_count++];
    heap_base_local->name.start = "__heap_base";
    heap_base_local->name.length = 11;
    heap_base_local->type = TYPE_CHAR | TYPE_POINTER_FLAG;
    heap_base_local->is_on_stack = false;
    heap_base_local->is_absolute = false;
    heap_base_local->is_param = false;
    heap_base_local->is_global = true;
    heap_base_local->depth = 0;

    while (parser.current.type != TOKEN_EOF) {
        declaration();
    }

    heap_base_local->frame_index = 16384 + compiler.data_offset;

    for (int i = 0; i < compiler.const_count; i++) {
        const_t *const_value = &compiler.consts[i];
        char buffer[32];

        write_string("(data (i32.const ");
        snprintf(buffer, sizeof(buffer), "%d", 16384 + const_value->offset);
        write_string(buffer);
        write_string(") \"");

        if (is_type(const_value->type, TYPE_CHAR)) {
            const char *str = const_value->as.string;
            for (int j = 0; j < const_value->size; j++) {
                char c = str[j];
                if (c == '"' || c == '\\') {
                    write_string((char[]){c, '\0'});
                } else if (c >= 32 && c <= 126) {
                    write_string((char[]){c, '\0'});
                } else {
                    snprintf(buffer, sizeof(buffer), "\\%02x",
                             (unsigned char)c);
                    write_string(buffer);
                }
            }

            write_string("\\00");
        } else if (is_pointer(const_value->type)) {
            int offset = const_value->is_local
                             ? const_value->as.local->frame_index
                             : const_value->as.c.i32;

            for (int j = 0; j < 4; ++j) {
                unsigned char byte = (offset >> (j * 8)) & 0xFF;
                snprintf(buffer, sizeof(buffer), "\\%02x", byte);
                write_string(buffer);
            }
        } else if (is_type(const_value->type, TYPE_I8)) {
            unsigned char byte = const_value->as.c.i8;
            snprintf(buffer, sizeof(buffer), "\\%02x", byte);
            write_string(buffer);
        } else if (is_type(const_value->type, TYPE_I16)) {
            for (int j = 0; j < 2; ++j) {
                unsigned char byte = (const_value->as.c.i16 >> (j * 8)) & 0xFF;
                snprintf(buffer, sizeof(buffer), "\\%02x", byte);
                write_string(buffer);
            }
        } else if (is_type(const_value->type, TYPE_I32)) {
            for (int j = 0; j < 4; ++j) {
                unsigned char byte = (const_value->as.c.i32 >> (j * 8)) & 0xFF;
                snprintf(buffer, sizeof(buffer), "\\%02x", byte);
                write_string(buffer);
            }
        } else if (is_type(const_value->type, TYPE_I64)) {
            for (int j = 0; j < 8; ++j) {
                unsigned char byte = (const_value->as.c.i64 >> (j * 8)) & 0xFF;
                snprintf(buffer, sizeof(buffer), "\\%02x", byte);
                write_string(buffer);
            }
        } else if (is_type(const_value->type, TYPE_F32)) {
            uint32_t f32_bits;
            memcpy(&f32_bits, &const_value->as.c.f32, sizeof(f32_bits));
            for (int j = 0; j < 4; ++j) {
                unsigned char byte = (f32_bits >> (j * 8)) & 0xFF;
                snprintf(buffer, sizeof(buffer), "\\%02x", byte);
                write_string(buffer);
            }
        } else if (is_type(const_value->type, TYPE_F64)) {
            uint64_t f64_bits;
            memcpy(&f64_bits, &const_value->as.c.f64, sizeof(f64_bits));
            for (int j = 0; j < 8; ++j) {
                unsigned char byte = (f64_bits >> (j * 8)) & 0xFF;
                snprintf(buffer, sizeof(buffer), "\\%02x", byte);
                write_string(buffer);
            }
        } else if (is_type(const_value->type, TYPE_BOOL)) {
            unsigned char byte = const_value->as.c.i32 != 0 ? 1 : 0;
            snprintf(buffer, sizeof(buffer), "\\%02x", byte);
            write_string(buffer);
        } else if (is_type(const_value->type, TYPE_STRUCT)) {
            int structdef_index = get_type_data(const_value->type);
            structdef_t *structdef = &compiler.structdefs[structdef_index];
            int size = calculate_struct_size(structdef);

            for (int j = 0; j < size; ++j) {
                write_string("\\00");
            }
        } else {
            error("Unsupported constant type for data segment.");
        }

        write_string("\")\n");
    }

    write_string("(global $__sp (mut i32) (i32.const 16384))\n");
    write_string("(global $__heap_base i32 (i32.const ");
    write_int(heap_base_local->frame_index);
    write_string("))\n");

    write_string("(export \"memory\" (memory $0))\n");
    for (int i = 0; i < compiler.fundef_count; ++i) {
        fundef_t *def = &compiler.fundefs[i];
        write_string("(export \"");
        write_string_length(def->name.start, def->name.length);
        write_string("\" (func $");
        write_string_length(def->name.start, def->name.length);
        write_string("))\n");
    }

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
