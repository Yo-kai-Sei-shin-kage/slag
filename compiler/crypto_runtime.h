#ifndef CRYPTO_RUNTIME_H
#define CRYPTO_RUNTIME_H

#include "codegen_internal.h"

// Crypto runtime (bcrypt.dll / CNG). ECDH P-256 key exchange + AES-256.
// Pure primitives -- no automatic dispatch from net_runtime.c or
// server_runtime.c. Script calls these manually around whichever
// transport (net.* or net.server_*) it is using.
// Call these from codegen_program alongside other runtime emitters.

void emit_crypto_imports(Codegen *cg);   // bcrypt extern decls
void emit_crypto_bss(Codegen *cg);       // .bss globals
void emit_crypto_runtime(Codegen *cg);   // the _slag_crypto_* procs (.text)

#endif
