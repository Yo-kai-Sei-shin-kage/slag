// texture_runtime.h — procedural texture generation for Slag.
//
// Builtins (wired in codegen.c as tex.*):
//   tex.noise2d(x, y, seed)           -> int (0-255 grayscale)
//   tex.perlin2d(x, y, freq, seed)    -> int (0-255 perlin noise)
//   tex.checker(x, y, size)           -> int (0 or 255)
//   tex.gradient_h(x, width)          -> int (0-255 horizontal gradient)
//   tex.gradient_v(y, height)         -> int (0-255 vertical gradient)
//   tex.brick(x, y, bw, bh, mortar)   -> int (0=mortar, 255=brick)
//   tex.wood(x, y, rings, seed)       -> int (0-255 wood grain)
//   tex.marble(x, y, freq, seed)      -> int (0-255 marble veins)

#ifndef TEXTURE_RUNTIME_H
#define TEXTURE_RUNTIME_H

#include "codegen_internal.h"

void emit_tex_imports(Codegen *cg);   // No external imports needed
void emit_tex_bss(Codegen *cg);       // .bss globals (permutation table)
void emit_tex_data(Codegen *cg);      // .data section (constants)
void emit_tex_runtime(Codegen *cg);   // the _slag_tex_* procs (.text)

#endif // TEXTURE_RUNTIME_H
