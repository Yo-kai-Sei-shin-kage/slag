#ifndef CODEGEN_INTERNAL_H
#define CODEGEN_INTERNAL_H

#include <stdio.h>

typedef struct Codegen Codegen;

typedef struct {
    int has_key_down;
    int has_key_up;
    int has_mouse_move;
    int has_mouse_down;
    int has_mouse_up;
} EventHandlerFlags;


void cg_emit(Codegen *cg, const char *fmt, ...);
void cg_emit_raw(Codegen *cg, const char *fmt, ...);
int  cg_new_label(Codegen *cg);
int  cg_add_str_const(Codegen *cg, const char *s);
int  cg_add_float_const(Codegen *cg, double val);

#endif
