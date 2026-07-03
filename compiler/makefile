# Toolchain: MinGW-w64 gcc + NASM, targeting Win64 PE.
#
# Usage:
#   make            - build slag.exe (the compiler)
#   make hello.exe  - compile hello.slag -> hello.asm -> hello.obj -> hello.exe
#   make clean      - remove build artifacts

CC      = x86_64-w64-mingw32-gcc
CFLAGS  = -Wall -Wextra -O2
NASM    = nasm
NASMFLAGS = -f win64

LDLIBS  = -lkernel32 -luser32 -lgdi32 -lws2_32 -lwinmm

SRCS = main.c lexer.c ast.c parser.c codegen.c window_runtime.c net_runtime.c mem_runtime.c matrix_runtime.c simd_runtime.c mesh_runtime.c texture_runtime.c
OBJS = $(SRCS:.c=.o)

SLAG = slag.exe

.PHONY: all clean

all: $(SLAG)

$(SLAG): $(OBJS)
	$(CC) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Pattern rule: foo.slag -> foo.asm (via slag.exe)
%.asm: %.slag $(SLAG)
	./$(SLAG) $<

# Pattern rule: foo.asm -> foo.obj (via nasm)
%.obj: %.asm
	$(NASM) $(NASMFLAGS) $< -o $@

# Pattern rule: foo.obj -> foo.exe (via gcc, no CRT/startfiles)
%.exe: %.obj
	$(CC) $< -o $@ $(LDLIBS) -nostartfiles -e _start

clean:
	rm -f $(SLAG) $(OBJS) *.asm *.obj *.exe

