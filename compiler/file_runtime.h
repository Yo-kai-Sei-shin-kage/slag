#ifndef FILE_RUNTIME_H
#define FILE_RUNTIME_H

#include "codegen_internal.h"

// File I/O runtime. Mirrors mem_runtime.c / net_runtime.c.
// Handle-based file primitives (open/close/read/write/seek/size) plus
// path-based utility ops (exists/delete/mkdir). Paths are passed as
// null-terminated C strings (string literals in Slag are always
// null-terminated; see emit_data's string-constant comment).
//
// Builtins (wired in codegen.c as file.*):
//   file.open(path, mode)         -> int handle (-1 on fail)
//                                    mode: 1=read, 2=write(truncate), 3=append
//   file.close(handle)            -> (void)
//   file.read(handle, buf, n)     -> int bytes read (-1 on fail)
//   file.write(handle, buf, n)    -> int bytes written (-1 on fail)
//   file.seek(handle, offset, whence) -> int new position (-1 on fail)
//                                    whence: 0=start, 1=current, 2=end
//   file.size(handle)             -> int size in bytes (-1 on fail)
//   file.exists(path)             -> int bool (1/0)
//   file.delete(path)             -> int bool (1/0)
//   file.mkdir(path)              -> int bool (1/0)
//
// Per-handle directory listing (each search's state, including the
// Win32 WIN32_FIND_DATAA buffer, is heap-allocated independently so
// multiple listings can be open/interleaved at once -- mirrors how
// window_runtime.c gives each window its own heap-allocated state
// struct rather than one shared buffer):
//   file.list_open(pattern)       -> int handle (-1 on fail)
//                                    pattern is a glob like "dir/*" or
//                                    "dir/*.ext" (Win32 FindFirstFileA
//                                    semantics, not a bare directory path)
//   file.list_next(handle)        -> int bool (1 = advanced to next entry,
//                                    0 = no more entries)
//   file.list_name(handle)        -> str (current entry's filename)
//   file.list_close(handle)       -> (void)

void emit_file_imports(Codegen *cg);   // WriteFile/SetFilePointer/DeleteFileA/FindFirstFileA/
                                        // FindNextFileA/FindClose/GetFileAttributesA/CreateDirectoryA externs
void emit_file_bss(Codegen *cg);       // (none currently; kept for symmetry)
void emit_file_runtime(Codegen *cg);   // the _slag_file_* procs (.text)

#endif
