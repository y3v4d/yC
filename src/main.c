#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wasm.h>
#include <wasmtime.h>

#include "compiler.h"
#include "debug.h"

static void exit_with_error(const char *message, wasmtime_error_t *error,
                            wasm_trap_t *trap) {
    fprintf(stderr, "error: %s\n", message);

    wasm_byte_vec_t error_message;
    if (error != NULL) {
        wasmtime_error_message(error, &error_message);
        wasmtime_error_delete(error);
    } else {
        wasm_trap_message(trap, &error_message);
        wasm_trap_delete(trap);
    }

    fprintf(stderr, "%.*s\n", (int)error_message.size, error_message.data);

    wasm_byte_vec_delete(&error_message);
    exit(1);
}

static bool compile_wat(const char *wat, const char *output_path) {
    wasm_engine_t *engine = wasm_engine_new();
    assert(engine != NULL);

    wasm_byte_vec_t wat_vec;
    wasm_byte_vec_new_uninitialized(&wat_vec, strlen(wat));

    memcpy(wat_vec.data, wat, strlen(wat));

    wasm_byte_vec_t wasm_vec;
    wasmtime_error_t *error =
        wasmtime_wat2wasm(wat_vec.data, wat_vec.size, &wasm_vec);
    if (error != NULL)
        exit_with_error("failed to parse wat", error, NULL);
    wasm_byte_vec_delete(&wat_vec);

    FILE *output_file = fopen(output_path, "wb");
    if (output_file == NULL) {
        fprintf(stderr, "Failed to open output.wasm for writing\n");
        return false;
    }

    fwrite(wasm_vec.data, 1, wasm_vec.size, output_file);
    fclose(output_file);

    wasm_byte_vec_delete(&wasm_vec);
    wasm_engine_delete(engine);

    return true;
}

static void execute_wasm(const char *wasm_code) {
    bool result = compile_wat(wasm_code, "build/out.wasm");
    if (!result) {
        fprintf(stderr, "Failed to compile WAT to WASM.\n");
        return;
    }

    int ret = system("node index.js");
    if (ret != 0) {
        fprintf(stderr, "Failed to execute node index.js.\n");
        return;
    }
}

static void repl() {
    char input[2048];
    while (true) {
        printf("> ");
        if (!fgets(input, sizeof(input), stdin)) {
            printf("\n");
            break;
        }

        if (strcmp(input, "exit\n") == 0) {
            break;
        }

        if (strcmp(input, "l\n") == 0) {
            FILE *file = fopen("main.yc", "rb");
            if (!file) {
                fprintf(stderr, "Failed to open file: main.yc\n");
                continue;
            }

            fseek(file, 0, SEEK_END);
            long length = ftell(file);
            fseek(file, 0, SEEK_SET);

            size_t bytes_read = fread(input, 1, length, file);
            input[bytes_read] = '\0';
            fclose(file);
        } else if (strncmp(input, "load ", 5) == 0) {
            char *filename = input + 5;
            filename[strcspn(filename, "\n")] = '\0'; // remove newline

            FILE *file = fopen(filename, "rb");
            if (!file) {
                fprintf(stderr, "Failed to open file: %s\n", filename);
                continue;
            }

            fseek(file, 0, SEEK_END);
            long length = ftell(file);
            fseek(file, 0, SEEK_SET);

            char *file_content = malloc(length + 1);
            if (!file_content) {
                fprintf(stderr, "Memory allocation failed.\n");
                fclose(file);
                continue;
            }

            size_t bytes_read = fread(file_content, 1, length, file);
            file_content[bytes_read] = '\0';
            fclose(file);

            strcpy(input, file_content);
            free(file_content);
        }

        char *output = compile(input);
#ifdef DEBUG_OUTPUT_WAT
        FILE *wat_file = fopen("build/out.wat", "wb");
        if (!wat_file) {
            fprintf(stderr, "Failed to open output.wat for writing\n");
            continue;
        }

        fwrite(output, 1, strlen(output), wat_file);
        fclose(wat_file);
#endif
        if (output) {
            execute_wasm(output);
        } else {
            printf("Compilation failed.\n");
        }
    }

    printf("REPL exited.\n");
}

static void compile_yc(const char *path) {
    long path_len = strlen(path);
    if (path_len < 3 || strcmp(path + path_len - 3, ".yc") != 0) {
        fprintf(stderr, "Error: Input file must have a .yc extension.\n");
        return;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Failed to open file: %s\n", path);
        return;
    }

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *source = malloc(length + 1);
    if (!source) {
        fprintf(stderr, "Memory allocation failed.\n");
        fclose(file);

        return;
    }

    size_t bytes_read = fread(source, 1, length, file);
    source[bytes_read] = '\0';
    fclose(file);

    char *output = compile(source);
    free(source);

#ifdef DEBUG_OUTPUT_WAT
    char *output_path_wat = malloc(path_len + 2);
    if (!output_path_wat) {
        fprintf(stderr, "Memory allocation failed.\n");
        return;
    }

    strncpy(output_path_wat, path, path_len - 3);
    output_path_wat[path_len - 3] = '\0';
    strcat(output_path_wat, ".wat");

    FILE *wat_file = fopen(output_path_wat, "wb");
    free(output_path_wat);

    if (!wat_file) {
        fprintf(stderr, "Failed to open output.wat for writing\n");
        return;
    }

    fwrite(output, 1, strlen(output), wat_file);
    fclose(wat_file);
#endif

    char *output_path = malloc(path_len + 3);
    if (!output_path) {
        fprintf(stderr, "Memory allocation failed.\n");
        return;
    }

    strncpy(output_path, path, path_len - 3);
    output_path[path_len - 3] = '\0';
    strcat(output_path, ".wasm");

    if (output) {
        compile_wat(output, output_path);
        printf("Compilation successful. Output written to: %s\n", output_path);
    } else {
        printf("Compilation failed.\n");
    }

    free(output_path);
    free(output);
}

int main(int argc, const char **argv) {
    if (argc == 1) {
        repl();
    } else if (argc == 2) {
        compile_yc(argv[1]);
    } else {
        printf("Usage: %s [source_file]\n", argv[0]);
    }

    return 0;
}
