#ifndef MATRIX_RUNTIME_H
#define MATRIX_RUNTIME_H

#include "codegen_internal.h"

// Matrix stack runtime for 3D transformations.
// Provides a 3x4 matrix stack (rotation + translation, no perspective)
// with pre-computed trig tables for maximum performance.
//
// Builtins (wired in codegen.c as mat.*):
//   mat.identity()                -> reset current matrix to identity
//   mat.push()                    -> push current matrix onto stack
//   mat.pop()                     -> pop matrix from stack into current
//   mat.translate(x, y, z)        -> multiply translation into current
//   mat.rotate_x(angle)           -> multiply X rotation (angle in degrees)
//   mat.rotate_y(angle)           -> multiply Y rotation
//   mat.rotate_z(angle)           -> multiply Z rotation
//   mat.scale(sx, sy, sz)         -> multiply scale into current
//   mat.transform_x(x, y, z)      -> return transformed X coordinate
//   mat.transform_y(x, y, z)      -> return transformed Y coordinate
//   mat.transform_z(x, y, z)      -> return transformed Z coordinate
//
// Uses fixed-point 16.16 arithmetic internally for integer pipeline speed.
// Trig lookup tables: 256 entries, indexed by (angle % 256).

void emit_mat_imports(Codegen *cg);   // No Win32 imports needed
void emit_mat_data(Codegen *cg);      // .data section (sin/cos tables)
void emit_mat_bss(Codegen *cg);       // .bss section (matrix stack, current)
void emit_mat_runtime(Codegen *cg);   // the _slag_mat_* procs (.text)

#endif
