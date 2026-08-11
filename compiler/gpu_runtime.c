// D3D11 GPU runtime emitter. Detection (DXGI) + device/swapchain (D3D11).
#include <stdio.h>
#include "codegen_internal.h"
#include "gpu_runtime.h"

#define E(fmt, ...) cg_emit(cg, fmt, ##__VA_ARGS__)

static void emit_gpu_create_device(Codegen *cg);
static void emit_gpu_create_pipeline(Codegen *cg);
static void emit_gpu_stage_init(Codegen *cg);
static void emit_gpu_stage_pcolor(Codegen *cg);
static void emit_gpu_present_frame(Codegen *cg);

// dxgi.dll enumeration + d3d11.dll device/swapchain creation.
void emit_gpu_imports(Codegen *cg) {
    E("extern CreateDXGIFactory1");
    E("extern D3D11CreateDeviceAndSwapChain");
}

void emit_gpu_bss(Codegen *cg) {
    E("_gpu_present:   resq 1");   // 1 if a supported adapter was found
    E("_gpu_vendor:    resq 1");   // 0=none 1=Intel 2=AMD 3=NVIDIA
    E("_gpu_discrete:  resq 1");   // 1=discrete (UMA=0), 0=integrated (UMA=1); set once at device create
    E("_gpu_ready:     resq 1");   // 1 once device+swapchain+RTV are live
    E("_gpu_factory:   resq 1");   // IDXGIFactory1*
    E("_gpu_adapter:   resq 1");   // selected IDXGIAdapter1* (discrete if any, else integrated)");
    E("_gpu_cand_d:    resq 1");   // discrete candidate adapter (DedicatedVideoMemory>0), 0 if none");
    E("_gpu_vend_d:    resq 1");   // vendor code of discrete candidate");
    E("_gpu_cand_i:    resq 1");   // integrated candidate adapter (DedicatedVideoMemory==0), 0 if none");
    E("_gpu_vend_i:    resq 1");   // vendor code of integrated candidate");
    E("_gpu_device:    resq 1");   // ID3D11Device*
    E("_gpu_context:   resq 1");   // ID3D11DeviceContext*
    E("_gpu_swapchain: resq 1");   // IDXGISwapChain*
    E("_gpu_rtv:       resq 1");   // ID3D11RenderTargetView* (backbuffer)
    E("_gpu_vs:        resq 1");   // ID3D11VertexShader*
    E("_gpu_ps:        resq 1");   // ID3D11PixelShader*
    E("_gpu_layout:    resq 1");   // ID3D11InputLayout*
    E("_gpu_vbuf:      resq 1");   // ID3D11Buffer* dynamic vertex buffer
    E("_gpu_cbuf:      resq 1");   // ID3D11Buffer* constant buffer (viewport)
    E("_gpu_tex:       resq 1");   // ID3D11Texture2D* (512x512 BGRA)
    E("_gpu_srv:       resq 1");   // ID3D11ShaderResourceView*
    E("_gpu_sampler:   resq 1");   // ID3D11SamplerState* (point+clamp, s0: world textures)
    E("_gpu_sampler_lin: resq 1"); // ID3D11SamplerState* (linear+clamp, s1: SDF text)
    E("_gpu_sampler_cmp: resq 1"); // ID3D11SamplerState* (comparison LESS_EQUAL, s2: shadow PCF)
    E("_gpu_raster:    resq 1");   // ID3D11RasterizerState* (CULL_BACK, FrontCW = D3D default)
    E("_gpu_pipeline:  resq 1");   // 1 once all pipeline objects created
    E("_gpu_stage:     resq 1");   // heap buffer of staged raw pcolor verts
    E("_gpu_convbuf:   resq 1");   // cached scratch for converted float verts (bulk-copied to WC vbuf)
    E("_gpu_cap:       resq 1");   // current buffer capacity in triangles (grows on demand)
    E("_gpu_stage_cnt: resq 1");   // number of triangles staged this frame
    E("_gpu_stage_tex: resq 1");   // tex_ptr of last staged triangle (FTEX pixels)
    E("_gpu_tex_uploaded: resq 1"); // tex_ptr last sent via UpdateSubresource; skip re-upload when unchanged
    E("_gpu_vbuf2:     resq 1");    // 2nd dynamic vertex buffer (double-buffered: CPU writes one while GPU reads the other)
    E("_gpu_vbuf_idx:  resq 1");    // 0/1 -> which vbuf the CPU maps this frame
    E("_gpu_prebuilt:  resq 1");    // 1 = _gpu_convbuf already holds float32 verts (fill_triangle_gpu bulk path); present skips the int64->float convert
    E("_gpu_up_verts:  resq 1");     // verts ptr last uploaded to the GPU vbuf
    E("_gpu_up_count:  resq 1");     // tri count last uploaded
    E("_gpu_up_valid:  resq 1");     // 1 once a vbuf has been filled (both vbufs hold the geometry)
    E("_gpu_vbuf_dirty: resq 1");    // 1 = convbuf changed this frame -> re-upload needed");
    E("_gpu_cb_wh:     resq 1");     // packed (w<<32)|h last written to the constant buffer; skip cbuf Map when unchanged");
    E("_gpu_state_set: resq 1");     // 1 once the invariant pipeline state (layout/shaders/sampler/SRV/topology/raster) is bound; skip re-binding every frame");
    E("_gpu_waitable:  resq 1");     // frame-latency waitable HANDLE (flip swapchain); WaitForSingleObject each frame to pace CPU->GPU");
    E("_gpu_frame:     resq 1");     // frame counter -> shader time (motion animation)");
    E("_gpu_depthtex:  resq 1");     // ID3D11Texture2D* depth buffer (D32_FLOAT)");
    E("_gpu_dsv:       resq 1");     // ID3D11DepthStencilView*");
    E("_gpu_sc_w:      resq 1");     // swapchain backbuffer width  (last sized)");
    E("_gpu_sc_h:      resq 1");     // swapchain backbuffer height (last sized); resize when window differs");
    E("_gpu_dsstate:   resq 1");     // ID3D11DepthStencilState* (depth test+write, LESS)");
    E("_gpu_viewproj:  resq 1");     // ptr to 16 float32 (4x4 view-projection matrix) from the Slag camera; copied into the cbuf each frame");
    E("_gpu_stage_texw: resq 1");  // tex_w of staged triangles
    E("_gpu_stage_texh: resq 1");  // tex_h of staged triangles
    E("_gpu_clear_ptr:  resq 1");  // ptr to 4 x f32 (R,G,B,A) clear color from gpu.clear
    E("_gpu_clear_set:  resq 1");  // 1 once gpu.clear set a color; else fog fallback
    E("_gpu_blend:      resq 1");  // ID3D11BlendState* (straight-alpha), created at init
    E("_gpu_blend_mode: resq 1");  // 0=opaque (NULL state), 1=alpha blend (_gpu_blend)
    E("_gpu_shadowtex:  resq 1");  // ID3D11Texture2D* shadow depth map (R32_TYPELESS 2048x2048)");
    E("_gpu_shadow_dsv: resq 1");  // ID3D11DepthStencilView* (D32) for the shadow pass");
    E("_gpu_shadow_srv: resq 1");  // ID3D11ShaderResourceView* (R32_FLOAT) sampled in the main pass at t2");
    E("_gpu_lightproj:  resq 1");  // ptr to 16 f32 light view-projection (gpu.set_lightproj); 0 = no shadows");
    E("_gpu_lightdir:   resq 1");  // ptr to 3 f32 world light direction (gpu.set_lightproj arg2 tail)");
    E("_gpu_shadowpass_val: resq 1"); // 0/1 shadowPass flag written into the cbuf; set per pass by present");
    // Draw-list: each fill_triangle_gpu appends one item so a frame can contain
    // multiple draws with independent viewproj/light (e.g. a world-space 3D mesh
    // draw + the ortho 2D UI draw). present iterates them, one Present at the end.
    // Item = 8 qwords: startVertex, vertexCount, tex, texw, texh, viewproj,
    // lightproj, lightdir. Up to GPU_DRAW_MAX items.
    E("_gpu_draw_items: resq 128");   // 16 items x 8 qwords");
    E("_gpu_draw_cnt:   resq 1");     // number of draw items appended this frame");
    E("_gpu_stage_off:  resq 1");     // running vertex-count offset into _gpu_convbuf (verts, not tris)");
    E("_gpu_cur_start:  resq 1");     // current draw item's StartVertexLocation (present loop)");
    E("_gpu_cur_count:  resq 1");     // current draw item's VertexCount (present loop)");
}
// Per triangle: 3 verts x 8 int64 = 192 bytes raw. Cap 4096 tris/frame.
// GPU_STAGE_CAP triangles * GPU_STAGE_TRI bytes.
#define GPU_STAGE_CAP_C 4096

// _slag_gpu_detect() -> rax = _gpu_vendor (1 Intel / 2 AMD / 3 NVIDIA / 0 none).
// Enumerates ALL adapters, reads DXGI_ADAPTER_DESC1, and auto-selects the best:
// a discrete GPU is preferred over an integrated one. The unique, device-free
// discriminator is DedicatedVideoMemory (desc+272): a dGPU reports a nonzero
// dedicated-VRAM pool "not shared with the CPU"; an iGPU has none (its memory is
// shared system RAM, reported under SharedSystemMemory), so DedicatedVideoMemory
// == 0 marks integrated and > 0 marks discrete. The scan keeps the first discrete
// candidate and the first integrated candidate, releasing all others, then binds
// _gpu_adapter to the discrete one if found, else the integrated one. Accepts
// Intel 0x8086 / AMD 0x1002 / NVIDIA 0x10de; other vendors are skipped. The live
// device's UMA flag later confirms the type authoritatively into _gpu_discrete.
void emit_gpu_runtime(Codegen *cg) {
    // DXGI_ADAPTER_DESC1 is 312 (0x138) bytes; VendorId at +256.
    E("; --- DXGI_ADAPTER_DESC1 field offsets ---");
    E("GPU_DESC_VENDORID    equ 256");
    E("GPU_DESC_DEDVIDMEM   equ 272");   // SIZE_T DedicatedVideoMemory (8B)
    E("");
    // Frame layout (below 4 pushes): [rsp+0x00..0x1F] shadow space,
    // [rsp+0x20..0x2F] IID, [rsp+0x30..0x167] desc buffer (312 bytes).
    // 4 pushes (32) keep entry-RSP's ...8 state; sub 0x178 (mod 16 == 8)
    // realigns to 16 before each call, and 0x178 fits 0x30+0x138.
    E("; --- _slag_gpu_detect -> rax (_gpu_vendor) ---");
    E("_slag_gpu_detect:");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    push r12");
    E("    sub  rsp, 0x178            ; shadow + IID + DXGI_ADAPTER_DESC1(0x138)");

    // Reset candidate slots so a repeat gpu.detect() call starts clean (the
    // slots persist in .bss; stale nonzero values would skip every adapter).
    E("    xor  eax, eax");
    E("    mov  [_gpu_cand_d], rax");
    E("    mov  [_gpu_vend_d], rax");
    E("    mov  [_gpu_cand_i], rax");
    E("    mov  [_gpu_vend_i], rax");
    E("    mov  [_gpu_adapter], rax");

    // IID_IDXGIFactory1 = {770aae78-f26f-4dba-a829-253c83d1b387}
    E("    mov  dword [rsp+0x20], 0x770aae78");
    E("    mov  dword [rsp+0x24], 0x4dbaf26f");
    E("    mov  dword [rsp+0x28], 0x3c2529a8");
    E("    mov  dword [rsp+0x2c], 0x87b3d183");

    // CreateDXGIFactory1(&IID, (void**)&_gpu_factory)
    E("    lea  rcx, [rsp+0x20]");
    E("    lea  rdx, [_gpu_factory]");
    E("    call CreateDXGIFactory1");
    E("    test eax, eax");
    E("    jnz  .gpu_none            ; HRESULT<0 -> no DXGI, bail");

    E("    xor  r12d, r12d           ; adapter index i");
    E(".gpu_enum_loop:");
    // factory->EnumAdapters1(i, &adapter) -- vtable slot 12 (0x60)
    E("    mov  rcx, [_gpu_factory]");
    E("    mov  rax, [rcx]");
    E("    mov  edx, r12d");
    E("    lea  r8, [_gpu_adapter]");
    E("    call [rax + 0x60]         ; IDXGIFactory1::EnumAdapters1");
    E("    test eax, eax");
    E("    jnz  .gpu_none            ; NOT_FOUND -> all adapters scanned, pick best");

    // adapter->GetDesc1(&desc) -- IDXGIAdapter1 vtable slot 10 (0x50)
    E("    mov  rcx, [_gpu_adapter]");
    E("    mov  rax, [rcx]");
    E("    lea  rdx, [rsp+0x30]");
    E("    call [rax + 0x50]         ; IDXGIAdapter1::GetDesc1");

    // Classify vendor (Intel/AMD/NVIDIA accepted; all else skipped). r10d = the
    // vendor code for this adapter; the DedicatedVideoMemory test below routes it
    // to the integrated or discrete slot. No early exit -- every adapter scanned.
    E("    mov  eax, [rsp+0x30+GPU_DESC_VENDORID]");
    E("    xor  r10d, r10d");
    E("    cmp  eax, 0x8086          ; Intel");
    E("    je   .gpu_v_intel");
    E("    cmp  eax, 0x1002          ; AMD");
    E("    je   .gpu_v_amd");
    E("    cmp  eax, 0x10de          ; NVIDIA");
    E("    je   .gpu_v_nvidia");
    E("    jmp  .gpu_next            ; unknown vendor -> skip");
    E(".gpu_v_intel:");
    E("    mov  r10d, 1");
    E("    jmp  .gpu_classify");
    E(".gpu_v_amd:");
    E("    mov  r10d, 2");
    E("    jmp  .gpu_classify");
    E(".gpu_v_nvidia:");
    E("    mov  r10d, 3");

    E(".gpu_classify:");
    // DedicatedVideoMemory (8B) == 0 -> integrated slot; > 0 -> discrete slot.
    E("    mov  rax, [rsp+0x30+GPU_DESC_DEDVIDMEM]");
    E("    test rax, rax");
    E("    jnz  .gpu_slot_d          ; nonzero dedicated VRAM -> discrete");

    // Integrated candidate: keep the first one seen, release later duplicates.
    E("    cmp  qword [_gpu_cand_i], 0");
    E("    jne  .gpu_next            ; already have an iGPU -> release this one");
    E("    mov  rax, [_gpu_adapter]");
    E("    mov  [_gpu_cand_i], rax");
    E("    mov  [_gpu_vend_i], r10");
    E("    mov  qword [_gpu_adapter], 0   ; adapter now owned by the slot");
    E("    jmp  .gpu_next_skip");

    E(".gpu_slot_d:");
    // Discrete candidate: keep the first one seen, release later duplicates.
    E("    cmp  qword [_gpu_cand_d], 0");
    E("    jne  .gpu_next            ; already have a dGPU -> release this one");
    E("    mov  rax, [_gpu_adapter]");
    E("    mov  [_gpu_cand_d], rax");
    E("    mov  [_gpu_vend_d], r10");
    E("    mov  qword [_gpu_adapter], 0   ; adapter now owned by the slot");
    E("    jmp  .gpu_next_skip");

    E(".gpu_next:");
    // Release the unretained adapter, advance i.
    E("    mov  rcx, [_gpu_adapter]");
    E("    test rcx, rcx");
    E("    jz   .gpu_next_skip");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]         ; IUnknown::Release");
    E("    mov  qword [_gpu_adapter], 0");
    E(".gpu_next_skip:");
    E("    inc  r12d");
    E("    jmp  .gpu_enum_loop");

    // Enumeration exhausted (EnumAdapters1 returned NOT_FOUND). Pick the best
    // candidate: discrete if present, else integrated. Release the loser.
    E(".gpu_none:");
    E("    cmp  qword [_gpu_cand_d], 0");
    E("    je   .gpu_pick_i          ; no discrete -> try integrated");
    // Discrete wins. Bind it; release the unused integrated candidate.
    E("    mov  rax, [_gpu_cand_d]");
    E("    mov  [_gpu_adapter], rax");
    E("    mov  rax, [_gpu_vend_d]");
    E("    mov  [_gpu_vendor], rax");
    E("    mov  rcx, [_gpu_cand_i]");
    E("    test rcx, rcx");
    E("    jz   .gpu_have");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]         ; Release unused iGPU candidate");
    E("    mov  qword [_gpu_cand_i], 0");
    E("    jmp  .gpu_have");

    E(".gpu_pick_i:");
    E("    cmp  qword [_gpu_cand_i], 0");
    E("    je   .gpu_truly_none      ; no adapter at all");
    E("    mov  rax, [_gpu_cand_i]");
    E("    mov  [_gpu_adapter], rax");
    E("    mov  rax, [_gpu_vend_i]");
    E("    mov  [_gpu_vendor], rax");

    E(".gpu_have:");
    E("    mov  qword [_gpu_present], 1");
    E("    mov  rax, [_gpu_vendor]");
    E("    jmp  .gpu_ret");

    E(".gpu_truly_none:");
    E("    mov  qword [_gpu_present], 0");
    E("    mov  qword [_gpu_vendor], 0");
    E("    xor  eax, eax");

    E(".gpu_ret:");
    E("    add  rsp, 0x178");
    E("    pop  r12");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    ret");

    // Init dispatch: any accepted vendor (1 Intel / 2 AMD / 3 NVIDIA) uses the
    // same adapter-agnostic D3D11 device/swapchain path off _gpu_adapter; only a
    // zero (no adapter) vendor bails.
    E("");
    E("; --- _slag_gpu_init_dispatch (branch on _gpu_vendor) ---");
    E("_slag_gpu_init_dispatch:");
    E("    mov  rax, [_gpu_vendor]");
    E("    test rax, rax");
    E("    jz   .gid_none");
    E("    jmp  _slag_gpu_create_device");
    E(".gid_none:");
    E("    ret");

    emit_gpu_create_device(cg);
    emit_gpu_create_pipeline(cg);
    emit_gpu_stage_init(cg);
    emit_gpu_stage_pcolor(cg);
    emit_gpu_present_frame(cg);
}

// _slag_gpu_create_device: D3D11CreateDeviceAndSwapChain on _gpu_adapter, bound
// to the primary window HWND; then fetch the backbuffer (GetBuffer) and make an
// RTV (CreateRenderTargetView). Sets _gpu_ready=1 on full success.
static void emit_gpu_create_device(Codegen *cg) {
    // DXGI_SWAP_CHAIN_DESC field offsets (size 72, verified against dxgi.h).
    E("; --- DXGI_SWAP_CHAIN_DESC offsets ---");
    E("SCD_WIDTH        equ 0");    // BufferDesc.Width
    E("SCD_HEIGHT       equ 4");    // BufferDesc.Height
    E("SCD_FORMAT       equ 16");   // BufferDesc.Format
    E("SCD_SAMPLE_CNT   equ 28");   // SampleDesc.Count
    E("SCD_SAMPLE_QUAL  equ 32");   // SampleDesc.Quality
    E("SCD_BUFUSAGE     equ 36");   // BufferUsage");
    E("SCD_BUFCOUNT     equ 40");   // BufferCount");
    E("SCD_OUTWINDOW    equ 48");   // OutputWindow (HWND)");
    E("SCD_WINDOWED     equ 56");   // Windowed (BOOL)");
    E("SCD_SWAPEFFECT   equ 60");   // SwapEffect");
    E("SCD_FLAGS        equ 64");   // Flags");
    E("DXGI_FMT_BGRA8       equ 87");        // DXGI_FORMAT_B8G8R8A8_UNORM
    E("DXGI_USAGE_RTOUT     equ 32");        // DXGI_USAGE_RENDER_TARGET_OUTPUT
    E("D3D_DRIVER_UNKNOWN   equ 0");         // required when adapter != NULL
    E("D3D11_SDK_VER        equ 7");
    E("FEATURE_LEVEL_11_0   equ 0xb000");
    E("");
    // Frame: shadow(0x20) + stack args 5..12 (0x40) = 0x60 arg area,
    // then SCD(0x48), IID buf(0x10), fl local(8), obtained-fl(8), backbuffer(8).
    // Locals placed above the 0x60 arg area.
    E("; --- _slag_gpu_create_device ---");
    E("_slag_gpu_create_device:");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    push r12");
    E("    sub  rsp, 0x108           ; arg area + SCD + IID + locals (16-aligned)");

    // Resolve primary window struct (HWND at +0). Bail if no window yet.
    E("    mov  rbx, [_window_primary_state]");
    E("    test rbx, rbx");
    E("    jz   .cd_fail");

    // Zero the SCD (at rsp+0x60, 72 bytes).
    E("    lea  rdi, [rsp+0x60]");
    E("    xor  eax, eax");
    E("    mov  ecx, 9               ; 9 qwords = 72 bytes");
    E("    rep  stosq");

    // Fill SCD from the window's live client size.
    E("    mov  eax, [rbx + 48]      ; WSTATE_WIDTH");
    E("    mov  [rsp+0x60+SCD_WIDTH], eax");
    E("    mov  eax, [rbx + 56]      ; WSTATE_HEIGHT");
    E("    mov  [rsp+0x60+SCD_HEIGHT], eax");
    E("    mov  dword [rsp+0x60+SCD_FORMAT], DXGI_FMT_BGRA8");
    E("    mov  dword [rsp+0x60+SCD_SAMPLE_CNT], 1     ; no MSAA");
    E("    mov  dword [rsp+0x60+SCD_BUFUSAGE], DXGI_USAGE_RTOUT");
    E("    mov  dword [rsp+0x60+SCD_BUFCOUNT], 3       ; triple-buffered (flip-model: deeper queue so Map(WRITE_DISCARD) does not block on the GPU)");
    E("    mov  rax, [rbx + 0]       ; WSTATE_HWND");
    E("    mov  [rsp+0x60+SCD_OUTWINDOW], rax");
    E("    mov  dword [rsp+0x60+SCD_WINDOWED], 1");
    E("    mov  dword [rsp+0x60+SCD_SWAPEFFECT], 4     ; DXGI_SWAP_EFFECT_FLIP_DISCARD (no DWM blit; ~1.5ms/frame Present floor removed)");
    // ALLOW_TEARING: flip-model honors vsync even at SyncInterval=0 UNLESS the
    // swapchain sets this flag AND Present passes DXGI_PRESENT_ALLOW_TEARING.
    // Both together uncap the frame rate; either alone is ignored/errors.
    // 0x800 ALLOW_TEARING | 0x40 FRAME_LATENCY_WAITABLE_OBJECT: the waitable
    // object lets us block briefly at frame top (paced to the GPU) instead of
    // stalling ~30ms inside Present when the CPU runs too far ahead.
    E("    mov  dword [rsp+0x60+SCD_FLAGS], 0x840");

    // Feature level array (single entry) at rsp+0xB0.
    E("    mov  dword [rsp+0xB0], FEATURE_LEVEL_11_0");

    // D3D11CreateDeviceAndSwapChain(adapter, UNKNOWN, NULL, 0, &fl, 1,
    //   SDK_VER, &scd, &swapchain, &device, &obtained_fl, &context)
    E("    mov  rcx, [_gpu_adapter]           ; adapter");
    E("    xor  edx, edx                      ; D3D_DRIVER_TYPE_UNKNOWN");
    E("    xor  r8,  r8                       ; swrast HMODULE = NULL");
    E("    xor  r9,  r9                       ; flags = 0");
    E("    lea  rax, [rsp+0xB0]               ; &feature_levels");
    E("    mov  [rsp+0x20], rax");
    E("    mov  qword [rsp+0x28], 1           ; levels count");
    E("    mov  qword [rsp+0x30], D3D11_SDK_VER");
    E("    lea  rax, [rsp+0x60]               ; &scd");
    E("    mov  [rsp+0x38], rax");
    E("    lea  rax, [_gpu_swapchain]         ; &swapchain");
    E("    mov  [rsp+0x40], rax");
    E("    lea  rax, [_gpu_device]            ; &device");
    E("    mov  [rsp+0x48], rax");
    E("    lea  rax, [rsp+0xB8]               ; &obtained_feature_level");
    E("    mov  [rsp+0x50], rax");
    E("    lea  rax, [_gpu_context]           ; &immediate_context");
    E("    mov  [rsp+0x58], rax");
    E("    call D3D11CreateDeviceAndSwapChain");
    E("    test eax, eax");
    E("    jnz  .cd_fail                      ; HRESULT<0");

    // swapchain->GetBuffer(0, IID_ID3D11Texture2D, &backbuffer) -- slot 9 (0x48)
    // IID_ID3D11Texture2D = {6f15aaf2-d208-4e89-9ab4-489535d34f9c}
    E("    mov  dword [rsp+0xC0], 0x6f15aaf2");
    E("    mov  dword [rsp+0xC4], 0x4e89d208");
    E("    mov  dword [rsp+0xC8], 0x9548b49a");
    E("    mov  dword [rsp+0xCC], 0x9c4fd335");
    E("    mov  rcx, [_gpu_swapchain]");
    E("    mov  rax, [rcx]");
    E("    xor  edx, edx                      ; buffer index 0");
    E("    lea  r8,  [rsp+0xC0]               ; riid");
    E("    lea  r9,  [rsp+0xD0]               ; &backbuffer (ID3D11Texture2D*)");
    E("    call [rax + 0x48]                  ; IDXGISwapChain::GetBuffer");
    E("    test eax, eax");
    E("    jnz  .cd_fail");

    // device->CreateRenderTargetView(backbuffer, NULL, &_gpu_rtv) -- slot 9 (0x48)
    E("    mov  rcx, [_gpu_device]");
    E("    mov  rax, [rcx]");
    E("    mov  rdx, [rsp+0xD0]               ; backbuffer resource");
    E("    xor  r8,  r8                       ; default RTV desc");
    E("    lea  r9,  [_gpu_rtv]");
    E("    call [rax + 0x48]                  ; ID3D11Device::CreateRenderTargetView");
    E("    test eax, eax");
    E("    jnz  .cd_fail_release_bb");

    // Release the backbuffer texture ref (RTV holds its own) -- Release slot 2.
    E("    mov  rcx, [rsp+0xD0]");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]");

    E("    mov  qword [_gpu_ready], 1");

    // Record the initial backbuffer size (window client area at create time) so
    // the present frame can detect a later resize and call ResizeBuffers.
    E("    mov  rsi, [_window_primary_state]");
    E("    test rsi, rsi");
    E("    jz   .cd_nosize");
    E("    mov  eax, [rsi + 48]               ; WSTATE_WIDTH");
    E("    mov  [_gpu_sc_w], rax");
    E("    mov  eax, [rsi + 56]               ; WSTATE_HEIGHT");
    E("    mov  [_gpu_sc_h], rax");
    E(".cd_nosize:");

    // Frame-latency waitable object: swapchain->QueryInterface(IDXGISwapChain2),
    // SetMaximumFrameLatency(1) (slot 31/0xF8), GetFrameLatencyWaitableObject
    // (slot 33/0x108) -> HANDLE. present WaitForSingleObject's it each frame so
    // the CPU is paced one frame behind the GPU: no deep queue, no ~30ms Present
    // drain. Non-fatal if unsupported (present skips the wait).
    // IID_IDXGISwapChain2 = {a8be2ac4-199f-4946-b331-79599fb98de7}
    E("    mov  dword [rsp+0xC0], 0xa8be2ac4");
    E("    mov  dword [rsp+0xC4], 0x4946199f");
    E("    mov  dword [rsp+0xC8], 0x59793331");
    E("    mov  dword [rsp+0xCC], 0xe78db99f");
    E("    mov  rcx, [_gpu_swapchain]");
    E("    mov  rax, [rcx]");
    E("    lea  rdx, [rsp+0xC0]                ; riid");
    E("    lea  r8,  [rsp+0xD8]                ; &swapchain2");
    E("    call [rax + 0x00]                   ; QueryInterface(IDXGISwapChain2)");
    E("    test eax, eax");
    E("    jnz  .cd_nowait");
    E("    mov  rcx, [rsp+0xD8]");
    E("    mov  rax, [rcx]");
    E("    mov  edx, 1                         ; max frame latency = 1");
    E("    call [rax + 0xF8]                   ; SetMaximumFrameLatency");
    E("    mov  rcx, [rsp+0xD8]");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x108]                  ; GetFrameLatencyWaitableObject");
    E("    mov  [_gpu_waitable], rax           ; HANDLE");
    E("    mov  rcx, [rsp+0xD8]");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]                   ; Release IDXGISwapChain2");
    E(".cd_nowait:");

    // One-time integrated-vs-discrete probe on the live device (NOT per-frame).
    // ID3D11Device::CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2=14, &data,
    // sizeof=32) -- device vtable slot 33 (0x108). D3D11_FEATURE_DATA_D3D11_OPTIONS2
    // is 8 x 4-byte fields (32 bytes total, verified against d3d11.h); its 8th
    // field, UnifiedMemoryArchitecture at +28, is 1 on an integrated GPU and 0
    // on a discrete GPU. Read that DWORD, store the inverse: _gpu_discrete = !UMA.
    // Struct scratch lives at rsp+0xE0 (free region above backbuffer@0xD0).
    E("    xor  eax, eax");
    E("    mov  [rsp+0xE0], rax               ; zero 32-byte OPTIONS2 struct");
    E("    mov  [rsp+0xE8], rax");
    E("    mov  [rsp+0xF0], rax");
    E("    mov  [rsp+0xF8], rax");
    E("    mov  rcx, [_gpu_device]");
    E("    mov  rax, [rcx]");
    E("    mov  edx, 14                       ; D3D11_FEATURE_D3D11_OPTIONS2");
    E("    lea  r8,  [rsp+0xE0]               ; &feature data");
    E("    mov  r9d, 32                       ; sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS2)");
    E("    call [rax + 0x108]                 ; CheckFeatureSupport");
    // eax = HRESULT: on success read UMA@+28; on failure leave _gpu_discrete=0.
    E("    test eax, eax");
    E("    jnz  .cd_igpu                      ; query failed -> assume integrated");
    E("    mov  eax, [rsp+0xE0+28]            ; UnifiedMemoryArchitecture");
    E("    test eax, eax");
    E("    jnz  .cd_igpu                      ; UMA=1 -> integrated");
    E("    mov  qword [_gpu_discrete], 1      ; UMA=0 -> discrete");
    E("    jmp  .cd_pipe");
    E(".cd_igpu:");
    E("    mov  qword [_gpu_discrete], 0");
    E(".cd_pipe:");

    E("    call _slag_gpu_create_pipeline    ; VS/PS/layout/buffers/tex/sampler");
    E("    jmp  .cd_ret");

    E(".cd_fail_release_bb:");
    E("    mov  rcx, [rsp+0xD0]");
    E("    test rcx, rcx");
    E("    jz   .cd_fail");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]                  ; Release backbuffer");
    E(".cd_fail:");
    E("    mov  qword [_gpu_ready], 0");
    E(".cd_ret:");
    E("    add  rsp, 0x108");
    E("    pop  r12");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    ret");
}

void emit_gpu_data(Codegen *cg) {
    // Semantic-name C-strings for the input layout (ASCII, null-terminated).
    E("_gpu_sem_pos:   db 80,79,83,73,84,73,79,78,0   ; \"POSITION\"");
    E("_gpu_sem_tex:   db 84,69,88,67,79,79,82,68,0   ; \"TEXCOORD\"");
    E("_gpu_sem_col:   db 67,79,76,79,82,0            ; \"COLOR\"");
    E("_gpu_sem_nrm:   db 78,79,82,77,65,76,0         ; \"NORMAL\"");
    E("align 16");
    E("_gpu_vs_blob:  ; 3560 bytes DXBC (widened 64B vtx, NORMAL input, shadow-pass, camPos light)");
    E("    db 68,88,66,67,82,56,203,250,22,23,13,2,15,125,76,54");
    E("    db 173,72,65,242,1,0,0,0,232,13,0,0,5,0,0,0");
    E("    db 52,0,0,0,76,3,0,0,12,4,0,0,16,5,0,0");
    E("    db 76,13,0,0,82,68,69,70,16,3,0,0,1,0,0,0");
    E("    db 96,0,0,0,1,0,0,0,60,0,0,0,0,5,254,255");
    E("    db 8,1,0,0,229,2,0,0,82,68,49,49,60,0,0,0");
    E("    db 24,0,0,0,32,0,0,0,40,0,0,0,36,0,0,0");
    E("    db 12,0,0,0,0,0,0,0,92,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,67,0,171,171,92,0,0,0");
    E("    db 9,0,0,0,120,0,0,0,176,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,224,1,0,0,0,0,0,0,64,0,0,0");
    E("    db 2,0,0,0,244,1,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,24,2,0,0");
    E("    db 64,0,0,0,12,0,0,0,2,0,0,0,40,2,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,76,2,0,0,76,0,0,0,4,0,0,0");
    E("    db 2,0,0,0,92,2,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,128,2,0,0");
    E("    db 80,0,0,0,4,0,0,0,2,0,0,0,92,2,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,140,2,0,0,84,0,0,0,8,0,0,0");
    E("    db 2,0,0,0,160,2,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,196,2,0,0");
    E("    db 92,0,0,0,4,0,0,0,0,0,0,0,92,2,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,203,2,0,0,96,0,0,0,64,0,0,0");
    E("    db 2,0,0,0,244,1,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,211,2,0,0");
    E("    db 160,0,0,0,12,0,0,0,0,0,0,0,40,2,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,218,2,0,0,172,0,0,0,4,0,0,0");
    E("    db 2,0,0,0,92,2,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,118,105,101,119");
    E("    db 112,114,111,106,0,102,108,111,97,116,52,120,52,0,171,171");
    E("    db 2,0,3,0,4,0,4,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 233,1,0,0,102,111,103,67,111,108,111,114,0,102,108,111");
    E("    db 97,116,51,0,1,0,3,0,1,0,3,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,33,2,0,0,102,111,103,83,116,97,114,116");
    E("    db 0,102,108,111,97,116,0,171,0,0,3,0,1,0,1,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,85,2,0,0,102,111,103,73");
    E("    db 110,118,82,97,110,103,101,0,105,110,118,84,101,120,68,105");
    E("    db 109,115,0,102,108,111,97,116,50,0,171,171,1,0,3,0");
    E("    db 1,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,151,2,0,0");
    E("    db 115,117,110,65,110,103,0,108,105,103,104,116,86,80,0,99");
    E("    db 97,109,80,111,115,0,115,104,97,100,111,119,80,97,115,115");
    E("    db 0,77,105,99,114,111,115,111,102,116,32,40,82,41,32,72");
    E("    db 76,83,76,32,83,104,97,100,101,114,32,67,111,109,112,105");
    E("    db 108,101,114,32,49,48,46,49,0,171,171,171,73,83,71,78");
    E("    db 184,0,0,0,6,0,0,0,8,0,0,0,152,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0");
    E("    db 7,7,0,0,161,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 3,0,0,0,1,0,0,0,3,3,0,0,170,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0");
    E("    db 15,15,0,0,161,0,0,0,1,0,0,0,0,0,0,0");
    E("    db 3,0,0,0,3,0,0,0,1,1,0,0,161,0,0,0");
    E("    db 2,0,0,0,0,0,0,0,3,0,0,0,4,0,0,0");
    E("    db 1,1,0,0,176,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 3,0,0,0,5,0,0,0,7,7,0,0,80,79,83,73");
    E("    db 84,73,79,78,0,84,69,88,67,79,79,82,68,0,67,79");
    E("    db 76,79,82,0,78,79,82,77,65,76,0,171,79,83,71,78");
    E("    db 252,0,0,0,9,0,0,0,8,0,0,0,224,0,0,0");
    E("    db 0,0,0,0,1,0,0,0,3,0,0,0,0,0,0,0");
    E("    db 15,0,0,0,236,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 3,0,0,0,1,0,0,0,3,12,0,0,236,0,0,0");
    E("    db 1,0,0,0,0,0,0,0,3,0,0,0,1,0,0,0");
    E("    db 4,11,0,0,236,0,0,0,2,0,0,0,0,0,0,0");
    E("    db 3,0,0,0,1,0,0,0,8,7,0,0,245,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0");
    E("    db 15,0,0,0,236,0,0,0,3,0,0,0,0,0,0,0");
    E("    db 3,0,0,0,3,0,0,0,7,8,0,0,236,0,0,0");
    E("    db 5,0,0,0,0,0,0,0,3,0,0,0,3,0,0,0");
    E("    db 8,7,0,0,236,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 3,0,0,0,4,0,0,0,15,0,0,0,236,0,0,0");
    E("    db 6,0,0,0,0,0,0,0,3,0,0,0,5,0,0,0");
    E("    db 7,8,0,0,83,86,95,80,79,83,73,84,73,79,78,0");
    E("    db 84,69,88,67,79,79,82,68,0,67,79,76,79,82,0,171");
    E("    db 83,72,69,88,52,8,0,0,80,0,1,0,13,2,0,0");
    E("    db 106,8,0,1,89,0,0,4,70,142,32,0,0,0,0,0");
    E("    db 11,0,0,0,95,0,0,3,114,16,16,0,0,0,0,0");
    E("    db 95,0,0,3,50,16,16,0,1,0,0,0,95,0,0,3");
    E("    db 242,16,16,0,2,0,0,0,95,0,0,3,18,16,16,0");
    E("    db 3,0,0,0,95,0,0,3,18,16,16,0,4,0,0,0");
    E("    db 95,0,0,3,114,16,16,0,5,0,0,0,103,0,0,4");
    E("    db 242,32,16,0,0,0,0,0,1,0,0,0,101,0,0,3");
    E("    db 50,32,16,0,1,0,0,0,101,0,0,3,66,32,16,0");
    E("    db 1,0,0,0,101,0,0,3,130,32,16,0,1,0,0,0");
    E("    db 101,0,0,3,242,32,16,0,2,0,0,0,101,0,0,3");
    E("    db 114,32,16,0,3,0,0,0,101,0,0,3,130,32,16,0");
    E("    db 3,0,0,0,101,0,0,3,242,32,16,0,4,0,0,0");
    E("    db 101,0,0,3,114,32,16,0,5,0,0,0,104,0,0,2");
    E("    db 3,0,0,0,49,0,0,8,18,0,16,0,0,0,0,0");
    E("    db 1,64,0,0,0,0,0,63,58,128,32,0,0,0,0,0");
    E("    db 10,0,0,0,31,0,4,3,10,0,16,0,0,0,0,0");
    E("    db 49,0,0,7,18,0,16,0,0,0,0,0,1,64,0,0");
    E("    db 0,0,192,63,10,16,16,0,4,0,0,0,49,0,0,7");
    E("    db 34,0,16,0,0,0,0,0,10,16,16,0,4,0,0,0");
    E("    db 1,64,0,0,0,0,32,64,1,0,0,7,18,0,16,0");
    E("    db 0,0,0,0,26,0,16,0,0,0,0,0,10,0,16,0");
    E("    db 0,0,0,0,56,0,0,8,242,0,16,0,1,0,0,0");
    E("    db 86,21,16,0,0,0,0,0,70,142,32,0,0,0,0,0");
    E("    db 7,0,0,0,50,0,0,10,242,0,16,0,1,0,0,0");
    E("    db 6,16,16,0,0,0,0,0,70,142,32,0,0,0,0,0");
    E("    db 6,0,0,0,70,14,16,0,1,0,0,0,50,0,0,10");
    E("    db 242,0,16,0,1,0,0,0,166,26,16,0,0,0,0,0");
    E("    db 70,142,32,0,0,0,0,0,8,0,0,0,70,14,16,0");
    E("    db 1,0,0,0,0,0,0,8,242,0,16,0,1,0,0,0");
    E("    db 70,14,16,0,1,0,0,0,70,142,32,0,0,0,0,0");
    E("    db 9,0,0,0,55,0,0,12,242,32,16,0,0,0,0,0");
    E("    db 6,0,16,0,0,0,0,0,70,14,16,0,1,0,0,0");
    E("    db 2,64,0,0,0,0,0,64,0,0,0,64,0,0,0,64");
    E("    db 0,0,128,63,54,0,0,8,114,32,16,0,1,0,0,0");
    E("    db 2,64,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,54,0,0,5,130,32,16,0,1,0,0,0");
    E("    db 10,16,16,0,4,0,0,0,54,0,0,8,242,32,16,0");
    E("    db 2,0,0,0,2,64,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,54,0,0,5,114,32,16,0");
    E("    db 3,0,0,0,70,18,16,0,0,0,0,0,54,0,0,5");
    E("    db 130,32,16,0,3,0,0,0,1,64,0,0,0,0,0,0");
    E("    db 54,0,0,8,242,32,16,0,4,0,0,0,2,64,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,128,63");
    E("    db 54,0,0,8,114,32,16,0,5,0,0,0,2,64,0,0");
    E("    db 0,0,0,0,0,0,128,63,0,0,0,0,0,0,0,0");
    E("    db 62,0,0,1,18,0,0,1,54,0,0,5,114,0,16,0");
    E("    db 0,0,0,0,70,18,16,0,5,0,0,0,21,0,0,1");
    E("    db 49,0,0,7,130,0,16,0,0,0,0,0,10,16,16,0");
    E("    db 4,0,0,0,1,64,0,0,0,0,0,0,31,0,4,3");
    E("    db 58,0,16,0,0,0,0,0,56,0,0,8,242,0,16,0");
    E("    db 1,0,0,0,86,21,16,0,0,0,0,0,70,142,32,0");
    E("    db 0,0,0,0,1,0,0,0,50,0,0,10,242,0,16,0");
    E("    db 1,0,0,0,6,16,16,0,0,0,0,0,70,142,32,0");
    E("    db 0,0,0,0,0,0,0,0,70,14,16,0,1,0,0,0");
    E("    db 50,0,0,10,242,0,16,0,1,0,0,0,166,26,16,0");
    E("    db 0,0,0,0,70,142,32,0,0,0,0,0,2,0,0,0");
    E("    db 70,14,16,0,1,0,0,0,0,0,0,8,242,0,16,0");
    E("    db 1,0,0,0,70,14,16,0,1,0,0,0,70,142,32,0");
    E("    db 0,0,0,0,3,0,0,0,56,0,0,7,50,0,16,0");
    E("    db 2,0,0,0,70,16,16,0,1,0,0,0,6,16,16,0");
    E("    db 3,0,0,0,50,0,0,9,50,32,16,0,0,0,0,0");
    E("    db 70,0,16,0,2,0,0,0,246,15,16,0,1,0,0,0");
    E("    db 70,0,16,0,1,0,0,0,0,0,0,7,130,0,16,0");
    E("    db 0,0,0,0,58,0,16,0,1,0,0,0,1,64,0,0");
    E("    db 0,0,128,192,56,32,0,7,130,0,16,0,0,0,0,0");
    E("    db 58,0,16,0,0,0,0,0,1,64,0,0,0,0,128,62");
    E("    db 0,0,0,8,18,32,16,0,2,0,0,0,58,0,16,128");
    E("    db 65,0,0,0,0,0,0,0,1,64,0,0,0,0,128,63");
    E("    db 54,0,0,5,194,32,16,0,0,0,0,0,166,14,16,0");
    E("    db 1,0,0,0,54,0,0,8,114,32,16,0,1,0,0,0");
    E("    db 2,64,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,54,0,0,5,130,32,16,0,1,0,0,0");
    E("    db 10,16,16,0,4,0,0,0,54,0,0,8,162,32,16,0");
    E("    db 2,0,0,0,2,64,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,128,63,54,0,0,5,66,32,16,0");
    E("    db 2,0,0,0,58,0,16,0,0,0,0,0,54,0,0,5");
    E("    db 114,32,16,0,3,0,0,0,70,18,16,0,0,0,0,0");
    E("    db 54,0,0,5,130,32,16,0,3,0,0,0,1,64,0,0");
    E("    db 0,0,0,0,54,0,0,8,242,32,16,0,4,0,0,0");
    E("    db 2,64,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,128,63,54,0,0,8,114,32,16,0,5,0,0,0");
    E("    db 2,64,0,0,0,0,0,0,0,0,128,63,0,0,0,0");
    E("    db 0,0,0,0,62,0,0,1,18,0,0,1,54,0,0,5");
    E("    db 114,32,16,0,5,0,0,0,70,2,16,0,0,0,0,0");
    E("    db 21,0,0,1,56,0,0,8,242,0,16,0,0,0,0,0");
    E("    db 86,21,16,0,0,0,0,0,70,142,32,0,0,0,0,0");
    E("    db 1,0,0,0,50,0,0,10,242,0,16,0,0,0,0,0");
    E("    db 6,16,16,0,0,0,0,0,70,142,32,0,0,0,0,0");
    E("    db 0,0,0,0,70,14,16,0,0,0,0,0,50,0,0,10");
    E("    db 242,0,16,0,0,0,0,0,166,26,16,0,0,0,0,0");
    E("    db 70,142,32,0,0,0,0,0,2,0,0,0,70,14,16,0");
    E("    db 0,0,0,0,0,0,0,8,242,0,16,0,0,0,0,0");
    E("    db 70,14,16,0,0,0,0,0,70,142,32,0,0,0,0,0");
    E("    db 3,0,0,0,56,0,0,8,50,32,16,0,1,0,0,0");
    E("    db 70,16,16,0,1,0,0,0,150,133,32,0,0,0,0,0");
    E("    db 5,0,0,0,56,0,0,8,242,0,16,0,1,0,0,0");
    E("    db 86,21,16,0,0,0,0,0,70,142,32,0,0,0,0,0");
    E("    db 7,0,0,0,50,0,0,10,242,0,16,0,1,0,0,0");
    E("    db 6,16,16,0,0,0,0,0,70,142,32,0,0,0,0,0");
    E("    db 6,0,0,0,70,14,16,0,1,0,0,0,50,0,0,10");
    E("    db 242,0,16,0,1,0,0,0,166,26,16,0,0,0,0,0");
    E("    db 70,142,32,0,0,0,0,0,8,0,0,0,70,14,16,0");
    E("    db 1,0,0,0,0,0,0,8,242,32,16,0,4,0,0,0");
    E("    db 70,14,16,0,1,0,0,0,70,142,32,0,0,0,0,0");
    E("    db 9,0,0,0,49,0,0,7,18,0,16,0,1,0,0,0");
    E("    db 1,64,0,0,0,0,0,63,10,16,16,0,4,0,0,0");
    E("    db 55,0,0,9,18,0,16,0,1,0,0,0,10,0,16,0");
    E("    db 1,0,0,0,1,64,0,0,0,0,0,0,1,64,0,0");
    E("    db 0,0,128,63,0,0,0,9,34,0,16,0,1,0,0,0");
    E("    db 58,0,16,0,0,0,0,0,58,128,32,128,65,0,0,0");
    E("    db 0,0,0,0,4,0,0,0,56,32,0,8,34,0,16,0");
    E("    db 1,0,0,0,26,0,16,0,1,0,0,0,10,128,32,0");
    E("    db 0,0,0,0,5,0,0,0,56,0,0,7,18,0,16,0");
    E("    db 1,0,0,0,10,0,16,0,1,0,0,0,26,0,16,0");
    E("    db 1,0,0,0,56,0,0,10,226,0,16,0,1,0,0,0");
    E("    db 6,25,16,0,2,0,0,0,2,64,0,0,0,0,0,0");
    E("    db 129,128,128,59,129,128,128,59,129,128,128,59,50,0,0,14");
    E("    db 114,0,16,0,2,0,0,0,70,18,16,128,65,0,0,0");
    E("    db 2,0,0,0,2,64,0,0,129,128,128,59,129,128,128,59");
    E("    db 129,128,128,59,0,0,0,0,70,130,32,0,0,0,0,0");
    E("    db 4,0,0,0,50,0,0,9,114,32,16,0,2,0,0,0");
    E("    db 6,0,16,0,1,0,0,0,70,2,16,0,2,0,0,0");
    E("    db 150,7,16,0,1,0,0,0,56,0,0,7,130,32,16,0");
    E("    db 2,0,0,0,58,16,16,0,2,0,0,0,1,64,0,0");
    E("    db 129,128,128,59,54,0,0,5,242,32,16,0,0,0,0,0");
    E("    db 70,14,16,0,0,0,0,0,54,0,0,5,66,32,16,0");
    E("    db 1,0,0,0,10,16,16,0,3,0,0,0,54,0,0,5");
    E("    db 130,32,16,0,1,0,0,0,10,16,16,0,4,0,0,0");
    E("    db 54,0,0,5,114,32,16,0,3,0,0,0,70,18,16,0");
    E("    db 0,0,0,0,54,0,0,5,130,32,16,0,3,0,0,0");
    E("    db 58,0,16,0,0,0,0,0,62,0,0,1,83,84,65,84");
    E("    db 148,0,0,0,69,0,0,0,3,0,0,0,0,0,0,0");
    E("    db 15,0,0,0,34,0,0,0,0,0,0,0,1,0,0,0");
    E("    db 5,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 23,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0");
    E("_gpu_vs_blob_len equ 3560");
    E("align 16");
    E("_gpu_ps_blob:  ; 2560 bytes DXBC (point-light diffuse (working illuminated scene, shadow term=1))");
    E("    db 68,88,66,67,221,128,24,114,208,144,104,219,68,204,166,216");
    E("    db 159,151,136,206,1,0,0,0,0,10,0,0,5,0,0,0");
    E("    db 52,0,0,0,188,3,0,0,192,4,0,0,244,4,0,0");
    E("    db 100,9,0,0,82,68,69,70,128,3,0,0,1,0,0,0");
    E("    db 208,0,0,0,4,0,0,0,60,0,0,0,0,5,255,255");
    E("    db 0,1,0,0,85,3,0,0,82,68,49,49,60,0,0,0");
    E("    db 24,0,0,0,32,0,0,0,40,0,0,0,36,0,0,0");
    E("    db 12,0,0,0,0,0,0,0,188,0,0,0,3,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,192,0,0,0,3,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,199,0,0,0,2,0,0,0");
    E("    db 5,0,0,0,5,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 1,0,0,0,13,0,0,0,203,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,115,109,112,0,115,109,112,76");
    E("    db 105,110,0,116,101,120,0,67,0,171,171,171,203,0,0,0");
    E("    db 9,0,0,0,232,0,0,0,176,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,80,2,0,0,0,0,0,0,64,0,0,0");
    E("    db 0,0,0,0,100,2,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,136,2,0,0");
    E("    db 64,0,0,0,12,0,0,0,0,0,0,0,152,2,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,188,2,0,0,76,0,0,0,4,0,0,0");
    E("    db 0,0,0,0,204,2,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,240,2,0,0");
    E("    db 80,0,0,0,4,0,0,0,0,0,0,0,204,2,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,252,2,0,0,84,0,0,0,8,0,0,0");
    E("    db 0,0,0,0,16,3,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,52,3,0,0");
    E("    db 92,0,0,0,4,0,0,0,0,0,0,0,204,2,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,59,3,0,0,96,0,0,0,64,0,0,0");
    E("    db 0,0,0,0,100,2,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,67,3,0,0");
    E("    db 160,0,0,0,12,0,0,0,2,0,0,0,152,2,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,74,3,0,0,172,0,0,0,4,0,0,0");
    E("    db 0,0,0,0,204,2,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,118,105,101,119");
    E("    db 112,114,111,106,0,102,108,111,97,116,52,120,52,0,171,171");
    E("    db 2,0,3,0,4,0,4,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 89,2,0,0,102,111,103,67,111,108,111,114,0,102,108,111");
    E("    db 97,116,51,0,1,0,3,0,1,0,3,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,145,2,0,0,102,111,103,83,116,97,114,116");
    E("    db 0,102,108,111,97,116,0,171,0,0,3,0,1,0,1,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,197,2,0,0,102,111,103,73");
    E("    db 110,118,82,97,110,103,101,0,105,110,118,84,101,120,68,105");
    E("    db 109,115,0,102,108,111,97,116,50,0,171,171,1,0,3,0");
    E("    db 1,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,7,3,0,0");
    E("    db 115,117,110,65,110,103,0,108,105,103,104,116,86,80,0,99");
    E("    db 97,109,80,111,115,0,115,104,97,100,111,119,80,97,115,115");
    E("    db 0,77,105,99,114,111,115,111,102,116,32,40,82,41,32,72");
    E("    db 76,83,76,32,83,104,97,100,101,114,32,67,111,109,112,105");
    E("    db 108,101,114,32,49,48,46,49,0,171,171,171,73,83,71,78");
    E("    db 252,0,0,0,9,0,0,0,8,0,0,0,224,0,0,0");
    E("    db 0,0,0,0,1,0,0,0,3,0,0,0,0,0,0,0");
    E("    db 15,0,0,0,236,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 3,0,0,0,1,0,0,0,3,3,0,0,236,0,0,0");
    E("    db 1,0,0,0,0,0,0,0,3,0,0,0,1,0,0,0");
    E("    db 4,4,0,0,236,0,0,0,2,0,0,0,0,0,0,0");
    E("    db 3,0,0,0,1,0,0,0,8,8,0,0,245,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0");
    E("    db 15,15,0,0,236,0,0,0,3,0,0,0,0,0,0,0");
    E("    db 3,0,0,0,3,0,0,0,7,7,0,0,236,0,0,0");
    E("    db 5,0,0,0,0,0,0,0,3,0,0,0,3,0,0,0");
    E("    db 8,0,0,0,236,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 3,0,0,0,4,0,0,0,15,0,0,0,236,0,0,0");
    E("    db 6,0,0,0,0,0,0,0,3,0,0,0,5,0,0,0");
    E("    db 7,7,0,0,83,86,95,80,79,83,73,84,73,79,78,0");
    E("    db 84,69,88,67,79,79,82,68,0,67,79,76,79,82,0,171");
    E("    db 79,83,71,78,44,0,0,0,1,0,0,0,8,0,0,0");
    E("    db 32,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 0,0,0,0,15,0,0,0,83,86,95,84,65,82,71,69");
    E("    db 84,0,171,171,83,72,69,88,104,4,0,0,80,0,0,0");
    E("    db 26,1,0,0,106,8,0,1,89,0,0,4,70,142,32,0");
    E("    db 0,0,0,0,11,0,0,0,90,0,0,3,0,96,16,0");
    E("    db 0,0,0,0,90,0,0,3,0,96,16,0,1,0,0,0");
    E("    db 88,64,0,4,0,112,16,0,0,0,0,0,85,85,0,0");
    E("    db 98,16,0,3,50,16,16,0,1,0,0,0,98,16,0,3");
    E("    db 66,16,16,0,1,0,0,0,98,16,0,3,130,16,16,0");
    E("    db 1,0,0,0,98,16,0,3,242,16,16,0,2,0,0,0");
    E("    db 98,16,0,3,114,16,16,0,3,0,0,0,98,16,0,3");
    E("    db 114,16,16,0,5,0,0,0,101,0,0,3,242,32,16,0");
    E("    db 0,0,0,0,104,0,0,2,3,0,0,0,49,0,0,7");
    E("    db 18,0,16,0,0,0,0,0,42,16,16,0,1,0,0,0");
    E("    db 1,64,0,0,0,0,0,0,31,0,4,3,10,0,16,0");
    E("    db 0,0,0,0,56,0,0,10,114,0,16,0,0,0,0,0");
    E("    db 70,18,16,0,1,0,0,0,2,64,0,0,0,0,128,63");
    E("    db 0,0,128,63,0,0,128,191,0,0,0,0,69,0,0,139");
    E("    db 2,2,0,128,67,85,21,0,18,0,16,0,0,0,0,0");
    E("    db 70,2,16,0,0,0,0,0,70,126,16,0,0,0,0,0");
    E("    db 0,96,16,0,1,0,0,0,0,0,0,7,18,0,16,0");
    E("    db 0,0,0,0,10,0,16,0,0,0,0,0,1,64,0,0");
    E("    db 31,133,235,190,56,32,0,7,18,0,16,0,0,0,0,0");
    E("    db 10,0,16,0,0,0,0,0,1,64,0,0,254,255,71,65");
    E("    db 50,0,0,9,34,0,16,0,0,0,0,0,10,0,16,0");
    E("    db 0,0,0,0,1,64,0,0,0,0,0,192,1,64,0,0");
    E("    db 0,0,64,64,56,0,0,7,18,0,16,0,0,0,0,0");
    E("    db 10,0,16,0,0,0,0,0,10,0,16,0,0,0,0,0");
    E("    db 56,0,0,7,18,0,16,0,0,0,0,0,10,0,16,0");
    E("    db 0,0,0,0,26,0,16,0,0,0,0,0,56,0,0,7");
    E("    db 130,32,16,0,0,0,0,0,10,0,16,0,0,0,0,0");
    E("    db 58,16,16,0,2,0,0,0,54,0,0,5,114,32,16,0");
    E("    db 0,0,0,0,70,18,16,0,2,0,0,0,62,0,0,1");
    E("    db 21,0,0,1,69,0,0,139,2,2,0,128,67,85,21,0");
    E("    db 114,0,16,0,0,0,0,0,70,18,16,0,1,0,0,0");
    E("    db 70,126,16,0,0,0,0,0,0,96,16,0,0,0,0,0");
    E("    db 56,0,0,7,114,0,16,0,0,0,0,0,70,2,16,0");
    E("    db 0,0,0,0,70,18,16,0,2,0,0,0,49,0,0,7");
    E("    db 130,0,16,0,0,0,0,0,1,64,0,0,0,0,192,63");
    E("    db 58,16,16,0,1,0,0,0,49,0,0,7,18,0,16,0");
    E("    db 1,0,0,0,58,16,16,0,1,0,0,0,1,64,0,0");
    E("    db 0,0,32,64,1,0,0,7,130,0,16,0,0,0,0,0");
    E("    db 58,0,16,0,0,0,0,0,10,0,16,0,1,0,0,0");
    E("    db 31,0,4,3,58,0,16,0,0,0,0,0,16,0,0,7");
    E("    db 130,0,16,0,0,0,0,0,70,18,16,0,5,0,0,0");
    E("    db 70,18,16,0,5,0,0,0,68,0,0,5,130,0,16,0");
    E("    db 0,0,0,0,58,0,16,0,0,0,0,0,56,0,0,7");
    E("    db 114,0,16,0,1,0,0,0,246,15,16,0,0,0,0,0");
    E("    db 70,18,16,0,5,0,0,0,0,0,0,9,114,0,16,0");
    E("    db 2,0,0,0,70,18,16,128,65,0,0,0,3,0,0,0");
    E("    db 70,130,32,0,0,0,0,0,10,0,0,0,16,0,0,7");
    E("    db 130,0,16,0,0,0,0,0,70,2,16,0,2,0,0,0");
    E("    db 70,2,16,0,2,0,0,0,68,0,0,5,130,0,16,0");
    E("    db 0,0,0,0,58,0,16,0,0,0,0,0,56,0,0,7");
    E("    db 114,0,16,0,2,0,0,0,246,15,16,0,0,0,0,0");
    E("    db 70,2,16,0,2,0,0,0,16,0,0,7,130,0,16,0");
    E("    db 0,0,0,0,70,2,16,0,1,0,0,0,70,2,16,0");
    E("    db 2,0,0,0,52,0,0,7,130,0,16,0,0,0,0,0");
    E("    db 58,0,16,0,0,0,0,0,1,64,0,0,0,0,0,0");
    E("    db 50,0,0,9,130,0,16,0,0,0,0,0,58,0,16,0");
    E("    db 0,0,0,0,1,64,0,0,0,0,67,67,1,64,0,0");
    E("    db 0,0,112,66,51,0,0,7,130,0,16,0,0,0,0,0");
    E("    db 58,0,16,0,0,0,0,0,1,64,0,0,0,0,128,67");
    E("    db 0,0,0,7,130,0,16,0,0,0,0,0,58,0,16,0");
    E("    db 0,0,0,0,1,64,0,0,0,0,112,194,50,0,0,9");
    E("    db 130,0,16,0,0,0,0,0,58,0,16,0,0,0,0,0");
    E("    db 1,64,0,0,0,0,128,59,1,64,0,0,0,0,112,62");
    E("    db 56,0,0,7,114,32,16,0,0,0,0,0,246,15,16,0");
    E("    db 0,0,0,0,70,2,16,0,0,0,0,0,54,0,0,5");
    E("    db 130,32,16,0,0,0,0,0,58,16,16,0,2,0,0,0");
    E("    db 62,0,0,1,21,0,0,1,54,0,0,5,130,32,16,0");
    E("    db 0,0,0,0,58,16,16,0,2,0,0,0,54,0,0,5");
    E("    db 114,32,16,0,0,0,0,0,70,2,16,0,0,0,0,0");
    E("    db 62,0,0,1,83,84,65,84,148,0,0,0,39,0,0,0");
    E("    db 3,0,0,0,0,0,0,0,7,0,0,0,25,0,0,0");
    E("    db 0,0,0,0,1,0,0,0,3,0,0,0,2,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("_gpu_ps_blob_len equ 2560");
}

// _slag_gpu_create_pipeline: create all pcolor pipeline objects (VS, PS, input
// layout, dynamic vertex buffer, constant buffer, 512x512 BGRA texture + SRV,
// point sampler). Sets _gpu_pipeline=1 on full success. Device vtable slots:
// CreateVertexShader 0x60, CreatePixelShader 0x78, CreateInputLayout 0x58,
// CreateBuffer 0x18, CreateTexture2D 0x28, CreateShaderResourceView 0x38,
// CreateSamplerState 0xb8.
static void emit_gpu_create_pipeline(Codegen *cg) {
    E("; --- D3D11 resource desc offsets/enums (verified against d3d11.h) ---");
    E("BUFDESC_BYTEWIDTH  equ 0");
    E("BUFDESC_USAGE      equ 4");
    E("BUFDESC_BIND       equ 8");
    E("BUFDESC_CPUACCESS  equ 12");
    E("USAGE_DEFAULT      equ 0");
    E("USAGE_DYNAMIC      equ 2");
    E("BIND_VERTEX        equ 1");
    E("BIND_CONSTANT      equ 4");
    E("BIND_SRV           equ 8");
    E("D3DCPU_WRITE          equ 0x10000");
    E("FMT_RGBA32F        equ 2");
    E("FMT_RGB32F         equ 6");
    E("FMT_RG32F          equ 16");
    E("FMT_R32F           equ 41");
    E("FMT_BGRA8          equ 87");
    E("GPU_TEX_SLICES     equ 256");       // Texture2DArray slice count (512x512 BGRA each)
    E("FILTER_POINT       equ 0");
    E("FILTER_LINEAR      equ 21");       // D3D11_FILTER_MIN_MAG_MIP_LINEAR (0x15)
    E("ADDR_CLAMP         equ 3");
    E("");
    E("; --- _slag_gpu_create_pipeline ---");
    E("_slag_gpu_create_pipeline:");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    push r12");
    E("    sub  rsp, 0x228");

    E("    ; set initial capacity before the vbuf CreateBuffer sizes off it");
    E("    cmp  qword [_gpu_cap], 0");
    E("    jne  .pl_cap_ok");
    E("    mov  qword [_gpu_cap], GPU_STAGE_CAP");
    E(".pl_cap_ok:");
    E("    mov  rbx, [_gpu_device]");
    E("    test rbx, rbx");
    E("    jz   .pl_fail");

    // CreateVertexShader(blob, len, NULL, &_gpu_vs) -- slot 0x60, 5 args
    E("    lea  rax, [_gpu_vs]");
    E("    mov  [rsp+0x20], rax");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [_gpu_vs_blob]");
    E("    mov  r8,  _gpu_vs_blob_len");
    E("    xor  r9,  r9");
    E("    call [rax + 0x60]");
    E("    test eax, eax");
    E("    jnz  .pl_fail");

    // CreatePixelShader(blob, len, NULL, &_gpu_ps) -- slot 0x78, 5 args
    E("    lea  rax, [_gpu_ps]");
    E("    mov  qword [rsp+0x20], rax");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [_gpu_ps_blob]");
    E("    mov  r8,  _gpu_ps_blob_len");
    E("    xor  r9,  r9");
    E("    call [rax + 0x78]");
    E("    test eax, eax");
    E("    jnz  .pl_fail");

    // Build 4 D3D11_INPUT_ELEMENT_DESC at [rsp+0x80] (32 bytes each).
    E("    lea  rax, [_gpu_sem_pos]");
    E("    mov  [rsp+0x80+0], rax");
    E("    mov  dword [rsp+0x80+8], 0");
    E("    mov  dword [rsp+0x80+12], FMT_RGB32F");
    E("    mov  dword [rsp+0x80+16], 0");
    E("    mov  dword [rsp+0x80+20], 0");
    E("    mov  dword [rsp+0x80+24], 0");
    E("    mov  dword [rsp+0x80+28], 0");
    E("    lea  rax, [_gpu_sem_tex]");
    E("    mov  [rsp+0xA0+0], rax");
    E("    mov  dword [rsp+0xA0+8], 0");
    E("    mov  dword [rsp+0xA0+12], FMT_RG32F");
    E("    mov  dword [rsp+0xA0+16], 0");
    E("    mov  dword [rsp+0xA0+20], 12");
    E("    mov  dword [rsp+0xA0+24], 0");
    E("    mov  dword [rsp+0xA0+28], 0");
    E("    lea  rax, [_gpu_sem_col]");
    E("    mov  [rsp+0xC0+0], rax");
    E("    mov  dword [rsp+0xC0+8], 0");
    E("    mov  dword [rsp+0xC0+12], FMT_RGBA32F");
    E("    mov  dword [rsp+0xC0+16], 0");
    E("    mov  dword [rsp+0xC0+20], 20");
    E("    mov  dword [rsp+0xC0+24], 0");
    E("    mov  dword [rsp+0xC0+28], 0");
    E("    lea  rax, [_gpu_sem_tex]");
    E("    mov  [rsp+0xE0+0], rax");
    E("    mov  dword [rsp+0xE0+8], 1        ; TEXCOORD1 = slice");
    E("    mov  dword [rsp+0xE0+12], FMT_R32F");
    E("    mov  dword [rsp+0xE0+16], 0");
    E("    mov  dword [rsp+0xE0+20], 36       ; offset: after 9 f32 (pos3 uv2 col4)");
    E("    mov  dword [rsp+0xE0+24], 0");
    E("    mov  dword [rsp+0xE0+28], 0");
    // 5th element: flag = TEXCOORD2, R32F, offset 40 (11th f32: pos3 uv2 col4 slice).
    E("    lea  rax, [_gpu_sem_tex]");
    E("    mov  [rsp+0x100+0], rax");
    E("    mov  dword [rsp+0x100+8], 2        ; TEXCOORD2 = flag");
    E("    mov  dword [rsp+0x100+12], FMT_R32F");
    E("    mov  dword [rsp+0x100+16], 0");
    E("    mov  dword [rsp+0x100+20], 40      ; offset: after 10 f32 (pos3 uv2 col4 slice)");
    E("    mov  dword [rsp+0x100+24], 0");
    E("    mov  dword [rsp+0x100+28], 0");
    // 6th element: NORMAL = nrm.xyz, RGB32F, offset 44 (12th f32: after flag).
    // Widened 64B vertex: pos3 uv2 col4 slice flag nrm3 pad2 (16 f32).
    E("    lea  rax, [_gpu_sem_nrm]");
    E("    mov  [rsp+0x120+0], rax");
    E("    mov  dword [rsp+0x120+8], 0        ; NORMAL semantic index 0");
    E("    mov  dword [rsp+0x120+12], FMT_RGB32F");
    E("    mov  dword [rsp+0x120+16], 0");
    E("    mov  dword [rsp+0x120+20], 44      ; offset: after 11 f32 (pos3 uv2 col4 slice flag)");
    E("    mov  dword [rsp+0x120+24], 0");
    E("    mov  dword [rsp+0x120+28], 0");

    // CreateInputLayout(descs, 6, vs_blob, vs_len, &_gpu_layout) -- 0x58, 6 args
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x80]");
    E("    mov  r8,  6");
    E("    lea  r9,  [_gpu_vs_blob]");
    E("    mov  qword [rsp+0x20], _gpu_vs_blob_len");
    E("    lea  rax, [_gpu_layout]");
    E("    mov  [rsp+0x28], rax");
    E("    mov  rax, [rbx]");
    E("    call [rax + 0x58]");
    E("    test eax, eax");
    E("    jnz  .pl_fail");

    // Dynamic vertex buffer: sized to the current capacity (_gpu_cap tris * 3
    // verts * GPU_VTX_STRIDE), computed at runtime so it grows with _gpu_cap.
    E("    mov  rax, [_gpu_cap]");
    E("    imul rax, 3 * GPU_VTX_STRIDE");
    E("    mov  dword [rsp+0x100+BUFDESC_BYTEWIDTH], eax");
    E("    mov  dword [rsp+0x100+BUFDESC_USAGE], USAGE_DYNAMIC");
    E("    mov  dword [rsp+0x100+BUFDESC_BIND], BIND_VERTEX");
    E("    mov  dword [rsp+0x100+BUFDESC_CPUACCESS], D3DCPU_WRITE");
    E("    mov  dword [rsp+0x100+16], 0");
    E("    mov  dword [rsp+0x100+20], 0");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x100]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_vbuf]");
    E("    call [rax + 0x18]");
    E("    test eax, eax");
    E("    jnz  .pl_fail");

    // Second dynamic vertex buffer (same desc) for double-buffering: the CPU
    // writes one while the GPU still reads the other, so Map(WRITE_DISCARD)
    // never blocks on the prior frame's Draw. Desc at rsp+0x100 is unchanged.
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x100]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_vbuf2]");
    E("    call [rax + 0x18]");
    E("    test eax, eax");
    E("    jnz  .pl_fail");

    // Constant buffer: 176 bytes, dynamic. Layout: viewproj(64) + fog tail(32,
    // incl invTexDims) + lightVP(64 at off 96) + lightDir.xyz+shadowPass(16 at
    // off 160). 176 is a 16-byte multiple as D3D11 requires.
    E("    mov  dword [rsp+0x100+BUFDESC_BYTEWIDTH], 176");
    E("    mov  dword [rsp+0x100+BUFDESC_USAGE], USAGE_DYNAMIC");
    E("    mov  dword [rsp+0x100+BUFDESC_BIND], BIND_CONSTANT");
    E("    mov  dword [rsp+0x100+BUFDESC_CPUACCESS], D3DCPU_WRITE");
    E("    mov  dword [rsp+0x100+16], 0");
    E("    mov  dword [rsp+0x100+20], 0");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x100]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_cbuf]");
    E("    call [rax + 0x18]");
    E("    test eax, eax");
    E("    jnz  .pl_fail");

    // 512x512 BGRA Texture2DArray (GPU_TEX_SLICES slices): TEXTURE2D_DESC at [rsp+0x120] (44 bytes).
    E("    mov  dword [rsp+0x120+0], 512");
    E("    mov  dword [rsp+0x120+4], 512");
    E("    mov  dword [rsp+0x120+8], 1");
    E("    mov  dword [rsp+0x120+12], GPU_TEX_SLICES");
    E("    mov  dword [rsp+0x120+16], FMT_BGRA8");
    E("    mov  dword [rsp+0x120+20], 1");
    E("    mov  dword [rsp+0x120+24], 0");
    E("    mov  dword [rsp+0x120+28], USAGE_DEFAULT");
    E("    mov  dword [rsp+0x120+32], BIND_SRV");
    E("    mov  dword [rsp+0x120+36], 0");
    E("    mov  dword [rsp+0x120+40], 0");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x120]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_tex]");
    E("    call [rax + 0x28]");
    E("    test eax, eax");
    E("    jnz  .pl_fail");

    // CreateShaderResourceView(tex, NULL, &_gpu_srv) -- slot 0x38, 4 args.
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    mov  rdx, [_gpu_tex]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_srv]");
    E("    call [rax + 0x38]");
    E("    test eax, eax");
    E("    jnz  .pl_fail");

    // Sampler: SAMPLER_DESC at [rsp+0x150] (52 bytes), point + clamp.
    E("    lea  rdi, [rsp+0x150]");
    E("    xor  eax, eax");
    E("    mov  ecx, 13");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x150+0], FILTER_POINT");
    E("    mov  dword [rsp+0x150+4], ADDR_CLAMP");
    E("    mov  dword [rsp+0x150+8], ADDR_CLAMP");
    E("    mov  dword [rsp+0x150+12], ADDR_CLAMP");
    E("    mov  dword [rsp+0x150+48], 0x7f7fffff");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x150]");
    E("    lea  r8,  [_gpu_sampler]");
    E("    call [rax + 0xb8]");
    E("    test eax, eax");
    E("    jnz  .pl_fail");

    // Second sampler: linear + clamp (s1), for SDF text distance interpolation.
    // Reuses rsp+0x150 scratch -- the point sampler above already consumed its
    // desc, and the rasterizer below re-zeros this region before its own use.
    E("    lea  rdi, [rsp+0x150]");
    E("    xor  eax, eax");
    E("    mov  ecx, 13");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x150+0], FILTER_LINEAR");
    E("    mov  dword [rsp+0x150+4], ADDR_CLAMP");
    E("    mov  dword [rsp+0x150+8], ADDR_CLAMP");
    E("    mov  dword [rsp+0x150+12], ADDR_CLAMP");
    E("    mov  dword [rsp+0x150+48], 0x7f7fffff");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x150]");
    E("    lea  r8,  [_gpu_sampler_lin]");
    E("    call [rax + 0xb8]");
    E("    test eax, eax");
    E("    jnz  .pl_fail");

    // Third sampler: COMPARISON (s2), for the shadow-map hardware PCF. HLSL uses
    // SamplerComparisonState + SampleCmpLevelZero, which REQUIRES a comparison
    // sampler (a normal sampler returns garbage). Filter =
    // D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT (0x80), ComparisonFunc@24 =
    // D3D11_COMPARISON_LESS_EQUAL (4), CLAMP addressing.
    E("    lea  rdi, [rsp+0x150]");
    E("    xor  eax, eax");
    E("    mov  ecx, 13");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x150+0], 0x80          ; COMPARISON_MIN_MAG_MIP_POINT");
    E("    mov  dword [rsp+0x150+4], ADDR_CLAMP");
    E("    mov  dword [rsp+0x150+8], ADDR_CLAMP");
    E("    mov  dword [rsp+0x150+12], ADDR_CLAMP");
    E("    mov  dword [rsp+0x150+24], 4            ; ComparisonFunc = LESS_EQUAL");
    E("    mov  dword [rsp+0x150+48], 0x7f7fffff   ; MaxLOD = FLT_MAX");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x150]");
    E("    lea  r8,  [_gpu_sampler_cmp]");
    E("    call [rax + 0xb8]");
    E("    test eax, eax");
    E("    jnz  .pl_fail");

    // Rasterizer state: solid fill, back-face cull, standard D3D winding.
    // FrontCounterClockwise=FALSE -> front-facing is CLOCKWISE in clip space (the
    // D3D default agents are trained on). With the script's Y-flip viewproj this
    // keeps triangles whose screen (y-down) signed area is >0.
    // RASTERIZER_DESC(40) at [rsp+0x150]: FillMode@0=SOLID(3), CullMode@4=BACK(3),
    // FrontCCW@8=0, DepthClipEnable@24=1, rest 0.
    E("    lea  rdi, [rsp+0x150]");
    E("    xor  eax, eax");
    E("    mov  ecx, 10");                   // 40 bytes / 4
    E("    rep  stosd");
    E("    mov  dword [rsp+0x150+0], 3");    // FILL_SOLID
    E("    mov  dword [rsp+0x150+4], 3");    // CULL_BACK
    E("    mov  dword [rsp+0x150+8], 0");    // FrontCounterClockwise = FALSE (D3D default)
    E("    mov  dword [rsp+0x150+24], 1");   // DepthClipEnable
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x150]");
    E("    lea  r8,  [_gpu_raster]");
    E("    call [rax + 0xb0]                 ; CreateRasterizerState");
    E("    test eax, eax");
    E("    jnz  .pl_fail");

    // --- Depth buffer: D32_FLOAT texture + DSV + depth-stencil state (LESS) ---
    // TEXTURE2D_DESC (44 bytes) at rsp+0x100, sized to the window client area.
    E("    mov  rsi, [_window_primary_state]");
    E("    mov  eax, [rsi + 48]              ; WSTATE_WIDTH");
    E("    mov  [rsp+0x100+0], eax");
    E("    mov  eax, [rsi + 56]              ; WSTATE_HEIGHT");
    E("    mov  [rsp+0x100+4], eax");
    E("    mov  dword [rsp+0x100+8], 1        ; MipLevels");
    E("    mov  dword [rsp+0x100+12], 1       ; ArraySize");
    E("    mov  dword [rsp+0x100+16], 0x28    ; DXGI_FORMAT_D32_FLOAT");
    E("    mov  dword [rsp+0x100+20], 1       ; SampleDesc.Count");
    E("    mov  dword [rsp+0x100+24], 0       ; SampleDesc.Quality");
    E("    mov  dword [rsp+0x100+28], 0       ; USAGE_DEFAULT");
    E("    mov  dword [rsp+0x100+32], 0x40    ; BIND_DEPTH_STENCIL");
    E("    mov  dword [rsp+0x100+36], 0");
    E("    mov  dword [rsp+0x100+40], 0");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x100]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_depthtex]");
    E("    call [rax + 0x28]                  ; CreateTexture2D");
    E("    test eax, eax");
    E("    jnz  .pl_fail");
    // CreateDepthStencilView(depthtex, NULL, &_gpu_dsv) -- slot 10 (0x50)
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    mov  rdx, [_gpu_depthtex]");
    E("    xor  r8,  r8                       ; default DSV desc (D32, TEX2D)");
    E("    lea  r9,  [_gpu_dsv]");
    E("    call [rax + 0x50]                  ; CreateDepthStencilView");
    E("    test eax, eax");
    E("    jnz  .pl_fail");
    // D3D11_DEPTH_STENCIL_DESC (52 bytes) at rsp+0x100: DepthEnable=1,
    // WriteMask=ALL(1), Func=LESS(2), Stencil disabled.
    E("    lea  rdi, [rsp+0x100]");
    E("    xor  eax, eax");
    E("    mov  ecx, 13");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x100+0], 1        ; DepthEnable");
    E("    mov  dword [rsp+0x100+4], 1        ; DepthWriteMask = ALL");
    E("    mov  dword [rsp+0x100+8], 2        ; DepthFunc = LESS");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x100]");
    E("    lea  r8,  [_gpu_dsstate]");
    E("    call [rax + 0xA8]                  ; CreateDepthStencilState");
    E("    test eax, eax");
    E("    jnz  .pl_fail");

    // --- Shadow map: 2048x2048 R32_TYPELESS depth texture, viewable as both a
    // depth target (shadow pass) and a shader resource (main pass). ---
    // TEXTURE2D_DESC (44 bytes) at rsp+0x100.
    E("    lea  rdi, [rsp+0x100]");
    E("    xor  eax, eax");
    E("    mov  ecx, 11");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x100+0], 2048     ; Width");
    E("    mov  dword [rsp+0x100+4], 2048     ; Height");
    E("    mov  dword [rsp+0x100+8], 1        ; MipLevels");
    E("    mov  dword [rsp+0x100+12], 1       ; ArraySize");
    E("    mov  dword [rsp+0x100+16], 0x27    ; DXGI_FORMAT_R32_TYPELESS");
    E("    mov  dword [rsp+0x100+20], 1       ; SampleDesc.Count");
    E("    mov  dword [rsp+0x100+24], 0       ; SampleDesc.Quality");
    E("    mov  dword [rsp+0x100+28], 0       ; USAGE_DEFAULT");
    E("    mov  dword [rsp+0x100+32], 0x48    ; BIND_DEPTH_STENCIL(0x40)|BIND_SHADER_RESOURCE(0x08)");
    E("    mov  dword [rsp+0x100+36], 0");
    E("    mov  dword [rsp+0x100+40], 0");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x100]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_shadowtex]");
    E("    call [rax + 0x28]                  ; CreateTexture2D");
    E("    test eax, eax");
    E("    jnz  .pl_fail");
    // DEPTH_STENCIL_VIEW_DESC (D3D11): Format(0)=D32_FLOAT(0x28), ViewDim(4)=
    // TEXTURE2D(3), Flags(8)=0, Texture2D.MipSlice(12)=0. 20+ bytes; zero a block.
    E("    lea  rdi, [rsp+0x100]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x100+0], 0x28     ; DXGI_FORMAT_D32_FLOAT");
    E("    mov  dword [rsp+0x100+4], 3        ; D3D11_DSV_DIMENSION_TEXTURE2D");
    E("    mov  dword [rsp+0x100+8], 0        ; Flags");
    E("    mov  dword [rsp+0x100+12], 0       ; Texture2D.MipSlice");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    mov  rdx, [_gpu_shadowtex]");
    E("    lea  r8,  [rsp+0x100]");
    E("    lea  r9,  [_gpu_shadow_dsv]");
    E("    call [rax + 0x50]                  ; CreateDepthStencilView");
    E("    test eax, eax");
    E("    jnz  .pl_fail");
    // SHADER_RESOURCE_VIEW_DESC: Format(0)=R32_FLOAT(0x29), ViewDim(4)=
    // TEXTURE2D(4), Texture2D.MostDetailedMip(8)=0, MipLevels(12)=1.
    E("    lea  rdi, [rsp+0x100]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x100+0], 0x29     ; DXGI_FORMAT_R32_FLOAT");
    E("    mov  dword [rsp+0x100+4], 4        ; D3D11_SRV_DIMENSION_TEXTURE2D");
    E("    mov  dword [rsp+0x100+8], 0        ; MostDetailedMip");
    E("    mov  dword [rsp+0x100+12], 1       ; MipLevels");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    mov  rdx, [_gpu_shadowtex]");
    E("    lea  r8,  [rsp+0x100]");
    E("    lea  r9,  [_gpu_shadow_srv]");
    E("    call [rax + 0x38]                  ; CreateShaderResourceView");
    E("    test eax, eax");
    E("    jnz  .pl_fail");

    // Blend state (straight alpha): CreateBlendState -> _gpu_blend, slot 0xA0.
    // D3D11_BLEND_DESC (264 bytes) at rsp+0x100: AlphaToCoverage@0=0,
    // IndependentBlend@4=0, RenderTarget[0] at +8: BlendEnable@8=1,
    // SrcBlend@12=SRC_ALPHA(5), DestBlend@16=INV_SRC_ALPHA(6), BlendOp@20=ADD(1),
    // SrcBlendAlpha@24=ONE(2), DestBlendAlpha@28=INV_SRC_ALPHA(6),
    // BlendOpAlpha@32=ADD(1), WriteMask@36=0x0F (all channels).
    E("    lea  rdi, [rsp+0x100]");
    E("    xor  eax, eax");
    E("    mov  ecx, 66");                   // 264 bytes / 4
    E("    rep  stosd");
    E("    mov  dword [rsp+0x100+8], 1");    // BlendEnable
    E("    mov  dword [rsp+0x100+12], 5");   // SrcBlend = SRC_ALPHA
    E("    mov  dword [rsp+0x100+16], 6");   // DestBlend = INV_SRC_ALPHA
    E("    mov  dword [rsp+0x100+20], 1");   // BlendOp = ADD
    E("    mov  dword [rsp+0x100+24], 2");   // SrcBlendAlpha = ONE
    E("    mov  dword [rsp+0x100+28], 6");   // DestBlendAlpha = INV_SRC_ALPHA
    E("    mov  dword [rsp+0x100+32], 1");   // BlendOpAlpha = ADD
    E("    mov  dword [rsp+0x100+36], 0x0F");// RenderTargetWriteMask = ALL
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x100]");
    E("    lea  r8,  [_gpu_blend]");
    E("    call [rax + 0xA0]                 ; CreateBlendState");
    E("    test eax, eax");
    E("    jnz  .pl_fail");

    E("    call _slag_gpu_stage_init         ; alloc per-frame vertex stage");
    E("    mov  qword [_gpu_pipeline], 1");
    E("    jmp  .pl_ret");
    E(".pl_fail:");
    E("    mov  qword [_gpu_pipeline], 0");
    E(".pl_ret:");
    E("    add  rsp, 0x228");
    E("    pop  r12");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    ret");
}

// _slag_gpu_stage_init: HeapAlloc the per-frame vertex stage buffer once.
static void emit_gpu_stage_init(Codegen *cg) {
    E("GPU_STAGE_TRI    equ 192");        // raw bytes per staged triangle
    E("GPU_STAGE_CAP    equ 65536");      // INITIAL triangle capacity; _gpu_cap grows on demand (_slag_gpu_grow). vbuf ByteWidth is a 32-bit dword = cap*144, so a single buffer tops out ~29.8M tris.
    E("GPU_VTX_STRIDE   equ 64");         // float vertex: pos3 + uv2 + col4 + slice + flag (11 f32)
    E("MAP_WR_DISCARD   equ 4");
    E("TOPOLOGY_TRILIST equ 4");
    E("");
    E("; --- _slag_gpu_stage_init (alloc _gpu_stage + convbuf at initial cap) ---");
    E("_slag_gpu_stage_init:");
    E("    cmp  qword [_gpu_stage], 0");
    E("    jne  .si_done");
    E("    mov  qword [_gpu_cap], GPU_STAGE_CAP   ; initial capacity in triangles");
    E("    sub  rsp, 40");
    E("    call GetProcessHeap");
    E("    mov  rcx, rax");
    E("    xor  edx, edx");
    E("    mov  r8, [_gpu_cap]");
    E("    imul r8, GPU_STAGE_TRI");
    E("    call HeapAlloc");
    E("    mov  [_gpu_stage], rax");
    E("    ; cached scratch for converted float verts (cap tris * 3 * GPU_VTX_STRIDE)");
    E("    call GetProcessHeap");
    E("    mov  rcx, rax");
    E("    xor  edx, edx");
    E("    mov  r8, [_gpu_cap]");
    E("    imul r8, GPU_VTX_STRIDE * 3");
    E("    call HeapAlloc");
    E("    mov  [_gpu_convbuf], rax");
    E("    add  rsp, 40");
    E(".si_done:");
    E("    ret");

    // _slag_gpu_grow(rcx = needed triangles): if needed > current _gpu_cap, grow
    // the CPU scratch (convbuf, stage) via HeapReAlloc and RECREATE both D3D11
    // vertex buffers at the new size (Release old, CreateBuffer new). Forces a
    // re-upload next present (new buffers are empty). No-op if needed <= cap or
    // no device. Called from _slag_fill_triangle_gpu before staging.
    E("; --- _slag_gpu_grow(rcx = needed triangles) ---");
    E("_slag_gpu_grow:");
    E("    cmp  rcx, [_gpu_cap]");
    E("    jbe  .gg_ret                 ; capacity already sufficient");
    E("    mov  rax, [_gpu_device]");
    E("    test rax, rax");
    E("    jz   .gg_ret                 ; no device");
    E("    push rbx");
    E("    push r12");
    E("    push r13");
    E("    sub  rsp, 0x60               ; 3 pushes (odd)+0x60 => 16-aligned; BUFDESC at rsp+0x40");
    E("    mov  r12, rcx                ; r12 = needed (new cap)");
    E("    mov  [_gpu_cap], r12");
    // HeapReAlloc convbuf -> needed*3*GPU_VTX_STRIDE
    E("    call GetProcessHeap");
    E("    mov  rcx, rax");
    E("    xor  edx, edx");
    E("    mov  r8, [_gpu_convbuf]");
    E("    mov  r9, r12");
    E("    imul r9, GPU_VTX_STRIDE * 3");
    E("    call HeapReAlloc");
    E("    mov  [_gpu_convbuf], rax");
    // HeapReAlloc stage -> needed*GPU_STAGE_TRI
    E("    call GetProcessHeap");
    E("    mov  rcx, rax");
    E("    xor  edx, edx");
    E("    mov  r8, [_gpu_stage]");
    E("    mov  r9, r12");
    E("    imul r9, GPU_STAGE_TRI");
    E("    call HeapReAlloc");
    E("    mov  [_gpu_stage], rax");
    // Release old vbuf / vbuf2 (COM Release = vtbl+0x10) if present.
    E("    mov  rcx, [_gpu_vbuf]");
    E("    test rcx, rcx");
    E("    jz   .gg_v1done");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]");
    E("    mov  qword [_gpu_vbuf], 0");
    E(".gg_v1done:");
    E("    mov  rcx, [_gpu_vbuf2]");
    E("    test rcx, rcx");
    E("    jz   .gg_v2done");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]");
    E("    mov  qword [_gpu_vbuf2], 0");
    E(".gg_v2done:");
    // Build BUFDESC at rsp+0x40: bytewidth=cap*3*stride, dynamic, vertex, cpuwrite.
    E("    mov  rax, r12");
    E("    imul rax, 3 * GPU_VTX_STRIDE");
    E("    mov  dword [rsp+0x40+BUFDESC_BYTEWIDTH], eax");
    E("    mov  dword [rsp+0x40+BUFDESC_USAGE], USAGE_DYNAMIC");
    E("    mov  dword [rsp+0x40+BUFDESC_BIND], BIND_VERTEX");
    E("    mov  dword [rsp+0x40+BUFDESC_CPUACCESS], D3DCPU_WRITE");
    E("    mov  dword [rsp+0x40+16], 0");
    E("    mov  dword [rsp+0x40+20], 0");
    // CreateBuffer(device, &desc, NULL, &_gpu_vbuf) -- device vtbl+0x18
    E("    mov  rbx, [_gpu_device]");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x40]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_vbuf]");
    E("    call [rax + 0x18]");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x40]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_vbuf2]");
    E("    call [rax + 0x18]");
    // New buffers are empty -> force re-upload next present.
    E("    mov  qword [_gpu_up_valid], 0");
    E("    mov  qword [_gpu_vbuf_dirty], 1");
    E("    mov  qword [_gpu_vbuf_idx], 0");
    E("    add  rsp, 0x60");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbx");
    E(".gg_ret:");
    E("    ret");
}

// _slag_gpu_stage_pcolor(rcx=verts, rdx=tex_ptr, r8=tex_w, r9=tex_h):
// copy 192 raw bytes (3x8 int64) into _gpu_stage at the current tri slot.
static void emit_gpu_stage_pcolor(Codegen *cg) {
    E("; --- _slag_gpu_stage_pcolor(rcx=verts, rdx=tex_ptr, r8=tex_w, r9=tex_h) ---");
    E("_slag_gpu_stage_pcolor:");
    E("    mov  rax, [_gpu_stage_cnt]");
    E("    cmp  rax, GPU_STAGE_CAP");
    E("    jae  .sp_full");
    E("    mov  r10, [_gpu_stage]");
    E("    test r10, r10");
    E("    jz   .sp_full");
    E("    mov  [_gpu_stage_tex], rdx");
    E("    mov  [_gpu_stage_texw], r8");
    E("    mov  [_gpu_stage_texh], r9");
    E("    mov  r11, rax");
    E("    imul r11, GPU_STAGE_TRI");
    E("    add  r10, r11               ; dst = stage + cnt*192");
    E("    xor  r11, r11");
    E(".sp_copy:");
    E("    mov  rax, [rcx + r11]");
    E("    mov  [r10 + r11], rax");
    E("    add  r11, 8");
    E("    cmp  r11, GPU_STAGE_TRI");
    E("    jl   .sp_copy");
    E("    inc  qword [_gpu_stage_cnt]");
    E(".sp_full:");
    E("    ret");
}

// _slag_gpu_resize: rebuild the swapchain backbuffer + RTV + depth buffer at the
// current window client size. Called from the present frame when the window has
// been resized (window.width()/height() differ from _gpu_sc_w/_gpu_sc_h). Without
// this the fixed initial backbuffer is stretch-blitted to fill the new window,
// which magnifies/blurs the whole UI (bold text, taller title bars). Self-
// contained stack frame. Clobbers volatile regs only; callers reload state.
static void emit_gpu_resize(Codegen *cg) {
    E("; --- _slag_gpu_resize ---");
    E("_slag_gpu_resize:");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    push r12");
    // retaddr(8) + 4 pushes(32) = 40 => rsp%16==8. 0x128 (296, /16) restores
    // 16-byte alignment at the call sites (Win32/D3D fault on misaligned rsp).
    E("    sub  rsp, 0x128           ; TEXTURE2D_DESC + arg area (16-aligned)");

    E("    mov  rbx, [_window_primary_state]");
    E("    test rbx, rbx");
    E("    jz   .rz_ret");
    E("    mov  r12, [_gpu_context]");
    E("    test r12, r12");
    E("    jz   .rz_ret");

    // Unbind render targets before releasing them (OMSetRenderTargets(0,NULL,NULL)).
    E("    mov  rcx, r12");
    E("    xor  edx, edx");
    E("    xor  r8, r8");
    E("    xor  r9, r9");
    E("    mov  rax, [r12]");
    E("    call [rax + 0x108]                 ; OMSetRenderTargets(0,NULL,NULL)");

    // Release old RTV, DSV, depth texture (each: obj->Release, slot 2 / 0x10).
    E("    mov  rcx, [_gpu_rtv]");
    E("    test rcx, rcx");
    E("    jz   .rz_no_rtv");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]");
    E("    mov  qword [_gpu_rtv], 0");
    E(".rz_no_rtv:");
    E("    mov  rcx, [_gpu_dsv]");
    E("    test rcx, rcx");
    E("    jz   .rz_no_dsv");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]");
    E("    mov  qword [_gpu_dsv], 0");
    E(".rz_no_dsv:");
    E("    mov  rcx, [_gpu_depthtex]");
    E("    test rcx, rcx");
    E("    jz   .rz_no_dt");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]");
    E("    mov  qword [_gpu_depthtex], 0");
    E(".rz_no_dt:");

    // ResizeBuffers(BufferCount=0 keep, W, H, Format=UNKNOWN(0) keep, Flags=0x840)
    // -- IDXGISwapChain slot 13 (0x68).
    E("    mov  rcx, [_gpu_swapchain]");
    E("    xor  edx, edx                      ; BufferCount = 0 (preserve)");
    E("    mov  r8d,  [rbx + 48]              ; new width");
    E("    mov  r9d,  [rbx + 56]              ; new height");
    E("    xor  eax, eax                      ; NewFormat = DXGI_FORMAT_UNKNOWN (keep)");
    E("    mov  [rsp+0x20], eax");
    E("    mov  dword [rsp+0x28], 0x840       ; SwapChainFlags (match creation)");
    E("    mov  rax, [_gpu_swapchain]");
    E("    mov  rax, [rax]");
    E("    call [rax + 0x68]                  ; ResizeBuffers");
    E("    test eax, eax");
    E("    jnz  .rz_ret                       ; on failure leave device as-is");

    // GetBuffer(0, IID_ID3D11Texture2D, &backbuffer) -- slot 9 (0x48).
    E("    mov  dword [rsp+0x100], 0x6f15aaf2");
    E("    mov  dword [rsp+0x104], 0x4e89d208");
    E("    mov  dword [rsp+0x108], 0x9548b49a");
    E("    mov  dword [rsp+0x10C], 0x9c4fd335");
    E("    mov  rcx, [_gpu_swapchain]");
    E("    mov  rax, [rcx]");
    E("    xor  edx, edx");
    E("    lea  r8,  [rsp+0x100]");
    E("    lea  r9,  [rsp+0x110]              ; &backbuffer");
    E("    call [rax + 0x48]                  ; GetBuffer");
    E("    test eax, eax");
    E("    jnz  .rz_ret");

    // CreateRenderTargetView(backbuffer, NULL, &_gpu_rtv) -- device slot 9 (0x48).
    E("    mov  rcx, [_gpu_device]");
    E("    mov  rax, [rcx]");
    E("    mov  rdx, [rsp+0x110]");
    E("    xor  r8, r8");
    E("    lea  r9, [_gpu_rtv]");
    E("    call [rax + 0x48]                  ; CreateRenderTargetView");
    // Release the backbuffer ref (RTV holds its own).
    E("    mov  rcx, [rsp+0x110]");
    E("    test rcx, rcx");
    E("    jz   .rz_depth");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]");
    E(".rz_depth:");

    // Recreate the depth texture (D32_FLOAT) at the new size. TEXTURE2D_DESC (44B)
    // at rsp+0x60, identical to the pipeline's original.
    E("    mov  eax, [rbx + 48]");
    E("    mov  [rsp+0x60+0], eax             ; Width");
    E("    mov  eax, [rbx + 56]");
    E("    mov  [rsp+0x60+4], eax             ; Height");
    E("    mov  dword [rsp+0x60+8], 1         ; MipLevels");
    E("    mov  dword [rsp+0x60+12], 1        ; ArraySize");
    E("    mov  dword [rsp+0x60+16], 0x28     ; DXGI_FORMAT_D32_FLOAT");
    E("    mov  dword [rsp+0x60+20], 1        ; SampleDesc.Count");
    E("    mov  dword [rsp+0x60+24], 0        ; SampleDesc.Quality");
    E("    mov  dword [rsp+0x60+28], 0        ; USAGE_DEFAULT");
    E("    mov  dword [rsp+0x60+32], 0x40     ; BIND_DEPTH_STENCIL");
    E("    mov  dword [rsp+0x60+36], 0");
    E("    mov  dword [rsp+0x60+40], 0");
    E("    mov  rcx, [_gpu_device]");
    E("    mov  rax, [rcx]");
    E("    lea  rdx, [rsp+0x60]");
    E("    xor  r8, r8");
    E("    lea  r9, [_gpu_depthtex]");
    E("    call [rax + 0x28]                  ; CreateTexture2D");
    E("    test eax, eax");
    E("    jnz  .rz_ret");
    // CreateDepthStencilView(depthtex, NULL, &_gpu_dsv) -- device slot 10 (0x50).
    E("    mov  rcx, [_gpu_device]");
    E("    mov  rax, [rcx]");
    E("    mov  rdx, [_gpu_depthtex]");
    E("    xor  r8, r8");
    E("    lea  r9, [_gpu_dsv]");
    E("    call [rax + 0x50]                  ; CreateDepthStencilView");

    // Record the new size so we don't resize again until it changes.
    E("    mov  eax, [rbx + 48]");
    E("    mov  [_gpu_sc_w], rax");
    E("    mov  eax, [rbx + 56]");
    E("    mov  [_gpu_sc_h], rax");

    E(".rz_ret:");
    E("    add  rsp, 0x128");
    E("    pop  r12");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    ret");
}

// _slag_gpu_present_frame: staged raw verts -> float vbuf, upload tex, set
// state, Draw, Present. No-op if nothing staged. Resets _gpu_stage_cnt.
static void emit_gpu_present_frame(Codegen *cg) {
    emit_gpu_resize(cg);
    E("; --- _slag_gpu_present_frame ---");
    E("_slag_gpu_present_frame:");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    push r12");
    E("    push r13");
    E("    push r14");
    E("    push r15");
    E("    sub  rsp, 0xC0");
    // Preserve callee-saved xmm6/xmm7 (used for texw/texh in the convert loop).
    E("    movaps [rsp+0xA0], xmm6");
    E("    movaps [rsp+0xB0], xmm7");

    E("    mov  r14, [_gpu_draw_cnt]");
    E("    test r14, r14");
    E("    jz   .pf_ret");
    E("    mov  r15, [_gpu_context]");
    E("    test r15, r15");
    E("    jz   .pf_ret");
    E("    mov  rbx, [_window_primary_state]");
    E("    test rbx, rbx");
    E("    jz   .pf_ret");

    // Detect a window resize: if the live client size differs from the last
    // sized backbuffer, rebuild the swapchain/RTV/depth at the new size so the
    // UI renders 1:1 instead of the fixed backbuffer being stretch-blitted
    // (which fattened text and grew title bars). r15 (context) is reloaded after.
    E("    xor  r12d, r12d              ; r12 = resized-this-frame flag (0 = no)");
    E("    mov  eax, [rbx + 48]              ; live width");
    E("    cmp  rax, [_gpu_sc_w]");
    E("    jne  .pf_do_resize");
    E("    mov  eax, [rbx + 56]              ; live height");
    E("    cmp  rax, [_gpu_sc_h]");
    E("    je   .pf_after_size");
    E(".pf_do_resize:");
    E("    call _slag_gpu_resize");
    // A resize tears down and recreates the RTV/DSV; skip drawing THIS frame and
    // let the resident geometry redraw cleanly next frame. Drawing immediately
    // after the rebuild raced the Map/copy (.pf_copy) and faulted. _gpu_stage_cnt
    // is preserved (not zeroed) so the app's next fill_triangle_gpu + present
    // draws normally at the new size.
    E("    jmp  .pf_ret");
    E(".pf_after_size:");

    // Frame-latency pace: block on the waitable object until the swapchain is
    // ready for a new frame. Keeps the CPU one frame ahead for steady pacing.
    // SKIP the wait on a frame we just resized: after ResizeBuffers the waitable
    // isn't promptly signaled, so the wait burns up to its full timeout and makes
    // live corner-drag resizing stutter. Presenting immediately keeps drag smooth.
    E("    test r12d, r12d");
    E("    jnz  .pf_nowait");
    E("    mov  rcx, [_gpu_waitable]");
    E("    test rcx, rcx");
    E("    jz   .pf_nowait");
    E("    mov  edx, 100               ; timeout 100ms (safety; normally signaled)");
    E("    sub  rsp, 32");
    E("    call WaitForSingleObject");
    E("    add  rsp, 32");
    E(".pf_nowait:");

    // Total vertex count to upload = running offset accumulated by the draw-list
    // appends (sum of every item's vertexCount). Per-item Draw ranges come from
    // the items themselves in the loop below.
    E("    mov  r13, [_gpu_stage_off]");

    // Select this frame's vertex buffer (double-buffered).
    E("    mov  rax, [_gpu_vbuf]");
    E("    cmp  qword [_gpu_vbuf_idx], 0");
    E("    je   .pf_vbuf_sel");
    E("    mov  rax, [_gpu_vbuf2]");
    E(".pf_vbuf_sel:");
    E("    mov  [rsp+0x38], rax        ; current-frame vbuf");

    // Static-geometry fast path: skip Map + the whole vertex re-upload when the
    // convbuf is unchanged AND this vbuf already holds it. With static geometry
    // both vbufs fill in the first 2 frames, then every frame jumps straight to
    // Draw -- no Map, no 9.6MB WC copy, near-zero per-frame CPU vertex cost.
    E("    cmp  qword [_gpu_vbuf_dirty], 0");
    E("    jne  .pf_upload");
    E("    cmp  qword [_gpu_up_valid], 2");
    E("    jae  .pf_draw_ready         ; unchanged + both vbufs filled -> skip upload");
    E(".pf_upload:");

    // Map vertex buffer (WRITE_DISCARD) -> mapped subres at rsp+0x40
    E("    lea  r11, [rsp+0x40]");
    E("    mov  [rsp+0x28], r11        ; &mapped (6th arg)");
    E("    mov  dword [rsp+0x20], 0    ; MapFlags (5th arg)");
    E("    mov  rcx, r15");
    E("    mov  rdx, [rsp+0x38]        ; current vbuf");
    E("    xor  r8d, r8d");
    E("    mov  r9d, MAP_WR_DISCARD");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x70]           ; Map");
    E("    test eax, eax");
    E("    jnz  .pf_ret");

    // fill_triangle_gpu already wrote float32 verts into _gpu_convbuf; skip the
    // int64->float convert pass entirely and go straight to the bulk copy.
    E("    cmp  qword [_gpu_prebuilt], 0");
    E("    jne  .pf_copy");
    E("    mov  rdi, [_gpu_convbuf]    ; convert into CACHED scratch, not WC map");
    E("    mov  rsi, [_gpu_stage]");
    // Precompute reciprocals once: 1/texw, 1/texh, 1/255. The per-vertex
    // divss (3x/vertex, ~14cy each) becomes mulss (~4cy), ~3x cheaper.
    E("    mov  eax, 1");
    E("    cvtsi2ss xmm2, eax          ; 1.0");
    E("    mov  eax, [_gpu_stage_texw]");
    E("    cvtsi2ss xmm0, eax");
    E("    movss xmm6, xmm2");
    E("    divss xmm6, xmm0            ; xmm6 = 1/texw");
    E("    mov  eax, [_gpu_stage_texh]");
    E("    cvtsi2ss xmm0, eax");
    E("    movss xmm7, xmm2");
    E("    divss xmm7, xmm0            ; xmm7 = 1/texh");
    E("    mov  eax, 255");
    E("    cvtsi2ss xmm0, eax");
    E("    movss xmm4, xmm2");
    E("    divss xmm4, xmm0            ; xmm4 = 1/255");
    E("    xor  r12, r12               ; vertex index");
    E(".pf_conv:");
    E("    mov  rcx, r12");
    E("    imul rcx, 64                ; verts packed 64B in stage");
    E("    lea  r10, [rsi + rcx]");
    E("    cvtsi2ss xmm0, qword [r10+0]");
    E("    movss [rdi+0], xmm0");
    E("    cvtsi2ss xmm0, qword [r10+8]");
    E("    movss [rdi+4], xmm0");
    E("    cvtsi2ss xmm0, qword [r10+16]");
    E("    movss [rdi+8], xmm0");
    E("    cvtsi2ss xmm0, qword [r10+24]");
    E("    mulss xmm0, xmm6");
    E("    movss [rdi+12], xmm0");
    E("    cvtsi2ss xmm0, qword [r10+32]");
    E("    mulss xmm0, xmm7");
    E("    movss [rdi+16], xmm0");
    E("    cvtsi2ss xmm0, qword [r10+40]");
    E("    mulss xmm0, xmm4");
    E("    movss [rdi+20], xmm0");
    E("    cvtsi2ss xmm0, qword [r10+48]");
    E("    mulss xmm0, xmm4");
    E("    movss [rdi+24], xmm0");
    E("    cvtsi2ss xmm0, qword [r10+56]");
    E("    mulss xmm0, xmm4");
    E("    movss [rdi+28], xmm0");
    E("    movss [rdi+32], xmm2        ; a = 1.0 (pcolor has no alpha source)");
    E("    add  rdi, GPU_VTX_STRIDE");
    E("    inc  r12");
    E("    cmp  r12, r13");
    E("    jl   .pf_conv");

    // Bulk sequential copy of the converted verts into the WC mapped buffer.
    // (Scattered per-field movss into write-combined memory is pathologically
    // slow; a single streaming rep movsq is orders of magnitude faster.)
    E(".pf_copy:");
    E("    mov  rsi, [_gpu_convbuf]");
    E("    mov  rdi, [rsp+0x40]        ; mapped.pData");
    E("    mov  rcx, r13");
    E("    imul rcx, GPU_VTX_STRIDE / 4  ; dwords = verts * stride / 4 (48B -> verts*12)");
    E("    rep  movsd");

    // Unmap vertex buffer (the same double-buffered vbuf mapped above)
    E("    mov  rcx, r15");
    E("    mov  rdx, [rsp+0x38]        ; current vbuf");
    E("    xor  r8d, r8d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x78]           ; Unmap");

    // This vbuf now holds the current geometry; count it toward "both filled".
    E("    inc  qword [_gpu_up_valid]");
    E("    mov  qword [_gpu_vbuf_dirty], 0");
    E(".pf_draw_ready:");

    // Clear the backbuffer color + shared depth ONCE before the item loop (each
    // item draws into the same target; clearing per item would wipe prior items).
    // Bind RTV+depth for the clear (items re-bind after their shadow pass).
    E("    mov  rax, [_gpu_rtv]");
    E("    mov  [rsp+0x90], rax");
    E("    mov  rcx, r15");
    E("    mov  edx, 1");
    E("    lea  r8, [rsp+0x90]");
    E("    mov  r9, [_gpu_dsv]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x108]           ; OMSetRenderTargets(rtv,dsv)");
    E("    cmp  qword [_gpu_clear_set], 0");
    E("    je   .pf_clr_fog0");
    E("    mov  r8, [_gpu_clear_ptr]");
    E("    jmp  .pf_clr_go0");
    E(".pf_clr_fog0:");
    E("    mov  dword [rsp+0x40], 0x3F168166");
    E("    mov  dword [rsp+0x44], 0x3F2AAAAB");
    E("    mov  dword [rsp+0x48], 0x3F43D70A");
    E("    mov  eax, 0x3F800000");
    E("    mov  [rsp+0x4C], eax");
    E("    lea  r8, [rsp+0x40]");
    E(".pf_clr_go0:");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_rtv]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x190]           ; ClearRenderTargetView");
    E("    mov  byte [rsp+0x20], 0");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_dsv]");
    E("    mov  r8d, 1");
    E("    mov  eax, 0x3F800000");
    E("    movd xmm3, eax");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x1A8]           ; ClearDepthStencilView (1.0)");

    // ================= DRAW-ITEM LOOP ===================================
    // Iterate the appended draw items. Each iteration loads the item's tex +
    // viewproj + light into the globals the cbuffer/shadow/draw code reads, then
    // runs one shadow pass (if lit) + one main Draw over the item's vertex range.
    // r14 = item index (0.._gpu_draw_cnt-1). One Present after the loop.
    E("    xor  r14, r14               ; item index");
    E(".pf_item_loop:");
    E("    mov  r11, r14");
    E("    imul r11, r11, 64");
    E("    lea  rax, [_gpu_draw_items]         ; load base separately (avoid ADDR32 [sym+reg] reloc)");
    E("    add  r11, rax                       ; &item");
    // publish this item's fields into the globals the downstream code reads
    E("    mov  rax, [r11 + 16]");
    E("    mov  [_gpu_stage_tex], rax");
    E("    mov  rax, [r11 + 24]");
    E("    mov  [_gpu_stage_texw], rax");
    E("    mov  rax, [r11 + 32]");
    E("    mov  [_gpu_stage_texh], rax");
    E("    mov  rax, [r11 + 40]");
    E("    mov  [_gpu_viewproj], rax");
    E("    mov  rax, [r11 + 48]");
    E("    mov  [_gpu_lightproj], rax");
    E("    mov  rax, [r11 + 56]");
    E("    mov  [_gpu_lightdir], rax");
    // this item's Draw range into free stack slots (0x58/0x60 are unused by the
    // cbuffer/tex/state/viewport/RTV code between here and the Draw).
    E("    mov  rax, [r11 + 0]");
    E("    mov  [_gpu_cur_start], rax  ; startVertex (BSS: stack 0x58/0x60 alias IASetVertexBuffers)");
    E("    mov  rax, [r11 + 8]");
    E("    mov  [_gpu_cur_count], rax  ; vertexCount");
    // force the tex re-upload check to run per item (each item may bind a diff tex)
    E("    mov  qword [_gpu_tex_uploaded], 0");

    // Select shadowPass=1 for this cbuffer write when a light is set, so the
    // shadow depth pass (which runs first) sees shadowPass=1. The main pass patches
    // it back to 0 (single-dword cbuffer update) just before its Draw.
    E("    mov  qword [_gpu_shadowpass_val], 0");
    E("    cmp  qword [_gpu_lightproj], 0");
    E("    je   .pf_sp_set");
    E("    mov  qword [_gpu_shadowpass_val], 1");
    E(".pf_sp_set:");

    // Constant buffer: copy the 64-byte view-projection matrix supplied by the
    // Slag camera (gpu.set_viewproj) into the cbuf each frame. Skip if none set.
    E("    mov  rsi, [_gpu_viewproj]");
    E("    test rsi, rsi");
    E("    jz   .pf_after_cb           ; no camera matrix -> leave cbuf as-is");
    E("    lea  r11, [rsp+0x40]");
    E("    mov  [rsp+0x28], r11");
    E("    mov  dword [rsp+0x20], 0");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_cbuf]");
    E("    xor  r8d, r8d");
    E("    mov  r9d, MAP_WR_DISCARD");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x70]");
    E("    test eax, eax");
    E("    jnz  .pf_after_cb");
    E("    mov  rdi, [rsp+0x40]        ; mapped.pData");
    E("    mov  rsi, [_gpu_viewproj]");
    E("    mov  ecx, 12                ; 12 qwords = 96 bytes (4x4 matrix + fog cbuf tail)");
    E("    rep  movsq");
    // Inject invTexDims (1/texw,1/texh) at cbuf offset 84 for the VS uv normalize.
    // Computed once per frame from _gpu_stage_texw/h (not per vertex). pData in rdi
    // was advanced by rep movsq; reload base from [rsp+0x40].
    E("    mov  rdi, [rsp+0x40]");
    E("    mov  eax, 1");
    E("    cvtsi2ss xmm1, eax          ; 1.0");
    E("    mov  eax, [_gpu_stage_texw]");
    E("    cvtsi2ss xmm0, eax");
    E("    movss xmm2, xmm1");
    E("    divss xmm2, xmm0            ; 1/texw");
    E("    movss [rdi+84], xmm2");
    E("    mov  eax, [_gpu_stage_texh]");
    E("    cvtsi2ss xmm0, eax");
    E("    movss xmm2, xmm1");
    E("    divss xmm2, xmm0            ; 1/texh");
    E("    movss [rdi+88], xmm2");
    // Light view-projection matrix (16 f32) -> cbuf offset 96. When no light is
    // set (_gpu_lightproj==0) leave it zero; the shadow term stays neutral.
    E("    mov  rsi, [_gpu_lightproj]");
    E("    test rsi, rsi");
    E("    jz   .pf_cb_nolight");
    E("    mov  rdi, [rsp+0x40]        ; mapped pData (deref, not lea)");
    E("    add  rdi, 96                ; cbuf offset 96 = lightVP");
    E("    mov  ecx, 8                 ; 8 qwords = 64 bytes");
    E("    rep  movsq");
    // lightDir (3 f32) -> cbuf offset 160
    E("    mov  rsi, [_gpu_lightdir]");
    E("    test rsi, rsi");
    E("    jz   .pf_cb_nolight");
    E("    mov  rdi, [rsp+0x40]        ; mapped pData (deref, not lea)");
    E("    add  rdi, 160");
    E("    mov  ecx, 3");
    E(".pf_cb_ldir:");
    E("    mov  eax, [rsi]");
    E("    mov  [rdi], eax");
    E("    add  rsi, 4");
    E("    add  rdi, 4");
    E("    dec  ecx");
    E("    jnz  .pf_cb_ldir");
    E(".pf_cb_nolight:");
    // shadowPass flag (f32) -> cbuf offset 172. 1.0 for the shadow pass, else 0.0.
    E("    mov  rdi, [rsp+0x40]");
    E("    xor  eax, eax");
    E("    cmp  qword [_gpu_shadowpass_val], 0");
    E("    je   .pf_cb_spz");
    E("    mov  eax, 0x3F800000        ; 1.0f");
    E(".pf_cb_spz:");
    E("    mov  [rdi+172], eax");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_cbuf]");
    E("    xor  r8d, r8d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x78]           ; Unmap cbuf");
    E(".pf_after_cb:");

    // UpdateSubresource per array slice: tex_ptr is a contiguous block of
    // GPU_TEX_SLICES images (512x512 BGRA, slice k at tex_ptr + k*1048576),
    // uploaded into subresource k. One-time: skipped when the base tex_ptr is
    // unchanged from the last upload (static texture set uploads exactly once).
    // NOTE: keyed on tex_ptr, not contents; mutating in place at the same ptr
    // must change the ptr (or this stays stale).
    E("    mov  r11, [_gpu_stage_tex]");
    E("    cmp  r11, [_gpu_tex_uploaded]");
    E("    je   .pf_tex_done           ; same texture as last frame -> no re-upload");
    E("    mov  eax, [_gpu_stage_texw]");
    E("    shl  eax, 2                 ; RowPitch = texw*4 (texw = low 32 of stage_texw)");
    E("    mov  [rsp+0x28], eax");
    E("    mov  dword [rsp+0x30], 0    ; DepthPitch");
    // Slice count = high 32 bits of stage_texw (tex_w packed as texw | (nslices<<32)).
    // Zero high bits -> 1 slice, so single-texture callers upload only slice 0.
    E("    mov  rax, [_gpu_stage_texw]");
    E("    shr  rax, 32                ; nslices");
    E("    test rax, rax");
    E("    jnz  .pf_texn");
    E("    mov  rax, 1                 ; default 1 slice");
    E(".pf_texn:");
    E("    cmp  rax, GPU_TEX_SLICES");
    E("    jbe  .pf_texn_ok");
    E("    mov  rax, GPU_TEX_SLICES    ; clamp to array capacity");
    E(".pf_texn_ok:");
    E("    mov  rsi, rax               ; rsi = slice upload count (callee-saved, preserves r13 vcount)");
    E("    xor  r12, r12               ; slice index k");
    E(".pf_texloop:");
    E("    mov  rax, r12");
    E("    imul rax, 1048576           ; k * (512*512*4)");
    E("    add  rax, [_gpu_stage_tex]  ; pSrcData = base + k*sliceBytes");
    E("    mov  [rsp+0x20], rax");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_tex]");
    E("    mov  r8d, r12d              ; DstSubresource = k");
    E("    xor  r9, r9                 ; pDstBox = NULL");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x180]          ; UpdateSubresource");
    E("    inc  r12");
    E("    cmp  r12, rsi");
    E("    jl   .pf_texloop");
    E("    mov  r11, [_gpu_stage_tex]");
    E("    mov  [_gpu_tex_uploaded], r11   ; record uploaded texture");
    E(".pf_tex_done:");

    // IASetVertexBuffers(0, 1, &vbuf, &stride, &offset) -- per frame: the bound
    // vbuf alternates each frame (double-buffered), so this must run every frame.
    E("    mov  dword [rsp+0x60], GPU_VTX_STRIDE");
    E("    mov  dword [rsp+0x68], 0");
    E("    mov  rax, [rsp+0x38]        ; current vbuf");
    E("    mov  [rsp+0x70], rax");
    E("    lea  r11, [rsp+0x60]");
    E("    mov  [rsp+0x20], r11        ; pStrides");
    E("    lea  r11, [rsp+0x68]");
    E("    mov  [rsp+0x28], r11        ; pOffsets");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");
    E("    mov  r8d, 1");
    E("    lea  r9, [rsp+0x70]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x90]");

    // Invariant pipeline state (layout, topology, shaders, cbuf binding, SRV,
    // sampler, rasterizer) never changes frame-to-frame -- bind it once, then
    // skip all 8 context calls on every subsequent frame.
    E("    cmp  qword [_gpu_state_set], 0");
    E("    jne  .pf_state_done");
    // IASetInputLayout(layout)
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_layout]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x88]");
    // IASetPrimitiveTopology(TRIANGLELIST)
    E("    mov  rcx, r15");
    E("    mov  edx, TOPOLOGY_TRILIST");
    E("    mov  rax, [r15]");
    E("    call [rax + 0xC0]");
    // VSSetShader(vs, NULL, 0)
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_vs]");
    E("    xor  r8, r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x58]");
    // VSSetConstantBuffers(0, 1, &cbuf)
    E("    mov  rax, [_gpu_cbuf]");
    E("    mov  [rsp+0x78], rax");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");
    E("    mov  r8d, 1");
    E("    lea  r9, [rsp+0x78]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x38]");
    // PSSetConstantBuffers(0, 1, &cbuf) -- REQUIRED: the pixel shader reads b0
    // (camPos, lightVP, fog). Without this the PS sees b0 as all zeros, killing
    // the diffuse light and the shadow lookup. slot 0x80 (before IASetInputLayout
    // 0x88, after Unmap 0x78 -- confirmed against the vtable calls in this file).
    E("    mov  rax, [_gpu_cbuf]");
    E("    mov  [rsp+0x78], rax");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");
    E("    mov  r8d, 1");
    E("    lea  r9, [rsp+0x78]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x80]");
    // PSSetShader(ps, NULL, 0)
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_ps]");
    E("    xor  r8, r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x48]");
    // PSSetShaderResources(0, 1, &srv)
    E("    mov  rax, [_gpu_srv]");
    E("    mov  [rsp+0x80], rax");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");
    E("    mov  r8d, 1");
    E("    lea  r9, [rsp+0x80]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x40]");
    // PSSetSamplers(0, 3, {point s0, linear s1, comparison s2}) -- 3-entry array
    // at rsp+0x88/0x90/0x98. s2 = shadow-map comparison sampler (required by the
    // PS SampleCmpLevelZero). Latched once here with the rest of invariant state.
    E("    mov  rax, [_gpu_sampler]");
    E("    mov  [rsp+0x88], rax");
    E("    mov  rax, [_gpu_sampler_lin]");
    E("    mov  [rsp+0x90], rax");
    E("    mov  rax, [_gpu_sampler_cmp]");
    E("    mov  [rsp+0x98], rax");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");
    E("    mov  r8d, 3");
    E("    lea  r9, [rsp+0x88]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x50]");
    // RSSetState(_gpu_raster)
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_raster]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x158]");
    E("    mov  qword [_gpu_state_set], 1");
    E(".pf_state_done:");

    // ================= SHADOW DEPTH PASS =================================
    // Only when a light is set. Renders the resident geometry from the light's
    // POV into the 2048x2048 shadow depth map. Depth-only: NULL pixel shader (no
    // fragment work), lit-flagged verts only (VS clips the rest). The cbuffer was
    // already written with shadowPass=1 selected via _gpu_shadowpass_val below.
    E("    cmp  qword [_gpu_lightproj], 0");
    E("    je   .pf_shadow_skip");
    // unbind the shadow SRV from t2 (can't be SRV + depth target at once)
    E("    xor  eax, eax");
    E("    mov  [rsp+0x90], rax");
    E("    mov  rcx, r15");
    E("    mov  edx, 2");
    E("    mov  r8d, 1");
    E("    lea  r9, [rsp+0x90]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x40]           ; PSSetShaderResources(2,1,{NULL})");
    // NULL pixel shader -> depth-only (fast).
    E("    mov  rcx, r15");
    E("    xor  rdx, rdx");
    E("    xor  r8, r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x48]           ; PSSetShader(NULL)");
    // viewport = 2048x2048
    E("    mov  dword [rsp+0x40], 0");
    E("    mov  dword [rsp+0x44], 0");
    E("    mov  eax, 0x45000000        ; 2048.0f");
    E("    mov  [rsp+0x48], eax");
    E("    mov  [rsp+0x4C], eax");
    E("    mov  dword [rsp+0x50], 0");
    E("    mov  eax, 0x3F800000");
    E("    mov  [rsp+0x54], eax");
    E("    mov  rcx, r15");
    E("    mov  edx, 1");
    E("    lea  r8, [rsp+0x40]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x160]          ; RSSetViewports");
    // OMSetRenderTargets(0, NULL, shadowDSV) -- depth target only, no color
    E("    mov  rcx, r15");
    E("    xor  edx, edx");
    E("    xor  r8, r8");
    E("    mov  r9, [_gpu_shadow_dsv]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x108]          ; OMSetRenderTargets(0,NULL,shadowDSV)");
    // clear shadow depth to 1.0
    E("    mov  byte [rsp+0x20], 0");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_shadow_dsv]");
    E("    mov  r8d, 1");
    E("    mov  eax, 0x3F800000");
    E("    movd xmm3, eax");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x1A8]          ; ClearDepthStencilView(shadowDSV,1.0)");
    // Draw this item's geometry into the shadow map (its vertex range)
    E("    mov  rcx, r15");
    E("    mov  edx, [_gpu_cur_count]  ; vertexCount");
    E("    mov  r8d, [_gpu_cur_start]  ; StartVertexLocation");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x68]           ; Draw (shadow depth)");
    // restore the real pixel shader for the main pass
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_ps]");
    E("    xor  r8, r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x48]           ; PSSetShader(_gpu_ps)");
    // bind the shadow SRV at t2 for the main pass sampling
    E("    mov  rax, [_gpu_shadow_srv]");
    E("    mov  [rsp+0x90], rax");
    E("    mov  rcx, r15");
    E("    mov  edx, 2");
    E("    mov  r8d, 1");
    E("    lea  r9, [rsp+0x90]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x40]           ; PSSetShaderResources(2,1,{shadowSRV})");
    E(".pf_shadow_skip:");

    // OMSetBlendState(mode? _gpu_blend : NULL, NULL factor, 0xFFFFFFFF) -- bound
    // every frame (not latched) since gpu.set_blend can change mode at runtime.
    E("    xor  rdx, rdx                     ; NULL = opaque (default)");
    E("    cmp  qword [_gpu_blend_mode], 0");
    E("    je   .pf_blend_go");
    E("    mov  rdx, [_gpu_blend]");
    E(".pf_blend_go:");
    E("    mov  rcx, r15");
    E("    xor  r8, r8                       ; BlendFactor = NULL");
    E("    mov  r9d, 0xFFFFFFFF              ; SampleMask");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x118]                ; OMSetBlendState");
    // RSSetViewports(1, &vp) -- vp at rsp+0x40
    E("    mov  dword [rsp+0x40], 0");
    E("    mov  dword [rsp+0x44], 0");
    E("    mov  eax, [rbx + 48]");
    E("    cvtsi2ss xmm0, eax");
    E("    movss [rsp+0x48], xmm0");
    E("    mov  eax, [rbx + 56]");
    E("    cvtsi2ss xmm0, eax");
    E("    movss [rsp+0x4C], xmm0");
    E("    mov  dword [rsp+0x50], 0");
    E("    mov  eax, 0x3F800000");
    E("    mov  [rsp+0x54], eax");
    E("    mov  rcx, r15");
    E("    mov  edx, 1");
    E("    lea  r8, [rsp+0x40]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x160]");
    // OMSetDepthStencilState(_gpu_dsstate, 0)
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_dsstate]");
    E("    xor  r8d, r8d               ; StencilRef");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x120]");
    // OMSetRenderTargets(1, &rtv, dsv) -- depth buffer now bound
    E("    mov  rax, [_gpu_rtv]");
    E("    mov  [rsp+0x90], rax");
    E("    mov  rcx, r15");
    E("    mov  edx, 1");
    E("    lea  r8, [rsp+0x90]");
    E("    mov  r9, [_gpu_dsv]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x108]           ; OMSetRenderTargets(rtv,dsv) -- rebind (shadow pass changed RT); NO clear here (cleared once pre-loop)");
    // Main pass needs shadowPass=0. When a light is set the cbuffer currently holds
    // shadowPass=1 (for the shadow pass), so re-map WRITE_DISCARD and rewrite it with
    // shadowPass=0. Full re-write (~176B) is trivial vs the draws; single cbuffer,
    // no extra resource. Skipped entirely when no light (cbuffer already shadowPass=0).
    E("    cmp  qword [_gpu_lightproj], 0");
    E("    je   .pf_main_cb_done");
    E("    lea  r11, [rsp+0x40]");
    E("    mov  [rsp+0x28], r11");
    E("    mov  dword [rsp+0x20], 0");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_cbuf]");
    E("    xor  r8d, r8d");
    E("    mov  r9d, MAP_WR_DISCARD");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x70]");
    E("    test eax, eax");
    E("    jnz  .pf_main_cb_done");
    E("    mov  rdi, [rsp+0x40]");
    E("    mov  rsi, [_gpu_viewproj]");
    E("    mov  ecx, 12");
    E("    rep  movsq");                 // viewproj + fog tail (96)
    E("    mov  rdi, [rsp+0x40]");
    E("    mov  eax, 1");
    E("    cvtsi2ss xmm1, eax");
    E("    mov  eax, [_gpu_stage_texw]");
    E("    cvtsi2ss xmm0, eax");
    E("    movss xmm2, xmm1");
    E("    divss xmm2, xmm0");
    E("    movss [rdi+84], xmm2");
    E("    mov  eax, [_gpu_stage_texh]");
    E("    cvtsi2ss xmm0, eax");
    E("    movss xmm2, xmm1");
    E("    divss xmm2, xmm0");
    E("    movss [rdi+88], xmm2");
    E("    mov  rsi, [_gpu_lightproj]");
    E("    mov  rdi, [rsp+0x40]        ; mapped pData (deref, not lea)");
    E("    add  rdi, 96");
    E("    mov  ecx, 8");
    E("    rep  movsq");                 // lightVP (64)
    E("    mov  rsi, [_gpu_lightdir]");
    E("    test rsi, rsi");
    E("    jz   .pf_main_ldir_done");
    E("    mov  rdi, [rsp+0x40]        ; mapped pData (deref, not lea)");
    E("    add  rdi, 160");
    E("    mov  ecx, 3");
    E(".pf_main_ldir:");
    E("    mov  eax, [rsi]");
    E("    mov  [rdi], eax");
    E("    add  rsi, 4");
    E("    add  rdi, 4");
    E("    dec  ecx");
    E("    jnz  .pf_main_ldir");
    E(".pf_main_ldir_done:");
    E("    mov  rdi, [rsp+0x40]");
    E("    mov  dword [rdi+172], 0        ; shadowPass = 0.0 (main pass)");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_cbuf]");
    E("    xor  r8d, r8d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x78]           ; Unmap cbuf");
    E(".pf_main_cb_done:");
    // Draw this item's vertex range: Draw(VertexCount, StartVertexLocation)
    E("    mov  rcx, r15");
    E("    mov  edx, [_gpu_cur_count]  ; vertexCount");
    E("    mov  r8d, [_gpu_cur_start]  ; StartVertexLocation");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x68]           ; Draw (main)");

    // ---- next draw item ----
    E("    inc  r14");
    E("    cmp  r14, [_gpu_draw_cnt]");
    E("    jae  .pf_items_done");
    E("    jmp  .pf_item_loop");
    E(".pf_items_done:");

    // Present ONCE after all items.
    // Present(SyncInterval=0, Flags=DXGI_PRESENT_ALLOW_TEARING). The tearing
    // flag pairs with the swapchain's ALLOW_TEARING flag to present immediately
    // instead of syncing to the display refresh (flip-model default).
    E("    mov  rcx, [_gpu_swapchain]");
    E("    xor  edx, edx               ; SyncInterval = 0");
    E("    mov  r8d, 0x200             ; DXGI_PRESENT_ALLOW_TEARING");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x40]");

    // Flip the double-buffer index for next frame (0<->1).
    E("    mov  rax, [_gpu_vbuf_idx]");
    E("    xor  rax, 1");
    E("    mov  [_gpu_vbuf_idx], rax");

    E("    mov  qword [_gpu_stage_cnt], 0");
    E("    mov  qword [_gpu_prebuilt], 0");
    E(".pf_ret:");
    // Reset the draw-list on EVERY exit path (items are rebuilt each frame by the
    // app's fill_triangle_gpu calls; skipping this on an early-out would let the
    // next frame's appends stack on stale items and overflow).
    E("    mov  qword [_gpu_draw_cnt], 0");
    E("    mov  qword [_gpu_stage_off], 0");
    E("    movaps xmm6, [rsp+0xA0]");
    E("    movaps xmm7, [rsp+0xB0]");
    E("    add  rsp, 0xC0");
    E("    pop  r15");
    E("    pop  r14");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    ret");
}
