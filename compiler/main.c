// main.c — Slag compiler entry point
//
// Usage:
//   slag <file.slag>
//
// Reads the source file, parses it into an AST, then runs the code
// generator and writes NASM x86-64 Win64 assembly to <file>.asm.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "codegen.h"

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf) { fprintf(stderr, "error: out of memory\n"); exit(1); }
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)size;
    return buf;
}

// Replace or append file extension with .asm
static char *make_asm_path(const char *src_path) {
    const char *dot = strrchr(src_path, '.');
    size_t base_len = dot ? (size_t)(dot - src_path) : strlen(src_path);
    char *out = malloc(base_len + 5);
    memcpy(out, src_path, base_len);
    memcpy(out + base_len, ".asm", 5);
    return out;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: slag <file.slag>\n");
        return 1;
    }

    const char *src_path = argv[1];

    size_t len;
    char *src = read_file(src_path, &len);

    Program prog = parse_program(src, len);

    if (prog.functions.count == 0) {
        fprintf(stderr, "error: no functions parsed\n");
        free(src);
        return 1;
    }

    char *asm_path = make_asm_path(src_path);
    FILE *asm_out = fopen(asm_path, "w");
    if (!asm_out) {
        fprintf(stderr, "error: cannot write '%s'\n", asm_path);
        free(src);
        free(asm_path);
        return 1;
    }

    codegen_program(&prog, asm_out);
    fclose(asm_out);

    printf("%s compiled successfully\n", src_path);

    free(src);
    free(asm_path);
    return 0;
}
