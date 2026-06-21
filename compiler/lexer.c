#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "lexer.h"

void lexer_init(Lexer *lex, const char *src, size_t len) {
    lex->src = src;
    lex->pos = 0;
    lex->len = len;
    lex->line = 1;
    lex->col = 1;
}

void token_free(Token *tok) {
    if (tok->text) {
        free(tok->text);
        tok->text = NULL;
    }
}

static char peek(Lexer *lex) {
    if (lex->pos >= lex->len) return '\0';
    return lex->src[lex->pos];
}

static char peek_at(Lexer *lex, size_t offset) {
    size_t p = lex->pos + offset;
    if (p >= lex->len) return '\0';
    return lex->src[p];
}

static char advance(Lexer *lex) {
    char c = lex->src[lex->pos++];
    if (c == '\n') {
        lex->line++;
        lex->col = 1;
    } else {
        lex->col++;
    }
    return c;
}

static int is_ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static int is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

// Allocate and copy a substring of length n starting at src[start].
static char *dup_range(const char *src, size_t start, size_t n) {
    char *out = malloc(n + 1);
    memcpy(out, src + start, n);
    out[n] = '\0';
    return out;
}

// Skip whitespace and comments (// and /* */).
static void skip_whitespace_and_comments(Lexer *lex) {
    for (;;) {
        char c = peek(lex);

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(lex);
            continue;
        }

        if (c == '/' && peek_at(lex, 1) == '/') {
            while (peek(lex) != '\0' && peek(lex) != '\n') {
                advance(lex);
            }
            continue;
        }

        if (c == '/' && peek_at(lex, 1) == '*') {
            advance(lex);
            advance(lex);
            while (peek(lex) != '\0') {
                if (peek(lex) == '*' && peek_at(lex, 1) == '/') {
                    advance(lex);
                    advance(lex);
                    break;
                }
                advance(lex);
            }
            continue;
        }

        break;
    }
}

// Match a keyword string and return its token type, or TOK_IDENT if no match.
static TokenType keyword_type(const char *text) {
    if (strcmp(text, "int") == 0)      return TOK_KW_INT;
    if (strcmp(text, "float") == 0)    return TOK_KW_FLOAT;
    if (strcmp(text, "str") == 0)      return TOK_KW_STR;
    if (strcmp(text, "bool") == 0)     return TOK_KW_BOOL;
    if (strcmp(text, "if") == 0)       return TOK_KW_IF;
    if (strcmp(text, "else") == 0)     return TOK_KW_ELSE;
    if (strcmp(text, "while") == 0)    return TOK_KW_WHILE;
    if (strcmp(text, "true") == 0)     return TOK_KW_TRUE;
    if (strcmp(text, "false") == 0)    return TOK_KW_FALSE;
    if (strcmp(text, "function") == 0) return TOK_KW_FUNCTION;
    if (strcmp(text, "return") == 0)   return TOK_KW_RETURN;
    if (strcmp(text, "var") == 0)      return TOK_KW_VAR;
    if (strcmp(text, "thread") == 0)   return TOK_KW_THREAD;
    if (strcmp(text, "sync") == 0)     return TOK_KW_SYNC;
    if (strcmp(text, "lock") == 0)     return TOK_KW_LOCK;
    if (strcmp(text, "window") == 0)   return TOK_KW_WINDOW;
    if (strcmp(text, "pixel") == 0)    return TOK_KW_PIXEL;
    if (strcmp(text, "flush") == 0)    return TOK_KW_FLUSH;
    if (strcmp(text, "on") == 0)       return TOK_KW_ON;
    if (strcmp(text, "void") == 0)     return TOK_KW_VOID;
    if (strcmp(text, "global") == 0)   return TOK_KW_GLOBAL;
    if (strcmp(text, "local") == 0)    return TOK_KW_LOCAL;
    return TOK_IDENT;
}

// Scan an identifier or keyword starting at the current position.
static Token scan_ident_or_keyword(Lexer *lex, int start_line, int start_col) {
    size_t start = lex->pos;
    while (is_ident_char(peek(lex))) {
        advance(lex);
    }
    size_t n = lex->pos - start;
    char *text = dup_range(lex->src, start, n);

    Token tok;
    tok.type = keyword_type(text);
    tok.text = text;
    tok.line = start_line;
    tok.col = start_col;
    tok.int_val = 0;
    tok.float_val = 0.0;
    return tok;
}

// Scan a numeric literal (int or float).
static Token scan_number(Lexer *lex, int start_line, int start_col) {
    size_t start = lex->pos;
    int is_float = 0;

    while (isdigit((unsigned char)peek(lex))) {
        advance(lex);
    }

    if (peek(lex) == '.' && isdigit((unsigned char)peek_at(lex, 1))) {
        is_float = 1;
        advance(lex);
        while (isdigit((unsigned char)peek(lex))) {
            advance(lex);
        }
    }

    size_t n = lex->pos - start;
    char *text = dup_range(lex->src, start, n);

    Token tok;
    tok.text = text;
    tok.line = start_line;
    tok.col = start_col;
    tok.int_val = 0;
    tok.float_val = 0.0;

    if (is_float) {
        tok.type = TOK_FLOAT_LIT;
        tok.float_val = strtod(text, NULL);
    } else {
        tok.type = TOK_INT_LIT;
        tok.int_val = strtol(text, NULL, 10);
    }

    return tok;
}

// Scan a double-quoted string literal. Supports \n \t \\ \" escapes.
static Token scan_string(Lexer *lex, int start_line, int start_col) {
    advance(lex); // opening "

    size_t cap = 32;
    size_t n = 0;
    char *buf = malloc(cap);

    while (peek(lex) != '"' && peek(lex) != '\0') {
        char c = advance(lex);

        if (c == '\\') {
            char esc = advance(lex);
            switch (esc) {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case '\\': c = '\\'; break;
                case '"':  c = '"';  break;
                case '0':  c = '\0'; break;
                default:   c = esc;  break;
            }
        }

        if (n + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        buf[n++] = c;
    }

    if (peek(lex) == '"') {
        advance(lex); // closing "
    }

    buf[n] = '\0';

    Token tok;
    tok.type = TOK_STR_LIT;
    tok.text = buf;
    tok.line = start_line;
    tok.col = start_col;
    tok.int_val = 0;
    tok.float_val = 0.0;
    return tok;
}

// Scan a /regex/ literal. Preserves backslash escapes as-is so the regex
// engine sees the original sequences (\d, \w, \/, etc.).
static Token scan_regex(Lexer *lex, int start_line, int start_col) {
    advance(lex); // opening /

    size_t cap = 32;
    size_t n = 0;
    char *buf = malloc(cap);

    while (peek(lex) != '/' && peek(lex) != '\0' && peek(lex) != '\n') {
        char c = advance(lex);

        if (c == '\\' && peek(lex) != '\0') {
            if (n + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
            buf[n++] = c;
            c = advance(lex);
        }

        if (n + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[n++] = c;
    }

    if (peek(lex) == '/') {
        advance(lex); // closing /
    }

    buf[n] = '\0';

    Token tok;
    tok.type = TOK_REGEX_LIT;
    tok.text = buf;
    tok.line = start_line;
    tok.col = start_col;
    tok.int_val = 0;
    tok.float_val = 0.0;
    return tok;
}

// Scan $identifier or the $(( opener.
static Token scan_dollar(Lexer *lex, int start_line, int start_col) {
    advance(lex); // $

    if (peek(lex) == '(' && peek_at(lex, 1) == '(') {
        advance(lex); // (
        advance(lex); // (
        Token tok;
        tok.type = TOK_DOLLAR_LPAREN_LPAREN;
        tok.text = dup_range("$((", 0, 3);
        tok.line = start_line;
        tok.col = start_col;
        tok.int_val = 0;
        tok.float_val = 0.0;
        return tok;
    }

    if (is_ident_start(peek(lex))) {
        size_t start = lex->pos;
        while (is_ident_char(peek(lex))) {
            advance(lex);
        }
        size_t n = lex->pos - start;
        char *text = malloc(n + 2);
        text[0] = '$';
        memcpy(text + 1, lex->src + start, n);
        text[n + 1] = '\0';

        Token tok;
        tok.type = TOK_DOLLAR_IDENT;
        tok.text = text;
        tok.line = start_line;
        tok.col = start_col;
        tok.int_val = 0;
        tok.float_val = 0.0;
        return tok;
    }

    Token tok;
    tok.type = TOK_UNKNOWN;
    tok.text = dup_range("$", 0, 1);
    tok.line = start_line;
    tok.col = start_col;
    tok.int_val = 0;
    tok.float_val = 0.0;
    return tok;
}

// Heuristic to distinguish a regex literal from a division operator.
// Division only follows a value-producing token; otherwise, if a closing
// '/' exists before end of line or ';', treat as regex.
static int looks_like_regex_start(Lexer *lex, TokenType prev_type) {
    switch (prev_type) {
        case TOK_IDENT:
        case TOK_INT_LIT:
        case TOK_FLOAT_LIT:
        case TOK_STR_LIT:
        case TOK_RPAREN:
        case TOK_RBRACKET:
        case TOK_DOLLAR_IDENT:
            return 0;
        default:
            break;
    }

    size_t p = lex->pos + 1;
    while (p < lex->len) {
        char c = lex->src[p];
        if (c == '\n' || c == ';') return 0;
        if (c == '/') return 1;
        p++;
    }
    return 0;
}

Token lexer_next(Lexer *lex) {
    static TokenType last_type = TOK_UNKNOWN;

    skip_whitespace_and_comments(lex);

    int start_line = lex->line;
    int start_col = lex->col;

    char c = peek(lex);

    if (c == '\0') {
        Token tok;
        tok.type = TOK_EOF;
        tok.text = dup_range("", 0, 0);
        tok.line = start_line;
        tok.col = start_col;
        tok.int_val = 0;
        tok.float_val = 0.0;
        last_type = TOK_EOF;
        return tok;
    }

    if (is_ident_start(c)) {
        Token tok = scan_ident_or_keyword(lex, start_line, start_col);
        last_type = tok.type;
        return tok;
    }

    if (isdigit((unsigned char)c)) {
        Token tok = scan_number(lex, start_line, start_col);
        last_type = tok.type;
        return tok;
    }

    if (c == '"') {
        Token tok = scan_string(lex, start_line, start_col);
        last_type = tok.type;
        return tok;
    }

    if (c == '$') {
        Token tok = scan_dollar(lex, start_line, start_col);
        last_type = tok.type;
        return tok;
    }

    if (c == '/') {
        if (looks_like_regex_start(lex, last_type)) {
            Token tok = scan_regex(lex, start_line, start_col);
            last_type = tok.type;
            return tok;
        }
        advance(lex);
        Token tok;
        tok.type = TOK_SLASH;
        tok.text = dup_range("/", 0, 1);
        tok.line = start_line;
        tok.col = start_col;
        tok.int_val = 0;
        tok.float_val = 0.0;
        last_type = tok.type;
        return tok;
    }

    if (c == '=' && peek_at(lex, 1) == '=') {
        advance(lex); advance(lex);
        Token tok = { TOK_EQ, dup_range("==", 0, 2), start_line, start_col, 0, 0.0 };
        last_type = tok.type;
        return tok;
    }
    if (c == '!' && peek_at(lex, 1) == '=') {
        advance(lex); advance(lex);
        Token tok = { TOK_NEQ, dup_range("!=", 0, 2), start_line, start_col, 0, 0.0 };
        last_type = tok.type;
        return tok;
    }
    if (c == '<' && peek_at(lex, 1) == '=') {
        advance(lex); advance(lex);
        Token tok = { TOK_LE, dup_range("<=", 0, 2), start_line, start_col, 0, 0.0 };
        last_type = tok.type;
        return tok;
    }
    if (c == '>' && peek_at(lex, 1) == '=') {
        advance(lex); advance(lex);
        Token tok = { TOK_GE, dup_range(">=", 0, 2), start_line, start_col, 0, 0.0 };
        last_type = tok.type;
        return tok;
    }
    if (c == '&' && peek_at(lex, 1) == '&') {
        advance(lex); advance(lex);
        Token tok = { TOK_AND, dup_range("&&", 0, 2), start_line, start_col, 0, 0.0 };
        last_type = tok.type;
        return tok;
    }
    if (c == '|' && peek_at(lex, 1) == '|') {
        advance(lex); advance(lex);
        Token tok = { TOK_OR, dup_range("||", 0, 2), start_line, start_col, 0, 0.0 };
        last_type = tok.type;
        return tok;
    }

    TokenType type = TOK_UNKNOWN;
    switch (c) {
        case '(': type = TOK_LPAREN; break;
        case ')': type = TOK_RPAREN; break;
        case '{': type = TOK_LBRACE; break;
        case '}': type = TOK_RBRACE; break;
        case '[': type = TOK_LBRACKET; break;
        case ']': type = TOK_RBRACKET; break;
        case ';': type = TOK_SEMICOLON; break;
        case ',': type = TOK_COMMA; break;
        case '.': type = TOK_DOT; break;
        case '+': type = TOK_PLUS; break;
        case '-': type = TOK_MINUS; break;
        case '*': type = TOK_STAR; break;
        case '%': type = TOK_PERCENT; break;
        case '=': type = TOK_ASSIGN; break;
        case '<': type = TOK_LT; break;
        case '>': type = TOK_GT; break;
        case '!': type = TOK_NOT; break;
        default:  type = TOK_UNKNOWN; break;
    }

    advance(lex);
    Token tok;
    tok.type = type;
    tok.text = malloc(2);
    tok.text[0] = c;
    tok.text[1] = '\0';
    tok.line = start_line;
    tok.col = start_col;
    tok.int_val = 0;
    tok.float_val = 0.0;
    last_type = tok.type;
    return tok;
}

const char *token_type_name(TokenType type) {
    switch (type) {
        case TOK_EOF: return "EOF";
        case TOK_INT_LIT: return "INT_LIT";
        case TOK_FLOAT_LIT: return "FLOAT_LIT";
        case TOK_STR_LIT: return "STR_LIT";
        case TOK_REGEX_LIT: return "REGEX_LIT";
        case TOK_IDENT: return "IDENT";
        case TOK_KW_INT: return "KW_INT";
        case TOK_KW_FLOAT: return "KW_FLOAT";
        case TOK_KW_STR: return "KW_STR";
        case TOK_KW_BOOL: return "KW_BOOL";
        case TOK_KW_IF: return "KW_IF";
        case TOK_KW_ELSE: return "KW_ELSE";
        case TOK_KW_WHILE: return "KW_WHILE";
        case TOK_KW_TRUE: return "KW_TRUE";
        case TOK_KW_FALSE: return "KW_FALSE";
        case TOK_KW_FUNCTION: return "KW_FUNCTION";
        case TOK_KW_RETURN: return "KW_RETURN";
        case TOK_KW_VAR: return "KW_VAR";
        case TOK_KW_THREAD: return "KW_THREAD";
        case TOK_KW_SYNC: return "KW_SYNC";
        case TOK_KW_LOCK: return "KW_LOCK";
        case TOK_KW_WINDOW: return "KW_WINDOW";
        case TOK_KW_PIXEL: return "KW_PIXEL";
        case TOK_KW_FLUSH: return "KW_FLUSH";
        case TOK_KW_ON: return "KW_ON";
        case TOK_KW_VOID: return "KW_VOID";
        case TOK_DOLLAR_LPAREN_LPAREN: return "DOLLAR_LPAREN_LPAREN";
        case TOK_DOLLAR_IDENT: return "DOLLAR_IDENT";
        case TOK_LPAREN: return "LPAREN";
        case TOK_RPAREN: return "RPAREN";
        case TOK_LBRACE: return "LBRACE";
        case TOK_RBRACE: return "RBRACE";
        case TOK_LBRACKET: return "LBRACKET";
        case TOK_RBRACKET: return "RBRACKET";
        case TOK_SEMICOLON: return "SEMICOLON";
        case TOK_COMMA: return "COMMA";
        case TOK_DOT: return "DOT";
        case TOK_PLUS: return "PLUS";
        case TOK_MINUS: return "MINUS";
        case TOK_STAR: return "STAR";
        case TOK_SLASH: return "SLASH";
        case TOK_PERCENT: return "PERCENT";
        case TOK_ASSIGN: return "ASSIGN";
        case TOK_EQ: return "EQ";
        case TOK_NEQ: return "NEQ";
        case TOK_LT: return "LT";
        case TOK_GT: return "GT";
        case TOK_LE: return "LE";
        case TOK_GE: return "GE";
        case TOK_AND: return "AND";
        case TOK_OR: return "OR";
        case TOK_NOT: return "NOT";
        default: return "UNKNOWN";
    }
}
