// mesh_runtime.h — mesh management and BMP parsing for Slag.
//
// BMP parsing (24-bit uncompressed):
//   mesh.bmp_width(ptr)           -> int (image width)
//   mesh.bmp_height(ptr)          -> int (image height)
//   mesh.bmp_pixel(ptr, x, y)     -> int (0x00RRGGBB)
//   mesh.bmp_gray(ptr, x, y)      -> int (0-255 grayscale)
//
// Mesh management:
//   mesh.create(verts, faces)     -> handle (ptr to mesh struct)
//   mesh.free(handle)             -> void
//   mesh.vertex_count(handle)     -> int
//   mesh.face_count(handle)       -> int
//   mesh.set_vertex(h, i, x, y, z)-> void
//   mesh.get_vertex_x(h, i)       -> int (16.16 fixed)
//   mesh.get_vertex_y(h, i)       -> int (16.16 fixed)
//   mesh.get_vertex_z(h, i)       -> int (16.16 fixed)
//   mesh.set_face(h, i, v0, v1, v2) -> void
//   mesh.get_face(h, i, c)        -> int (vertex index, c=0/1/2)
//
// Heightmap generation:
//   mesh.from_heightmap(bmp_ptr, scale_xz, scale_y) -> handle
//
// Mesh struct layout (heap allocated):
//   offset 0:  vertex_count (8 bytes)
//   offset 8:  face_count (8 bytes)
//   offset 16: vertices_ptr (8 bytes) -> [x0,y0,z0, x1,y1,z1, ...] int64 triples
//   offset 24: faces_ptr (8 bytes) -> [v0,v1,v2, ...] int64 triples

#ifndef MESH_RUNTIME_H
#define MESH_RUNTIME_H

#include "codegen_internal.h"

void emit_mesh_imports(Codegen *cg);   // No external imports needed
void emit_mesh_bss(Codegen *cg);       // .bss globals
void emit_mesh_data(Codegen *cg);      // .data constants
void emit_mesh_runtime(Codegen *cg);   // the _slag_mesh_* procs (.text)

#endif // MESH_RUNTIME_H
