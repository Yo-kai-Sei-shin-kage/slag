#ifndef CODEGEN_H
#define CODEGEN_H

#include <stdio.h>
#include "ast.h"

// ---------------------------------------------------------------------
// Public code generator interface
// ---------------------------------------------------------------------
//
// codegen_program() walks the Program AST and emits NASM x86-64 Win64
// assembly to `out`. The output is a complete .asm file ready to be
// assembled with:
//
//   nasm -f win64 output.asm -o output.obj
//   gcc output.obj -o program.exe -lkernel32 -luser32 -lgdi32 \
//       -nostartfiles -e _start
//
// All Win32 calls are made directly via kernel32/user32/gdi32 imports.
// No CRT functions are used.
// ---------------------------------------------------------------------

void codegen_program(const Program *prog, FILE *out);

#endif // CODEGEN_H
