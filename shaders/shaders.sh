#!/usr/bin/env bash
set -euo pipefail

# ============================================================
# SLAG shader rebuild script
#
# Compile all SLAG HLSL shaders with Microsoft's FXC, produce
# CSO files, and generate one monolithic include containing
# NASM-style E("...") statements for gpu_runtime.c.
#
# Generated:
#
#     *.cso
#     gpu_shader_blobs.inc
#
# gpu_runtime.c includes the generated blob file from inside:
#
#     emit_gpu_data(Codegen *cg)
#
# The generated include deliberately uses the existing NASM
# emitter format:
#
#     E("_gpu_vs_blob:  ; 3560 bytes DXBC");
#     E("    db 68,88,66,67,...");
#     E("_gpu_vs_blob_len equ 3560");
#     E("align 16");
#
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ------------------------------------------------------------
# FXC
#
# Use the known-working Windows SDK installation.
# ------------------------------------------------------------

FXC="/cygdrive/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x86/fxc.exe"

OUT_INC="$SCRIPT_DIR/gpu_shader_blobs.inc"
TMP_INC="$OUT_INC.tmp"

# ------------------------------------------------------------
# Check compiler
# ------------------------------------------------------------

if [[ ! -f "$FXC" ]]; then
    echo "ERROR: fxc.exe not found:"
    echo "  $FXC"
    exit 1
fi

# ------------------------------------------------------------
# Explicit shader build targets.
#
# Format:
#
#   source|entry|profile|output.cso|blob_symbol
#
# The blob symbols MUST match the symbols expected by the
# existing gpu_runtime.c.
# ------------------------------------------------------------

SHADER_TARGETS=(
    "gpu.vs.hlsl|main|vs_5_0|gpu.vs.cso|_gpu_vs_blob"
    "gpu_ps.hlsl|main|ps_5_0|gpu_ps.cso|_gpu_ps_blob"

    "gpu_tess_vs.hlsl|main|vs_5_0|gpu_tess_vs.cso|_gpu_tvs_blob"
    "gpu_tess_hs.hlsl|main|hs_5_0|gpu_tess_hs.cso|_gpu_ths_blob"
    "gpu_tess_ds.hlsl|main|ds_5_0|gpu_tds.cso|_gpu_tds_blob"

    "physics.hlsl|main|cs_5_0|physics_integrate.cso|_gpu_cs_integrate_blob"
    "physics.hlsl|ClearImpulses|cs_5_0|physics_clear.cso|_gpu_cs_clear_blob"
    "physics.hlsl|ResolveBodyPairs|cs_5_0|physics_resolve.cso|_gpu_cs_resolve_blob"
    "physics.hlsl|ApplyImpulses|cs_5_0|physics_apply.cso|_gpu_cs_apply_blob"

    "particles.hlsl|Emit|cs_5_0|particles_emit.cso|_gpu_cs_pemit_blob"
    "particles.hlsl|Simulate|cs_5_0|particles_sim.cso|_gpu_cs_psim_blob"
)

# ------------------------------------------------------------
# Compile one explicit shader target.
# ------------------------------------------------------------

compile_target()
{
    local src="$1"
    local entry="$2"
    local profile="$3"
    local cso="$4"

    echo "============================================================"
    echo "Compiling: $src"
    echo "  entry:   $entry"
    echo "  profile: $profile"
    echo "  output:  $cso"
    echo "============================================================"

    rm -f "$cso"

    "$FXC" \
        /nologo \
        /T "$profile" \
        /E "$entry" \
        /Fo "$cso" \
        "$src"

    if [[ ! -s "$cso" ]]; then
        echo "ERROR: fxc produced no CSO:"
        echo "  $cso"
        exit 1
    fi

    echo "OK: $cso"
    echo
}

# ------------------------------------------------------------
# Convert CSO to:
#
#     E("_shader_blob:  ; N bytes DXBC");
#     E("    db 1,2,3,...");
#     ...
#     E("_shader_blob_len equ N");
#     E("align 16");
#
# This deliberately preserves the existing NASM-style format.
# ------------------------------------------------------------

emit_blob()
{
    local cso="$1"
    local symbol="$2"

    local size
    size="$(wc -c < "$cso")"

    echo "    E(\"$symbol:  ; $size bytes DXBC\");" >> "$TMP_INC"

    # Python is used only as a binary-to-byte converter.
    # The generated output remains NASM-style E("...") text.
    python3 - "$cso" "$TMP_INC" <<'PY'
import sys

cso = sys.argv[1]
out = sys.argv[2]

with open(cso, "rb") as f:
    data = f.read()

with open(out, "a", encoding="utf-8") as f:
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        nums = ",".join(str(b) for b in chunk)
        f.write(f'    E("    db {nums}");\n')
PY

    echo "    E(\"${symbol}_len equ ${size}\");" >> "$TMP_INC"
    echo "    E(\"align 16\");" >> "$TMP_INC"
    echo >> "$TMP_INC"
}

# ------------------------------------------------------------
# Show all shader targets.
# ------------------------------------------------------------

echo
echo "SLAG shader rebuild"
echo
echo "Using FXC:"
echo "  $FXC"
echo

echo "Shader targets:"
for target in "${SHADER_TARGETS[@]}"; do
    IFS='|' read -r src entry profile cso symbol <<< "$target"

    printf '  %-24s %-22s %-10s -> %s\n' \
        "$src" \
        "$entry" \
        "$profile" \
        "$symbol"
done
echo

# ------------------------------------------------------------
# Compile EVERYTHING first.
#
# If any shader fails, the existing include remains untouched.
# ------------------------------------------------------------

for target in "${SHADER_TARGETS[@]}"; do
    IFS='|' read -r src entry profile cso symbol <<< "$target"

    compile_target "$src" "$entry" "$profile" "$cso"
done

# ------------------------------------------------------------
# Build temporary monolithic include.
# ------------------------------------------------------------

rm -f "$TMP_INC"

cat > "$TMP_INC" <<'EOF'
/*
 * AUTO-GENERATED FILE -- DO NOT EDIT.
 *
 * Generated by:
 *     shaders/shaders.sh
 *
 * This file contains the compiled DXBC blobs for the SLAG
 * GPU runtime.
 *
 * Each blob is emitted as NASM-style E("...") statements.
 *
 * Do not manually modify the byte data.
 */
EOF

echo >> "$TMP_INC"

# ------------------------------------------------------------
# Embed every compiled target.
# ------------------------------------------------------------

for target in "${SHADER_TARGETS[@]}"; do
    IFS='|' read -r src entry profile cso symbol <<< "$target"

    echo "Embedding: $cso -> $symbol"

    emit_blob "$cso" "$symbol"
done

# ------------------------------------------------------------
# Atomic replacement.
#
# The old working include survives until the complete new file
# has been generated successfully.
# ------------------------------------------------------------

mv -f "$TMP_INC" "$OUT_INC"

# ------------------------------------------------------------
# Summary
# ------------------------------------------------------------

echo
echo "============================================================"
echo "Shader rebuild successful."
echo "============================================================"
echo

echo "Generated CSOs:"
for target in "${SHADER_TARGETS[@]}"; do
    IFS='|' read -r src entry profile cso symbol <<< "$target"
    echo "  $cso"
done

echo
echo "Generated monolithic include:"
echo "  $OUT_INC"
echo

echo "Expected blob symbols:"
for target in "${SHADER_TARGETS[@]}"; do
    IFS='|' read -r src entry profile cso symbol <<< "$target"
    echo "  ${symbol}"
    echo "  ${symbol}_len"
done

echo
echo "gpu_runtime.c should include:"
echo
echo '    #include "../shaders/gpu_shader_blobs.inc"'
echo
echo "inside emit_gpu_data(Codegen *cg), replacing the"
echo "hand-embedded DXBC blob E(...) statements."
echo
