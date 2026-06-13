#ifndef PARSER_H
#define PARSER_H

#include <stddef.h>
#include "ast.h"

// ---------------------------------------------------------------------
// Public parser interface
// ---------------------------------------------------------------------
//
// parse_program() is the single entry point. It takes a null-terminated
// source buffer and its byte length, runs the full recursive descent
// parse, and returns a Program AST.
//
// On parse errors the parser prints diagnostics to stderr and sets an
// internal error flag; parsing continues with best-effort recovery so
// that multiple errors can be reported in one pass. Check
// prog.functions.count == 0 and stderr output to detect failure, or
// add a parse_had_error() call if you want a programmatic check.
//
// The returned Program and all nodes it references are heap-allocated.
// The caller is responsible for freeing them (ast_free_* functions will
// be added in a later pass; for now the compiler process exits and the
// OS reclaims the memory).
// ---------------------------------------------------------------------

// Parse a complete Slag source file and return the program AST.
// `src`  — pointer to the source text (need not be null-terminated
//           beyond `len` bytes, but a null byte there is harmless).
// `len`  — byte length of the source text.
Program parse_program(const char *src, size_t len);

#endif // PARSER_H
