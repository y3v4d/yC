#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "debug.h"

static bool compile_wat(const char *wat) {
    // save wat to build/out.wat file
    FILE *file = fopen("build/out.wat", "w");
    if (!file) {
        fprintf(stderr, "Failed to open build/out.wat for writing.\n");
        return false;
    }

    fprintf(file, "%s", wat);
    fclose(file);

    // execute wat2wasm build/out.wat -o build/out.wasm
    int ret = system("wat2wasm build/out.wat -o build/out.wasm");
    if (ret != 0) {
        fprintf(stderr, "Failed to execute wat2wasm.\n");
        return false;
    }

    return true;
}

static void execute_wasm(const char *wasm_code) {
    bool result = compile_wat(wasm_code);
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
        if (output) {
#ifdef DEBUG_OUTPUT
            printf("Compiled output:\n%s\n", output);
#endif
            execute_wasm(output);
        } else {
            printf("Compilation failed.\n");
        }
    }

    printf("REPL exited.\n");
}

static void compile_path(const char *path) {
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

    if (output) {
#ifdef DEBUG_OUTPUT
        printf("Compiled output:\n%s\n", output);
#endif
        compile_wat(output);
    } else {
        printf("Compilation failed.\n");
    }
}

int main(int argc, const char **argv) {
    if (argc == 1) {
        repl();
    } else if (argc == 2) {
        compile_path(argv[1]);
    } else {
        printf("Usage: %s [source_file]\n", argv[0]);
    }

    return 0;
}
