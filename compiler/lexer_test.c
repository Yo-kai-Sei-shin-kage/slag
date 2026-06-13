#include <stdio.h>
#include <stdlib.h>
#include "lexer.h"

// Reads an entire file into a heap buffer. Caller frees the result.
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("fopen");
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);

    if (out_len) *out_len = (size_t)size;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.slag>\n", argv[0]);
        return 1;
    }

    size_t len;
    char *src = read_file(argv[1], &len);

    Lexer lex;
    lexer_init(&lex, src, len);

    for (;;) {
        Token tok = lexer_next(&lex);
        printf("%-22s line %-3d col %-3d  '%s'",
               token_type_name(tok.type), tok.line, tok.col, tok.text);

        if (tok.type == TOK_INT_LIT) {
            printf("  (int=%ld)", tok.int_val);
        } else if (tok.type == TOK_FLOAT_LIT) {
            printf("  (float=%f)", tok.float_val);
        }
        printf("\n");

        int done = (tok.type == TOK_EOF);
        token_free(&tok);
        if (done) break;
    }

    free(src);
    return 0;
}
