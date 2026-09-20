# yC - C-like toy language

yC is a toy programming language, similar to C. It features static typing, functions, structs, pointers and basic flow control. It's primary compilation target is WebAssembly (through WebAssembly text format), but in the future I'm planning to add support for other native architectures.
Both parser and lexer are hand-written, and the compiler is written in C (not self-hosted yet). The language is still in early development, so expect bugs and missing features.

## Features

A (yet small) list of features that yC currently supports:

- **Hand-written Lexer** - tokenizes the input source code
- **Hand-written Parser** - Pratt-parser that generates the WAT output directly (without an AST) in a single pass. In most places it first generates an intermidiate representation in form of simple opcodes which are later lowered to WAT. This is done to simplify the code generation and make it easier to add new features and targets in the future.
- **Static typing** - yC is a statically typed language, meaning that types are checked at compile time. Current primitive types are: i32, i64, f32 (not supported yet), f64 (not supported yet), bool, void (only as a return type for functions), char
- **Functions** - can be defined and called with parameters
- **Structs** - user-defined types that can contain multiple fields
- **Pointers** - variables that store the memory address of another variable. To declare a pointer, use the `*` symbol before the variable name. To dereference a pointer, use the `*` symbol before the pointer variable name. To get the address of a variable, use the `&` symbol before the variable name.
- **Basic flow control** - if/else statements, for and while loops
- **Structs** - user-defined types that can contain multiple fields. Structs as function parameters currently are only supported to be passed by value (utilizing similar copy mechanism C uses).
- **Shadow stack** - stack implemented in WebAssembly linear memory, used for storing variables. When possible the compiler will try to use WebAssembly's native local variables instead of the shadow stack, but all structs and addressed variables will be stored on the shadow stack. Currently, the allocated memory for shadow stack is 4kb and is not configurable. In the future I plan to add support for specifying the size of the shadow stack on compilation time.
- **Variable hoisting** - variables can be declared anywhere in the scope, but they will be always hoisted to the top of the running function. This is done due to necessity because WebAssembly local variables must be declared at the top of the function. Compiler should disallow using variables declared in an inner scope (like if statement) in an outer scope, but that's not fully implemented yet and trying to use such variable might result in undefined behavior.
- **Built-in statements** - print statement for printing values (currently supported i32, i64) to the console. It's done through importing a function from the host environment (in this case Node.js). In the future I plan to add support for more built-in statements and functions, like reading input from the console, file I/O, etc.

## Prerequisites

- [Clang](https://releases.llvm.org/download.html) (or other C compiler)
- [CMake](https://cmake.org/download/)
- [Node.js](https://nodejs.org/en/download) for running the WebAssembly output in a Node.js environment (used by current REPL)
- [WABT](https://github.com/webassembly/wabt) for converting WebAssembly text format to binary format (used by current REPL)

## Building

Build as any other C project with CMake:

```bash
cmake -S . -B build
cmake --build build
```

## Running

To run the provided REPL, use the following command:

```bash
./build/app
```

## Example code

```c
struct point_t {
    i32 x;
    i64 y;
};

void print_point(point_t point) {
    print point.a;
    print point.b;
}

void main() {
    bool is_valid = true;
    
    point_t p;

    if(is_valid) {
        p.x = 10;
    } else {
        p.x = 15;
    }
    
    p.y = 20;
    print_point(p);
}
```

## Roadmap

- Add better error handling and reporting
- Add better scoping support - currently scopes are only partially supported and there might be some weird bugs related to them
- Add support for f32 and f64 primitive types
- Add support for i8 and i16 primitive types
- Add support for unsigned integer types (u8, u16, u32, u64)
- Add support for arrays
- Add support for function pointers
- Add support for explicit type casting
- Add support for heap memory allocation (malloc, free)
- Add binary operation support
- Add support for constructors/destructors for structs (C++ like)
- Add support for modules and imports/exports
- Self-hosting

## References

- [Crafting Interpreters](https://craftinginterpreters.com) - A great book on building your first programming language, teaches all the basic and advanced concepts of programming languages and compilers. Nearly all of the current parser implementation is inspiried from this book alone.
- [TinyCC](https://bellard.org/tcc/tcc-doc.html) - A small C compiler, used as a reference for the yC language syntax and semantics.
- [WebAssembly](https://webassembly.org/) - The compilation target of yC, a low-level binary format for the web.
- [WebAssembly MDN Documentation](https://developer.mozilla.org/en-US/docs/WebAssembly) - A great resource for learning about WebAssembly and its features.
