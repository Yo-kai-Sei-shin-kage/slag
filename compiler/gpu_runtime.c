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
    E("_gpu_sampler:   resq 1");   // ID3D11SamplerState*
    E("_gpu_raster:    resq 1");   // ID3D11RasterizerState* (CULL_NONE)
    E("_gpu_pipeline:  resq 1");   // 1 once all pipeline objects created
    E("_gpu_stage:     resq 1");   // heap buffer of staged raw pcolor verts
    E("_gpu_convbuf:   resq 1");   // cached scratch for converted float verts (bulk-copied to WC vbuf)
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
    E("_gpu_dsstate:   resq 1");     // ID3D11DepthStencilState* (depth test+write, LESS)");
    E("_gpu_viewproj:  resq 1");     // ptr to 16 float32 (4x4 view-projection matrix) from the Slag camera; copied into the cbuf each frame");
    E("_gpu_stage_texw: resq 1");  // tex_w of staged triangles
    E("_gpu_stage_texh: resq 1");  // tex_h of staged triangles
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
    E("align 16");
    E("_gpu_vs_blob:  ; 1008 bytes DXBC (MVP, /Zpr row-major matrix packing to match row-major cbuf writes)");
    E("    db 68,88,66,67,145,109,57,254,242,183,104,78,209,79,152,154");
    E("    db 198,131,234,8,1,0,0,0,180,4,0,0,5,0,0,0");
    E("    db 52,0,0,0,60,1,0,0,172,1,0,0,32,2,0,0");
    E("    db 24,4,0,0,82,68,69,70,0,1,0,0,1,0,0,0");
    E("    db 96,0,0,0,1,0,0,0,60,0,0,0,0,5,254,255");
    E("    db 8,1,0,0,216,0,0,0,82,68,49,49,60,0,0,0");
    E("    db 24,0,0,0,32,0,0,0,40,0,0,0,36,0,0,0");
    E("    db 12,0,0,0,0,0,0,0,92,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,67,0,171,171,92,0,0,0");
    E("    db 1,0,0,0,120,0,0,0,64,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,160,0,0,0,0,0,0,0,64,0,0,0");
    E("    db 2,0,0,0,180,0,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,118,105,101,119");
    E("    db 112,114,111,106,0,102,108,111,97,116,52,120,52,0,171,171");
    E("    db 2,0,3,0,4,0,4,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 169,0,0,0,77,105,99,114,111,115,111,102,116,32,40,82");
    E("    db 41,32,72,76,83,76,32,83,104,97,100,101,114,32,67,111");
    E("    db 109,112,105,108,101,114,32,49,48,46,49,0,73,83,71,78");
    E("    db 104,0,0,0,3,0,0,0,8,0,0,0,80,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0");
    E("    db 7,7,0,0,89,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 3,0,0,0,1,0,0,0,3,3,0,0,98,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0");
    E("    db 7,7,0,0,80,79,83,73,84,73,79,78,0,84,69,88");
    E("    db 67,79,79,82,68,0,67,79,76,79,82,0,79,83,71,78");
    E("    db 108,0,0,0,3,0,0,0,8,0,0,0,80,0,0,0");
    E("    db 0,0,0,0,1,0,0,0,3,0,0,0,0,0,0,0");
    E("    db 15,0,0,0,92,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 3,0,0,0,1,0,0,0,3,12,0,0,101,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0");
    E("    db 7,8,0,0,83,86,95,80,79,83,73,84,73,79,78,0");
    E("    db 84,69,88,67,79,79,82,68,0,67,79,76,79,82,0,171");
    E("    db 83,72,69,88,240,1,0,0,80,0,1,0,124,0,0,0");
    E("    db 106,8,0,1,89,0,0,4,70,142,32,0,0,0,0,0");
    E("    db 4,0,0,0,95,0,0,3,114,16,16,0,0,0,0,0");
    E("    db 95,0,0,3,50,16,16,0,1,0,0,0,95,0,0,3");
    E("    db 114,16,16,0,2,0,0,0,103,0,0,4,242,32,16,0");
    E("    db 0,0,0,0,1,0,0,0,101,0,0,3,50,32,16,0");
    E("    db 1,0,0,0,101,0,0,3,114,32,16,0,2,0,0,0");
    E("    db 104,0,0,2,1,0,0,0,56,0,0,8,242,0,16,0");
    E("    db 0,0,0,0,86,21,16,0,0,0,0,0,70,142,32,0");
    E("    db 0,0,0,0,1,0,0,0,50,0,0,10,242,0,16,0");
    E("    db 0,0,0,0,6,16,16,0,0,0,0,0,70,142,32,0");
    E("    db 0,0,0,0,0,0,0,0,70,14,16,0,0,0,0,0");
    E("    db 50,0,0,10,242,0,16,0,0,0,0,0,166,26,16,0");
    E("    db 0,0,0,0,70,142,32,0,0,0,0,0,2,0,0,0");
    E("    db 70,14,16,0,0,0,0,0,0,0,0,8,242,0,16,0");
    E("    db 0,0,0,0,70,14,16,0,0,0,0,0,70,142,32,0");
    E("    db 0,0,0,0,3,0,0,0,54,0,0,5,242,32,16,0");
    E("    db 0,0,0,0,70,14,16,0,0,0,0,0,0,0,0,7");
    E("    db 18,0,16,0,0,0,0,0,58,0,16,0,0,0,0,0");
    E("    db 1,64,0,0,0,0,250,196,56,32,0,7,18,0,16,0");
    E("    db 0,0,0,0,10,0,16,0,0,0,0,0,1,64,0,0");
    E("    db 62,195,174,57,54,0,0,5,50,32,16,0,1,0,0,0");
    E("    db 70,16,16,0,1,0,0,0,0,0,0,8,34,0,16,0");
    E("    db 0,0,0,0,10,16,16,128,65,0,0,0,1,0,0,0");
    E("    db 1,64,0,0,0,0,128,63,56,0,0,7,18,0,16,0");
    E("    db 0,0,0,0,26,0,16,0,0,0,0,0,10,0,16,0");
    E("    db 0,0,0,0,0,0,0,11,226,0,16,0,0,0,0,0");
    E("    db 6,25,16,128,65,0,0,0,2,0,0,0,2,64,0,0");
    E("    db 0,0,0,0,151,150,22,63,171,170,42,63,196,195,67,63");
    E("    db 50,0,0,9,114,32,16,0,2,0,0,0,6,0,16,0");
    E("    db 0,0,0,0,150,7,16,0,0,0,0,0,70,18,16,0");
    E("    db 2,0,0,0,62,0,0,1,83,84,65,84,148,0,0,0");
    E("    db 13,0,0,0,1,0,0,0,0,0,0,0,6,0,0,0");
    E("    db 10,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0");
    E("_gpu_vs_blob_len equ 1204");
    E("align 16");
    E("_gpu_ps_blob:  ; 744 bytes DXBC");
    E("    db 68,88,66,67,95,133,16,255,50,221,11,173,49,44,13,208");
    E("    db 139,171,110,236,1,0,0,0,232,2,0,0,5,0,0,0");
    E("    db 52,0,0,0,232,0,0,0,92,1,0,0,144,1,0,0");
    E("    db 76,2,0,0,82,68,69,70,172,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,2,0,0,0,60,0,0,0,0,5,255,255");
    E("    db 0,1,0,0,132,0,0,0,82,68,49,49,60,0,0,0");
    E("    db 24,0,0,0,32,0,0,0,40,0,0,0,36,0,0,0");
    E("    db 12,0,0,0,0,0,0,0,124,0,0,0,3,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,128,0,0,0,2,0,0,0");
    E("    db 5,0,0,0,4,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 1,0,0,0,13,0,0,0,115,109,112,0,116,101,120,0");
    E("    db 77,105,99,114,111,115,111,102,116,32,40,82,41,32,72,76");
    E("    db 83,76,32,83,104,97,100,101,114,32,67,111,109,112,105,108");
    E("    db 101,114,32,49,48,46,49,0,73,83,71,78,108,0,0,0");
    E("    db 3,0,0,0,8,0,0,0,80,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,3,0,0,0,0,0,0,0,15,0,0,0");
    E("    db 92,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 1,0,0,0,3,3,0,0,101,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,2,0,0,0,7,7,0,0");
    E("    db 83,86,95,80,79,83,73,84,73,79,78,0,84,69,88,67");
    E("    db 79,79,82,68,0,67,79,76,79,82,0,171,79,83,71,78");
    E("    db 44,0,0,0,1,0,0,0,8,0,0,0,32,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0");
    E("    db 15,0,0,0,83,86,95,84,65,82,71,69,84,0,171,171");
    E("    db 83,72,69,88,180,0,0,0,80,0,0,0,45,0,0,0");
    E("    db 106,8,0,1,90,0,0,3,0,96,16,0,0,0,0,0");
    E("    db 88,24,0,4,0,112,16,0,0,0,0,0,85,85,0,0");
    E("    db 98,16,0,3,50,16,16,0,1,0,0,0,98,16,0,3");
    E("    db 114,16,16,0,2,0,0,0,101,0,0,3,242,32,16,0");
    E("    db 0,0,0,0,104,0,0,2,1,0,0,0,69,0,0,139");
    E("    db 194,0,0,128,67,85,21,0,114,0,16,0,0,0,0,0");
    E("    db 70,16,16,0,1,0,0,0,70,126,16,0,0,0,0,0");
    E("    db 0,96,16,0,0,0,0,0,56,0,0,7,114,32,16,0");
    E("    db 0,0,0,0,70,2,16,0,0,0,0,0,70,18,16,0");
    E("    db 2,0,0,0,54,0,0,5,130,32,16,0,0,0,0,0");
    E("    db 1,64,0,0,0,0,128,63,62,0,0,1,83,84,65,84");
    E("    db 148,0,0,0,4,0,0,0,1,0,0,0,0,0,0,0");
    E("    db 3,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0");
    E("_gpu_ps_blob_len equ 744");
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
    E("FMT_RGB32F         equ 6");
    E("FMT_RG32F          equ 16");
    E("FMT_BGRA8          equ 87");
    E("FILTER_POINT       equ 0");
    E("ADDR_CLAMP         equ 3");
    E("");
    E("; --- _slag_gpu_create_pipeline ---");
    E("_slag_gpu_create_pipeline:");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    push r12");
    E("    sub  rsp, 0x1B8");

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

    // Build 3 D3D11_INPUT_ELEMENT_DESC at [rsp+0x80] (32 bytes each).
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
    E("    mov  dword [rsp+0xC0+12], FMT_RGB32F");
    E("    mov  dword [rsp+0xC0+16], 0");
    E("    mov  dword [rsp+0xC0+20], 20");
    E("    mov  dword [rsp+0xC0+24], 0");
    E("    mov  dword [rsp+0xC0+28], 0");

    // CreateInputLayout(descs, 3, vs_blob, vs_len, &_gpu_layout) -- 0x58, 6 args
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x80]");
    E("    mov  r8,  3");
    E("    lea  r9,  [_gpu_vs_blob]");
    E("    mov  qword [rsp+0x20], _gpu_vs_blob_len");
    E("    lea  rax, [_gpu_layout]");
    E("    mov  [rsp+0x28], rax");
    E("    mov  rax, [rbx]");
    E("    call [rax + 0x58]");
    E("    test eax, eax");
    E("    jnz  .pl_fail");

    // Dynamic vertex buffer: sized for the full stage batch
    // (GPU_STAGE_CAP tris * 3 verts * 32B = 131072*96 = 12582912 bytes).
    E("    mov  dword [rsp+0x100+BUFDESC_BYTEWIDTH], 12582912");
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

    // Constant buffer: 64 bytes (float4x4 viewproj), dynamic.
    E("    mov  dword [rsp+0x100+BUFDESC_BYTEWIDTH], 64");
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

    // 512x512 BGRA texture: TEXTURE2D_DESC at [rsp+0x120] (44 bytes).
    E("    mov  dword [rsp+0x120+0], 512");
    E("    mov  dword [rsp+0x120+4], 512");
    E("    mov  dword [rsp+0x120+8], 1");
    E("    mov  dword [rsp+0x120+12], 1");
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

    // Rasterizer state: solid fill, back-face cull matching the CPU rasterizer.
    // The CPU pcolor cull keeps CCW-in-screen (y-down) triangles; the VS flips Y
    // for NDC, reversing winding to CW in clip space, so front=CW here
    // (FrontCounterClockwise=TRUE + CULL_BACK culls the same faces the CPU does).
    // RASTERIZER_DESC(40) at [rsp+0x150]: FillMode@0=SOLID(3), CullMode@4=BACK(3),
    // FrontCCW@8=1, DepthClipEnable@24=1, rest 0.
    E("    lea  rdi, [rsp+0x150]");
    E("    xor  eax, eax");
    E("    mov  ecx, 10");                   // 40 bytes / 4
    E("    rep  stosd");
    E("    mov  dword [rsp+0x150+0], 3");    // FILL_SOLID
    E("    mov  dword [rsp+0x150+4], 3");    // CULL_BACK
    E("    mov  dword [rsp+0x150+8], 1");    // FrontCounterClockwise = TRUE
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

    E("    call _slag_gpu_stage_init         ; alloc per-frame vertex stage");
    E("    mov  qword [_gpu_pipeline], 1");
    E("    jmp  .pl_ret");
    E(".pl_fail:");
    E("    mov  qword [_gpu_pipeline], 0");
    E(".pl_ret:");
    E("    add  rsp, 0x1B8");
    E("    pop  r12");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    ret");
}

// _slag_gpu_stage_init: HeapAlloc the per-frame vertex stage buffer once.
static void emit_gpu_stage_init(Codegen *cg) {
    E("GPU_STAGE_TRI    equ 192");        // raw bytes per staged triangle
    E("GPU_STAGE_CAP    equ 131072");     // max triangles per frame
    E("GPU_VTX_STRIDE   equ 32");         // float vertex: pos3 + uv2 + col3
    E("MAP_WR_DISCARD   equ 4");
    E("TOPOLOGY_TRILIST equ 4");
    E("");
    E("; --- _slag_gpu_stage_init (alloc _gpu_stage once) ---");
    E("_slag_gpu_stage_init:");
    E("    cmp  qword [_gpu_stage], 0");
    E("    jne  .si_done");
    E("    sub  rsp, 40");
    E("    call GetProcessHeap");
    E("    mov  rcx, rax");
    E("    xor  edx, edx");
    E("    mov  r8, GPU_STAGE_CAP * GPU_STAGE_TRI");
    E("    call HeapAlloc");
    E("    mov  [_gpu_stage], rax");
    E("    ; cached scratch for converted float verts (cap tris * 3 * 32B)");
    E("    call GetProcessHeap");
    E("    mov  rcx, rax");
    E("    xor  edx, edx");
    E("    mov  r8, GPU_STAGE_CAP * GPU_VTX_STRIDE * 3");
    E("    call HeapAlloc");
    E("    mov  [_gpu_convbuf], rax");
    E("    add  rsp, 40");
    E(".si_done:");
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

// _slag_gpu_present_frame: staged raw verts -> float vbuf, upload tex, set
// state, Draw, Present. No-op if nothing staged. Resets _gpu_stage_cnt.
static void emit_gpu_present_frame(Codegen *cg) {
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

    E("    mov  r14, [_gpu_stage_cnt]");
    E("    test r14, r14");
    E("    jz   .pf_ret");
    E("    mov  r15, [_gpu_context]");
    E("    test r15, r15");
    E("    jz   .pf_ret");
    E("    mov  rbx, [_window_primary_state]");
    E("    test rbx, rbx");
    E("    jz   .pf_ret");

    // Frame-latency pace: block on the waitable object until the swapchain is
    // ready for a new frame. This is a brief, GPU-paced wait at frame TOP (not a
    // hard Present stall) -- the CPU stays exactly one frame ahead, giving a
    // steady rate with no periodic multi-ms Present dips.
    E("    mov  rcx, [_gpu_waitable]");
    E("    test rcx, rcx");
    E("    jz   .pf_nowait");
    E("    mov  edx, 100               ; timeout 100ms (safety; normally signaled)");
    E("    sub  rsp, 32");
    E("    call WaitForSingleObject");
    E("    add  rsp, 32");
    E(".pf_nowait:");

    // Vertex count for Draw (needed whether or not we re-upload this frame).
    E("    mov  r13, r14");
    E("    imul r13, 3                 ; vertex count = ntri*3");

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
    E("    shl  rcx, 2                 ; qwords = verts * 32B / 8 = verts*4");
    E("    rep  movsq");

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
    E("    mov  ecx, 8                 ; 8 qwords = 64 bytes (4x4 float)");
    E("    rep  movsq");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_cbuf]");
    E("    xor  r8d, r8d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x78]           ; Unmap cbuf");
    E(".pf_after_cb:");

    // UpdateSubresource: texture from FTEX pixels (512x512 BGRA). Skip the
    // ~1MB/frame DMA + driver call when the texture pointer is unchanged from
    // the last upload -- static textures (the common case) upload exactly once.
    // NOTE: keyed on the tex_ptr, not its contents; a program mutating pixels
    // in place at the same pointer must change the pointer (or this stays stale).
    E("    mov  r11, [_gpu_stage_tex]");
    E("    cmp  r11, [_gpu_tex_uploaded]");
    E("    je   .pf_tex_done           ; same texture as last frame -> no re-upload");
    E("    mov  [rsp+0x20], r11        ; pSrcData");
    E("    mov  eax, [_gpu_stage_texw]");
    E("    shl  eax, 2                 ; RowPitch = texw*4");
    E("    mov  [rsp+0x28], eax");
    E("    mov  dword [rsp+0x30], 0    ; DepthPitch");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_tex]");
    E("    xor  r8d, r8d");
    E("    xor  r9, r9");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x180]          ; UpdateSubresource");
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
    // PSSetSamplers(0, 1, &sampler)
    E("    mov  rax, [_gpu_sampler]");
    E("    mov  [rsp+0x88], rax");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");
    E("    mov  r8d, 1");
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
    E("    call [rax + 0x108]");
    // ClearRenderTargetView(rtv, fog color {0.588,0.667,0.765,1}) so any pixel
    // not covered by geometry matches the fog/sky seam instead of black.
    E("    mov  dword [rsp+0x40], 0x3F168166");   // R 0.588");
    E("    mov  dword [rsp+0x44], 0x3F2AAAAB");   // G 0.667");
    E("    mov  dword [rsp+0x48], 0x3F43D70A");   // B 0.765");
    E("    mov  eax, 0x3F800000");
    E("    mov  [rsp+0x4C], eax");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_rtv]");
    E("    lea  r8, [rsp+0x40]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x190]");
    // ClearDepthStencilView(this, DSV, ClearFlags, Depth, Stencil) -- 5 params:
    //   1 this=rcx, 2 DSV=rdx, 3 ClearFlags=r8d, 4 Depth=xmm3, 5 Stencil=[rsp+0x20].
    E("    mov  byte [rsp+0x20], 0     ; Stencil (5th arg)");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_dsv]");
    E("    mov  r8d, 1                 ; D3D11_CLEAR_DEPTH");
    E("    mov  eax, 0x3F800000        ; 1.0f (far)");
    E("    movd xmm3, eax             ; Depth (4th arg slot)");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x1A8]          ; ClearDepthStencilView");
    // Draw(vertexcount, 0)
    E("    mov  rcx, r15");
    E("    mov  edx, r13d");
    E("    xor  r8d, r8d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x68]");
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
