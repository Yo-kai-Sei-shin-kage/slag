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
static void emit_gpu_physics(Codegen *cg);
static void emit_gpu_particles(Codegen *cg);
static void emit_gpu_dispmap(Codegen *cg);
static void emit_gpu_normmap(Codegen *cg);

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
    E("_gpu_sampler_wrap: resq 1"); // ID3D11SamplerState* (linear+WRAP, s0 override for tiled patch/terrain textures)
    E("_gpu_raster:    resq 1");   // ID3D11RasterizerState* (CULL_BACK, FrontCW = D3D default)
    E("_gpu_raster_wire: resq 1"); // ID3D11RasterizerState* (FILL_WIREFRAME, CULL_NONE) for tess debug view
    E("_gpu_raster_shadow: resq 1"); // ID3D11RasterizerState* (CULL_FRONT) for the shadow depth pass (kills grazing acne)
    E("_gpu_raster_none: resq 1");   // ID3D11RasterizerState* (SOLID, CULL_NONE) for particle billboards (no winding cull)");
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
    E("_gpu_dsstate_read: resq 1");  // ID3D11DepthStencilState* (depth test, NO write) for particles");
    E("_gpu_viewproj:  resq 1");     // ptr to 16 float32 (4x4 view-projection matrix) from the Slag camera; copied into the cbuf each frame");
    E("_gpu_stage_texw: resq 1");  // tex_w of staged triangles
    E("_gpu_stage_texh: resq 1");  // tex_h of staged triangles
    E("_gpu_clear_ptr:  resq 1");  // ptr to 4 x f32 (R,G,B,A) clear color from gpu.clear
    E("_gpu_clear_set:  resq 1");  // 1 once gpu.clear set a color; else fog fallback
    E("_gpu_blend:      resq 1");  // ID3D11BlendState* (straight-alpha), created at init
    E("_gpu_blend_mode: resq 1");  // 0=opaque (NULL state), 1=alpha blend (_gpu_blend)
    E("_gpu_shadowtex:  resq 1");  // ID3D11Texture2D* shadow depth map array (R32_TYPELESS 1024x1024 x SHADOW_MAX slices)");
    E("_gpu_shadow_dsv: resq 32");  // 32 ID3D11DepthStencilView* (D32), one per array slice (SHADOW_MAX)");
    E("_gpu_shadow_srv: resq 1");  // ID3D11ShaderResourceView* (R32_FLOAT Texture2DArray) sampled at t2");
    E("_gpu_lightproj:  resq 1");  // ptr to 16 f32 light view-projection (gpu.set_lightproj); 0 = no shadows");
    E("_gpu_lightdir:   resq 1");  // ptr to 3 f32 world light direction (gpu.set_lightproj arg2 tail)");
    E("_gpu_lights_ptr: resq 1");  // ptr to _gpu_lights_cnt Light structs (32B each) from gpu.set_lights");
    E("_gpu_lights_cnt: resq 1");  // active point-light count -> cbuffer lightCount @176");
    E("_gpu_lights_buf: resq 1");  // ID3D11Buffer* (structured, DYNAMIC) uploaded each frame");
    E("_gpu_lights_srv: resq 1");  // ID3D11ShaderResourceView* bound at PS t1");
    E("_gpu_lights_cap: resq 1");  // struct capacity of _gpu_lights_buf (grows on demand)");
    E("_gpu_lightvp_arr: resq 1");  // ptr to N row-major 4x4 lightVP matrices (gpu.set_lightproj_array); 0 = single/no");
    E("_gpu_lightvp_cnt: resq 1");  // count of lightVP matrices (= shadow-casting light count), capped SHADOW_MAX");
    E("_gpu_lightvp_buf: resq 1");  // ID3D11Buffer* (structured 64B stride, DYNAMIC) of lightVP matrices");
    E("_gpu_lightvp_srv: resq 1");  // ID3D11ShaderResourceView* bound at PS t5");
    E("_gpu_lightvp_cap: resq 1");  // matrix capacity of _gpu_lightvp_buf");
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
    // Tessellation path (opt-in per draw via fill_patch_gpu; bit 63 of an item's
    // vertexCount marks it as a patchlist draw). HS/DS distance-adaptive LOD.
    E("_gpu_tvs:        resq 1");     // ID3D11VertexShader*  (tess control-point pass-through)");
    E("_gpu_ths:        resq 1");     // ID3D11HullShader*");
    E("_gpu_tds:        resq 1");     // ID3D11DomainShader*");
    E("_gpu_dispmap:    resq 1");     // ID3D11Texture2D* R32F height map (DS t3)");
    E("_gpu_dispsrv:    resq 1");     // ID3D11ShaderResourceView* (DS t3)");
    E("_gpu_normmap:    resq 1");     // ID3D11Texture2D* RGBA normal map (DS t4)");
    E("_gpu_normsrv:    resq 1");     // ID3D11ShaderResourceView* (DS t4)");
    E("_gpu_tess_ptr:   resq 1");     // ptr to 8 f32 cbuffer tail @192 (tessScale,tessMax,dispScale,useNormMap,dispTexel.xy,pad2) from gpu.set_tess");
    E("_gpu_cur_patch:  resq 1");     // present loop: 1 if the current item is a patchlist (tessellated) draw");
    E("_gpu_patch_mark: resq 1");     // staging: bit63 when the current fill_*_gpu call is a patch draw, 0 otherwise (OR'd into the item's vertexCount)");
    E("_gpu_disp_ptr:   resq 1");     // Slag ptr of the height map last uploaded to _gpu_dispmap (skip re-upload when same)");
    E("_gpu_norm_ptr:   resq 1");     // Slag ptr of the normal map last uploaded to _gpu_normmap");
    E("_gpu_disp_dim:   resq 1");     // packed (w<<32)|h of the created disp texture (recreate when it changes)");
    E("_gpu_norm_dim:   resq 1");     // packed (w<<32)|h of the created normal texture");
    // physics compute state (gpu.physics_init/step); GPU-resident, no readback
    E("_gpu_cs_integrate: resq 1");   // ID3D11ComputeShader* (main)");
    E("_gpu_cs_clear:     resq 1");   // ID3D11ComputeShader* (ClearImpulses)");
    E("_gpu_cs_resolve:   resq 1");   // ID3D11ComputeShader* (ResolveBodyPairs)");
    E("_gpu_cs_apply:     resq 1");   // ID3D11ComputeShader* (ApplyImpulses)");
    E("_gpu_phys_bodies:  resq 1");   // ID3D11Buffer* Bodies (stride 96, UAV+SRV)");
    E("_gpu_phys_uav:     resq 1");   // Bodies UAV (u0)");
    E("_gpu_phys_srv:     resq 1");   // Bodies SRV (t0)");
    E("_gpu_phys_imp:     resq 1");   // ID3D11Buffer* ImpulseAccum (stride 32, UAV)");
    E("_gpu_phys_imp_uav: resq 1");   // ImpulseAccum UAV (u1)");
    E("_gpu_phys_cbuf:    resq 1");   // ID3D11Buffer* PhysicsConstants (192B, b0)");
    E("_gpu_phys_cap:     resq 1");   // body capacity of _gpu_phys_bodies (grows on demand)");
    E("_gpu_phys_ready:   resq 1");   // 1 once the 4 CS + buffers exist");
    E("_gpu_phys_bodies_ptr: resq 1"); // Slag bodies_ptr last uploaded (skip re-upload when same)");
    E("_gpu_phys_staging: resq 1");   // ID3D11Buffer* STAGING copy for gpu.physics_read (CPU_ACCESS_READ)");
    E("_gpu_phys_stage_cap: resq 1"); // body capacity of the staging buffer (recreate when it grows)");
    // particle compute state (particle.init/step/draw); GPU-resident dead-pool
    E("_gpu_cs_pemit:    resq 1");    // ID3D11ComputeShader* (particles.hlsl Emit)");
    E("_gpu_cs_psim:     resq 1");    // ID3D11ComputeShader* (particles.hlsl Simulate)");
    E("_gpu_part_buf:    resq 1");    // ID3D11Buffer* Particles (stride 64, UAV)");
    E("_gpu_part_uav:    resq 1");    // Particles UAV (u0)");
    E("_gpu_part_verts:  resq 1");    // ID3D11Buffer* render verts (stride 64, UAV+VERTEX)");
    E("_gpu_part_verts_uav: resq 1"); // render-verts UAV (u1, written by Simulate)");
    E("_gpu_part_cbuf:   resq 1");    // ID3D11Buffer* EmitterConstants (96B, b0)");
    E("_gpu_part_cap:    resq 1");    // particle capacity (grows on demand); verts = cap*6");
    E("_gpu_part_cnt:    resq 1");    // live particle count this frame (= last step's maxParticles); draw = cnt*6");
    E("_gpu_part_ready:  resq 1");    // 1 once the 2 CS + buffers exist");
    E("_gpu_part_staging:  resq 1");  // ID3D11Buffer* STAGING copy of render-verts for particle.read");
    E("_gpu_part_stage_cap: resq 1"); // vertex capacity of the staging buffer (recreate when it grows)");
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
    emit_gpu_physics(cg);
    emit_gpu_particles(cg);
    emit_gpu_dispmap(cg);
    emit_gpu_normmap(cg);
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

    // Hand-embedded shader blobs now use a script-generated include file so all changes to shaders will automatically
    // be added to the runtime without needing a runtime change. Will automatically be embedded during slag.exe recompile.
    #include "../shaders/gpu_shader_blobs.inc"
/*
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
    E("_gpu_ps_blob:  ; 7900 bytes DXBC (shadow-mapped ubershader PS, multi-light array + single)");
    E("    db 68,88,66,67,185,138,99,9,49,141,200,113,204,10,68,134");
    E("    db 24,0,189,250,1,0,0,0,220,30,0,0,5,0,0,0");
    E("    db 52,0,0,0,176,6,0,0,180,7,0,0,232,7,0,0");
    E("    db 64,30,0,0,82,68,69,70,116,6,0,0,3,0,0,0");
    E("    db 112,1,0,0,8,0,0,0,60,0,0,0,0,5,255,255");
    E("    db 0,1,0,0,76,6,0,0,82,68,49,49,60,0,0,0");
    E("    db 24,0,0,0,32,0,0,0,40,0,0,0,36,0,0,0");
    E("    db 12,0,0,0,0,0,0,0,60,1,0,0,3,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,64,1,0,0,3,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,71,1,0,0,3,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0");
    E("    db 1,0,0,0,3,0,0,0,78,1,0,0,2,0,0,0");
    E("    db 5,0,0,0,5,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 1,0,0,0,13,0,0,0,82,1,0,0,5,0,0,0");
    E("    db 6,0,0,0,1,0,0,0,32,0,0,0,1,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,89,1,0,0,2,0,0,0");
    E("    db 5,0,0,0,5,0,0,0,255,255,255,255,2,0,0,0");
    E("    db 1,0,0,0,13,0,0,0,99,1,0,0,5,0,0,0");
    E("    db 6,0,0,0,1,0,0,0,64,0,0,0,5,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,108,1,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,115,109,112,0,115,109,112,76");
    E("    db 105,110,0,115,109,112,67,109,112,0,116,101,120,0,108,105");
    E("    db 103,104,116,115,0,115,104,97,100,111,119,77,97,112,0,108");
    E("    db 105,103,104,116,86,80,115,0,67,0,171,171,108,1,0,0");
    E("    db 11,0,0,0,184,1,0,0,192,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,82,1,0,0,1,0,0,0,176,4,0,0");
    E("    db 32,0,0,0,0,0,0,0,3,0,0,0,99,1,0,0");
    E("    db 1,0,0,0,196,5,0,0,64,0,0,0,0,0,0,0");
    E("    db 3,0,0,0,112,3,0,0,0,0,0,0,64,0,0,0");
    E("    db 0,0,0,0,132,3,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,168,3,0,0");
    E("    db 64,0,0,0,12,0,0,0,0,0,0,0,184,3,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,220,3,0,0,76,0,0,0,4,0,0,0");
    E("    db 0,0,0,0,236,3,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,16,4,0,0");
    E("    db 80,0,0,0,4,0,0,0,0,0,0,0,236,3,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,28,4,0,0,84,0,0,0,8,0,0,0");
    E("    db 0,0,0,0,48,4,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,84,4,0,0");
    E("    db 92,0,0,0,4,0,0,0,0,0,0,0,236,3,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,91,4,0,0,96,0,0,0,64,0,0,0");
    E("    db 0,0,0,0,132,3,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,99,4,0,0");
    E("    db 160,0,0,0,12,0,0,0,0,0,0,0,184,3,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,106,4,0,0,172,0,0,0,4,0,0,0");
    E("    db 2,0,0,0,236,3,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,117,4,0,0");
    E("    db 176,0,0,0,4,0,0,0,2,0,0,0,132,4,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,168,4,0,0,180,0,0,0,12,0,0,0");
    E("    db 0,0,0,0,184,3,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,118,105,101,119");
    E("    db 112,114,111,106,0,102,108,111,97,116,52,120,52,0,171,171");
    E("    db 2,0,3,0,4,0,4,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 121,3,0,0,102,111,103,67,111,108,111,114,0,102,108,111");
    E("    db 97,116,51,0,1,0,3,0,1,0,3,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,177,3,0,0,102,111,103,83,116,97,114,116");
    E("    db 0,102,108,111,97,116,0,171,0,0,3,0,1,0,1,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,229,3,0,0,102,111,103,73");
    E("    db 110,118,82,97,110,103,101,0,105,110,118,84,101,120,68,105");
    E("    db 109,115,0,102,108,111,97,116,50,0,171,171,1,0,3,0");
    E("    db 1,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,39,4,0,0");
    E("    db 115,117,110,65,110,103,0,108,105,103,104,116,86,80,0,99");
    E("    db 97,109,80,111,115,0,115,104,97,100,111,119,80,97,115,115");
    E("    db 0,108,105,103,104,116,67,111,117,110,116,0,105,110,116,0");
    E("    db 0,0,2,0,1,0,1,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 128,4,0,0,95,108,112,97,100,0,171,171,216,4,0,0");
    E("    db 0,0,0,0,32,0,0,0,2,0,0,0,160,5,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,36,69,108,101,109,101,110,116,0,76,105,103");
    E("    db 104,116,0,112,111,115,0,171,1,0,3,0,1,0,3,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,177,3,0,0,99,111,108,111");
    E("    db 114,0,114,97,110,103,101,0,0,0,3,0,1,0,1,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,229,3,0,0,99,97,115,116");
    E("    db 83,104,97,100,111,119,115,0,0,0,2,0,1,0,1,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,128,4,0,0,231,4,0,0");
    E("    db 236,4,0,0,0,0,0,0,16,5,0,0,236,4,0,0");
    E("    db 12,0,0,0,22,5,0,0,28,5,0,0,24,0,0,0");
    E("    db 64,5,0,0,76,5,0,0,28,0,0,0,5,0,0,0");
    E("    db 1,0,8,0,0,0,4,0,112,5,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,225,4,0,0");
    E("    db 216,4,0,0,0,0,0,0,64,0,0,0,2,0,0,0");
    E("    db 40,6,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,76,105,103,104,116,86,80,0");
    E("    db 109,0,171,171,2,0,3,0,4,0,4,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,121,3,0,0,244,5,0,0,248,5,0,0");
    E("    db 0,0,0,0,5,0,0,0,1,0,16,0,0,0,1,0");
    E("    db 28,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,236,5,0,0,77,105,99,114,111,115,111,102");
    E("    db 116,32,40,82,41,32,72,76,83,76,32,83,104,97,100,101");
    E("    db 114,32,67,111,109,112,105,108,101,114,32,49,48,46,49,0");
    E("    db 73,83,71,78,252,0,0,0,9,0,0,0,8,0,0,0");
    E("    db 224,0,0,0,0,0,0,0,1,0,0,0,3,0,0,0");
    E("    db 0,0,0,0,15,0,0,0,236,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,1,0,0,0,3,3,0,0");
    E("    db 236,0,0,0,1,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 1,0,0,0,4,4,0,0,236,0,0,0,2,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,1,0,0,0,8,8,0,0");
    E("    db 245,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 2,0,0,0,15,15,0,0,236,0,0,0,3,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,3,0,0,0,7,7,0,0");
    E("    db 236,0,0,0,5,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 3,0,0,0,8,0,0,0,236,0,0,0,4,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,4,0,0,0,15,15,0,0");
    E("    db 236,0,0,0,6,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 5,0,0,0,7,7,0,0,83,86,95,80,79,83,73,84");
    E("    db 73,79,78,0,84,69,88,67,79,79,82,68,0,67,79,76");
    E("    db 79,82,0,171,79,83,71,78,44,0,0,0,1,0,0,0");
    E("    db 8,0,0,0,32,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 3,0,0,0,0,0,0,0,15,0,0,0,83,86,95,84");
    E("    db 65,82,71,69,84,0,171,171,83,72,69,88,80,22,0,0");
    E("    db 80,0,0,0,148,5,0,0,106,8,0,1,89,0,0,4");
    E("    db 70,142,32,0,0,0,0,0,12,0,0,0,90,0,0,3");
    E("    db 0,96,16,0,0,0,0,0,90,0,0,3,0,96,16,0");
    E("    db 1,0,0,0,90,8,0,3,0,96,16,0,2,0,0,0");
    E("    db 88,64,0,4,0,112,16,0,0,0,0,0,85,85,0,0");
    E("    db 162,0,0,4,0,112,16,0,1,0,0,0,32,0,0,0");
    E("    db 88,64,0,4,0,112,16,0,2,0,0,0,85,85,0,0");
    E("    db 162,0,0,4,0,112,16,0,5,0,0,0,64,0,0,0");
    E("    db 98,16,0,3,50,16,16,0,1,0,0,0,98,16,0,3");
    E("    db 66,16,16,0,1,0,0,0,98,16,0,3,130,16,16,0");
    E("    db 1,0,0,0,98,16,0,3,242,16,16,0,2,0,0,0");
    E("    db 98,16,0,3,114,16,16,0,3,0,0,0,98,16,0,3");
    E("    db 242,16,16,0,4,0,0,0,98,16,0,3,114,16,16,0");
    E("    db 5,0,0,0,101,0,0,3,242,32,16,0,0,0,0,0");
    E("    db 104,0,0,2,12,0,0,0,49,0,0,7,18,0,16,0");
    E("    db 0,0,0,0,42,16,16,0,1,0,0,0,1,64,0,0");
    E("    db 0,0,0,0,31,0,4,3,10,0,16,0,0,0,0,0");
    E("    db 56,0,0,10,114,0,16,0,0,0,0,0,70,18,16,0");
    E("    db 1,0,0,0,2,64,0,0,0,0,128,63,0,0,128,63");
    E("    db 0,0,128,191,0,0,0,0,69,0,0,139,2,2,0,128");
    E("    db 67,85,21,0,18,0,16,0,0,0,0,0,70,2,16,0");
    E("    db 0,0,0,0,70,126,16,0,0,0,0,0,0,96,16,0");
    E("    db 1,0,0,0,0,0,0,7,18,0,16,0,0,0,0,0");
    E("    db 10,0,16,0,0,0,0,0,1,64,0,0,31,133,235,190");
    E("    db 56,32,0,7,18,0,16,0,0,0,0,0,10,0,16,0");
    E("    db 0,0,0,0,1,64,0,0,254,255,71,65,50,0,0,9");
    E("    db 34,0,16,0,0,0,0,0,10,0,16,0,0,0,0,0");
    E("    db 1,64,0,0,0,0,0,192,1,64,0,0,0,0,64,64");
    E("    db 56,0,0,7,18,0,16,0,0,0,0,0,10,0,16,0");
    E("    db 0,0,0,0,10,0,16,0,0,0,0,0,56,0,0,7");
    E("    db 18,0,16,0,0,0,0,0,10,0,16,0,0,0,0,0");
    E("    db 26,0,16,0,0,0,0,0,56,0,0,7,130,32,16,0");
    E("    db 0,0,0,0,10,0,16,0,0,0,0,0,58,16,16,0");
    E("    db 2,0,0,0,54,0,0,5,114,32,16,0,0,0,0,0");
    E("    db 70,18,16,0,2,0,0,0,62,0,0,1,21,0,0,1");
    E("    db 69,0,0,139,2,2,0,128,67,85,21,0,114,0,16,0");
    E("    db 0,0,0,0,70,18,16,0,1,0,0,0,70,126,16,0");
    E("    db 0,0,0,0,0,96,16,0,0,0,0,0,56,0,0,7");
    E("    db 114,0,16,0,0,0,0,0,70,2,16,0,0,0,0,0");
    E("    db 70,18,16,0,2,0,0,0,49,0,0,7,130,0,16,0");
    E("    db 0,0,0,0,1,64,0,0,0,0,192,63,58,16,16,0");
    E("    db 1,0,0,0,49,0,0,7,18,0,16,0,1,0,0,0");
    E("    db 58,16,16,0,1,0,0,0,1,64,0,0,0,0,32,64");
    E("    db 1,0,0,7,130,0,16,0,0,0,0,0,58,0,16,0");
    E("    db 0,0,0,0,10,0,16,0,1,0,0,0,31,0,4,3");
    E("    db 58,0,16,0,0,0,0,0,16,0,0,7,130,0,16,0");
    E("    db 0,0,0,0,70,18,16,0,5,0,0,0,70,18,16,0");
    E("    db 5,0,0,0,68,0,0,5,130,0,16,0,0,0,0,0");
    E("    db 58,0,16,0,0,0,0,0,56,0,0,7,114,0,16,0");
    E("    db 1,0,0,0,246,15,16,0,0,0,0,0,70,18,16,0");
    E("    db 5,0,0,0,14,0,0,7,114,0,16,0,2,0,0,0");
    E("    db 70,18,16,0,4,0,0,0,246,31,16,0,4,0,0,0");
    E("    db 56,0,0,10,98,0,16,0,3,0,0,0,6,1,16,0");
    E("    db 2,0,0,0,2,64,0,0,0,0,0,0,0,0,0,63");
    E("    db 0,0,0,63,0,0,0,0,54,0,0,6,146,0,16,0");
    E("    db 3,0,0,0,166,10,16,128,65,0,0,0,3,0,0,0");
    E("    db 0,0,0,10,50,0,16,0,2,0,0,0,22,5,16,0");
    E("    db 3,0,0,0,2,64,0,0,0,0,0,63,0,0,0,63");
    E("    db 0,0,0,0,0,0,0,0,49,0,0,8,130,0,16,0");
    E("    db 0,0,0,0,1,64,0,0,0,0,192,63,58,128,32,0");
    E("    db 0,0,0,0,10,0,0,0,29,0,0,7,130,0,16,0");
    E("    db 1,0,0,0,1,64,0,0,0,0,128,63,42,0,16,0");
    E("    db 2,0,0,0,49,0,0,10,50,0,16,0,4,0,0,0");
    E("    db 70,0,16,0,2,0,0,0,2,64,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,49,0,0,10");
    E("    db 50,0,16,0,2,0,0,0,2,64,0,0,0,0,128,63");
    E("    db 0,0,128,63,0,0,0,0,0,0,0,0,70,0,16,0");
    E("    db 2,0,0,0,60,0,0,7,18,0,16,0,2,0,0,0");
    E("    db 10,0,16,0,2,0,0,0,10,0,16,0,4,0,0,0");
    E("    db 60,0,0,7,18,0,16,0,2,0,0,0,26,0,16,0");
    E("    db 4,0,0,0,10,0,16,0,2,0,0,0,60,0,0,7");
    E("    db 18,0,16,0,2,0,0,0,26,0,16,0,2,0,0,0");
    E("    db 10,0,16,0,2,0,0,0,0,0,0,7,34,0,16,0");
    E("    db 2,0,0,0,42,0,16,0,2,0,0,0,1,64,0,0");
    E("    db 111,18,3,186,0,0,0,10,50,0,16,0,4,0,0,0");
    E("    db 214,5,16,0,3,0,0,0,2,64,0,0,0,128,255,62");
    E("    db 0,128,255,62,0,0,0,0,0,0,0,0,54,0,0,5");
    E("    db 66,0,16,0,4,0,0,0,1,64,0,0,0,0,0,0");
    E("    db 71,0,0,141,2,2,0,128,67,85,21,0,66,0,16,0");
    E("    db 2,0,0,0,70,2,16,0,4,0,0,0,6,112,16,0");
    E("    db 2,0,0,0,0,96,16,0,2,0,0,0,26,0,16,0");
    E("    db 2,0,0,0,0,0,0,10,50,0,16,0,4,0,0,0");
    E("    db 214,5,16,0,3,0,0,0,2,64,0,0,0,0,0,63");
    E("    db 0,128,255,62,0,0,0,0,0,0,0,0,54,0,0,5");
    E("    db 66,0,16,0,4,0,0,0,1,64,0,0,0,0,0,0");
    E("    db 71,0,0,141,2,2,0,128,67,85,21,0,130,0,16,0");
    E("    db 2,0,0,0,70,2,16,0,4,0,0,0,6,112,16,0");
    E("    db 2,0,0,0,0,96,16,0,2,0,0,0,26,0,16,0");
    E("    db 2,0,0,0,0,0,0,7,66,0,16,0,2,0,0,0");
    E("    db 58,0,16,0,2,0,0,0,42,0,16,0,2,0,0,0");
    E("    db 0,0,0,10,50,0,16,0,4,0,0,0,214,5,16,0");
    E("    db 3,0,0,0,2,64,0,0,0,64,0,63,0,128,255,62");
    E("    db 0,0,0,0,0,0,0,0,54,0,0,5,66,0,16,0");
    E("    db 4,0,0,0,1,64,0,0,0,0,0,0,71,0,0,141");
    E("    db 2,2,0,128,67,85,21,0,130,0,16,0,2,0,0,0");
    E("    db 70,2,16,0,4,0,0,0,6,112,16,0,2,0,0,0");
    E("    db 0,96,16,0,2,0,0,0,26,0,16,0,2,0,0,0");
    E("    db 0,0,0,7,66,0,16,0,2,0,0,0,58,0,16,0");
    E("    db 2,0,0,0,42,0,16,0,2,0,0,0,0,0,0,10");
    E("    db 50,0,16,0,4,0,0,0,214,5,16,0,3,0,0,0");
    E("    db 2,64,0,0,0,128,255,62,0,0,0,63,0,0,0,0");
    E("    db 0,0,0,0,54,0,0,5,66,0,16,0,4,0,0,0");
    E("    db 1,64,0,0,0,0,0,0,71,0,0,141,2,2,0,128");
    E("    db 67,85,21,0,130,0,16,0,2,0,0,0,70,2,16,0");
    E("    db 4,0,0,0,6,112,16,0,2,0,0,0,0,96,16,0");
    E("    db 2,0,0,0,26,0,16,0,2,0,0,0,0,0,0,7");
    E("    db 66,0,16,0,2,0,0,0,58,0,16,0,2,0,0,0");
    E("    db 42,0,16,0,2,0,0,0,50,0,0,15,50,0,16,0");
    E("    db 4,0,0,0,150,5,16,0,3,0,0,0,2,64,0,0");
    E("    db 0,0,128,63,0,0,128,191,0,0,0,0,0,0,0,0");
    E("    db 2,64,0,0,0,0,0,63,0,0,0,63,0,0,0,0");
    E("    db 0,0,0,0,54,0,0,5,66,0,16,0,4,0,0,0");
    E("    db 1,64,0,0,0,0,0,0,71,0,0,141,2,2,0,128");
    E("    db 67,85,21,0,130,0,16,0,2,0,0,0,70,2,16,0");
    E("    db 4,0,0,0,6,112,16,0,2,0,0,0,0,96,16,0");
    E("    db 2,0,0,0,26,0,16,0,2,0,0,0,0,0,0,7");
    E("    db 66,0,16,0,2,0,0,0,58,0,16,0,2,0,0,0");
    E("    db 42,0,16,0,2,0,0,0,0,0,0,10,50,0,16,0");
    E("    db 4,0,0,0,214,5,16,0,3,0,0,0,2,64,0,0");
    E("    db 0,64,0,63,0,0,0,63,0,0,0,0,0,0,0,0");
    E("    db 54,0,0,5,66,0,16,0,4,0,0,0,1,64,0,0");
    E("    db 0,0,0,0,71,0,0,141,2,2,0,128,67,85,21,0");
    E("    db 130,0,16,0,2,0,0,0,70,2,16,0,4,0,0,0");
    E("    db 6,112,16,0,2,0,0,0,0,96,16,0,2,0,0,0");
    E("    db 26,0,16,0,2,0,0,0,0,0,0,7,66,0,16,0");
    E("    db 2,0,0,0,58,0,16,0,2,0,0,0,42,0,16,0");
    E("    db 2,0,0,0,0,0,0,10,50,0,16,0,4,0,0,0");
    E("    db 214,5,16,0,3,0,0,0,2,64,0,0,0,128,255,62");
    E("    db 0,64,0,63,0,0,0,0,0,0,0,0,54,0,0,5");
    E("    db 66,0,16,0,4,0,0,0,1,64,0,0,0,0,0,0");
    E("    db 71,0,0,141,2,2,0,128,67,85,21,0,130,0,16,0");
    E("    db 2,0,0,0,70,2,16,0,4,0,0,0,6,112,16,0");
    E("    db 2,0,0,0,0,96,16,0,2,0,0,0,26,0,16,0");
    E("    db 2,0,0,0,0,0,0,7,66,0,16,0,2,0,0,0");
    E("    db 58,0,16,0,2,0,0,0,42,0,16,0,2,0,0,0");
    E("    db 0,0,0,10,50,0,16,0,4,0,0,0,214,5,16,0");
    E("    db 3,0,0,0,2,64,0,0,0,0,0,63,0,64,0,63");
    E("    db 0,0,0,0,0,0,0,0,54,0,0,5,66,0,16,0");
    E("    db 4,0,0,0,1,64,0,0,0,0,0,0,71,0,0,141");
    E("    db 2,2,0,128,67,85,21,0,130,0,16,0,2,0,0,0");
    E("    db 70,2,16,0,4,0,0,0,6,112,16,0,2,0,0,0");
    E("    db 0,96,16,0,2,0,0,0,26,0,16,0,2,0,0,0");
    E("    db 0,0,0,7,66,0,16,0,2,0,0,0,58,0,16,0");
    E("    db 2,0,0,0,42,0,16,0,2,0,0,0,0,0,0,10");
    E("    db 50,0,16,0,3,0,0,0,214,5,16,0,3,0,0,0");
    E("    db 2,64,0,0,0,64,0,63,0,64,0,63,0,0,0,0");
    E("    db 0,0,0,0,54,0,0,8,194,0,16,0,3,0,0,0");
    E("    db 2,64,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,128,63,71,0,0,141,2,2,0,128,67,85,21,0");
    E("    db 34,0,16,0,2,0,0,0,70,2,16,0,3,0,0,0");
    E("    db 6,112,16,0,2,0,0,0,0,96,16,0,2,0,0,0");
    E("    db 26,0,16,0,2,0,0,0,0,0,0,7,34,0,16,0");
    E("    db 2,0,0,0,26,0,16,0,2,0,0,0,42,0,16,0");
    E("    db 2,0,0,0,56,0,0,7,34,0,16,0,2,0,0,0");
    E("    db 26,0,16,0,2,0,0,0,1,64,0,0,57,142,227,61");
    E("    db 54,0,0,5,114,0,16,0,3,0,0,0,70,18,16,0");
    E("    db 3,0,0,0,54,0,0,8,114,0,16,0,4,0,0,0");
    E("    db 2,64,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,54,0,0,5,66,0,16,0,2,0,0,0");
    E("    db 1,64,0,0,0,0,0,0,48,0,0,1,33,0,0,8");
    E("    db 130,0,16,0,2,0,0,0,42,0,16,0,2,0,0,0");
    E("    db 10,128,32,0,0,0,0,0,11,0,0,0,3,0,4,3");
    E("    db 58,0,16,0,2,0,0,0,167,0,0,139,2,3,1,128");
    E("    db 131,153,25,0,242,0,16,0,5,0,0,0,42,0,16,0");
    E("    db 2,0,0,0,1,64,0,0,0,0,0,0,70,126,16,0");
    E("    db 1,0,0,0,167,0,0,139,2,3,1,128,131,153,25,0");
    E("    db 242,0,16,0,6,0,0,0,42,0,16,0,2,0,0,0");
    E("    db 1,64,0,0,16,0,0,0,38,125,16,0,1,0,0,0");
    E("    db 0,0,0,8,114,0,16,0,5,0,0,0,70,2,16,0");
    E("    db 5,0,0,0,70,18,16,128,65,0,0,0,3,0,0,0");
    E("    db 16,0,0,7,130,0,16,0,2,0,0,0,70,2,16,0");
    E("    db 5,0,0,0,70,2,16,0,5,0,0,0,75,0,0,5");
    E("    db 130,0,16,0,2,0,0,0,58,0,16,0,2,0,0,0");
    E("    db 52,0,0,7,130,0,16,0,4,0,0,0,58,0,16,0");
    E("    db 2,0,0,0,1,64,0,0,23,183,209,56,14,0,0,7");
    E("    db 114,0,16,0,5,0,0,0,70,2,16,0,5,0,0,0");
    E("    db 246,15,16,0,4,0,0,0,16,0,0,7,130,0,16,0");
    E("    db 4,0,0,0,70,2,16,0,1,0,0,0,70,2,16,0");
    E("    db 5,0,0,0,52,0,0,7,130,0,16,0,4,0,0,0");
    E("    db 58,0,16,0,4,0,0,0,1,64,0,0,0,0,0,0");
    E("    db 49,0,0,7,18,0,16,0,5,0,0,0,1,64,0,0");
    E("    db 0,0,0,0,10,0,16,0,6,0,0,0,14,0,0,7");
    E("    db 130,0,16,0,2,0,0,0,58,0,16,0,2,0,0,0");
    E("    db 10,0,16,0,6,0,0,0,50,0,0,9,130,0,16,0");
    E("    db 2,0,0,0,58,0,16,0,2,0,0,0,58,0,16,0");
    E("    db 2,0,0,0,1,64,0,0,0,0,128,63,14,0,0,10");
    E("    db 130,0,16,0,2,0,0,0,2,64,0,0,0,0,128,63");
    E("    db 0,0,128,63,0,0,128,63,0,0,128,63,58,0,16,0");
    E("    db 2,0,0,0,55,0,0,9,130,0,16,0,2,0,0,0");
    E("    db 10,0,16,0,5,0,0,0,58,0,16,0,2,0,0,0");
    E("    db 1,64,0,0,0,0,128,63,31,0,4,3,58,0,16,0");
    E("    db 6,0,0,0,31,0,4,3,58,0,16,0,0,0,0,0");
    E("    db 167,0,0,139,2,3,2,128,131,153,25,0,242,0,16,0");
    E("    db 7,0,0,0,42,0,16,0,2,0,0,0,1,64,0,0");
    E("    db 0,0,0,0,70,126,16,0,5,0,0,0,167,0,0,139");
    E("    db 2,3,2,128,131,153,25,0,242,0,16,0,8,0,0,0");
    E("    db 42,0,16,0,2,0,0,0,1,64,0,0,16,0,0,0");
    E("    db 70,126,16,0,5,0,0,0,167,0,0,139,2,3,2,128");
    E("    db 131,153,25,0,242,0,16,0,9,0,0,0,42,0,16,0");
    E("    db 2,0,0,0,1,64,0,0,32,0,0,0,70,126,16,0");
    E("    db 5,0,0,0,167,0,0,139,2,3,2,128,131,153,25,0");
    E("    db 242,0,16,0,10,0,0,0,42,0,16,0,2,0,0,0");
    E("    db 1,64,0,0,48,0,0,0,134,119,16,0,5,0,0,0");
    E("    db 54,0,0,5,18,0,16,0,11,0,0,0,42,0,16,0");
    E("    db 7,0,0,0,54,0,0,5,34,0,16,0,11,0,0,0");
    E("    db 42,0,16,0,8,0,0,0,54,0,0,5,66,0,16,0");
    E("    db 11,0,0,0,42,0,16,0,9,0,0,0,54,0,0,5");
    E("    db 130,0,16,0,11,0,0,0,26,0,16,0,10,0,0,0");
    E("    db 17,0,0,7,18,0,16,0,5,0,0,0,70,14,16,0");
    E("    db 3,0,0,0,70,14,16,0,11,0,0,0,54,0,0,5");
    E("    db 18,0,16,0,11,0,0,0,58,0,16,0,7,0,0,0");
    E("    db 54,0,0,5,34,0,16,0,11,0,0,0,58,0,16,0");
    E("    db 8,0,0,0,54,0,0,5,66,0,16,0,11,0,0,0");
    E("    db 58,0,16,0,9,0,0,0,54,0,0,5,130,0,16,0");
    E("    db 11,0,0,0,42,0,16,0,10,0,0,0,17,0,0,7");
    E("    db 34,0,16,0,5,0,0,0,70,14,16,0,3,0,0,0");
    E("    db 70,14,16,0,11,0,0,0,14,0,0,7,18,0,16,0");
    E("    db 5,0,0,0,10,0,16,0,5,0,0,0,26,0,16,0");
    E("    db 5,0,0,0,29,0,0,7,66,0,16,0,5,0,0,0");
    E("    db 1,64,0,0,0,0,128,63,10,0,16,0,5,0,0,0");
    E("    db 31,0,4,3,42,0,16,0,5,0,0,0,54,0,0,5");
    E("    db 18,0,16,0,11,0,0,0,10,0,16,0,7,0,0,0");
    E("    db 54,0,0,5,34,0,16,0,11,0,0,0,10,0,16,0");
    E("    db 8,0,0,0,54,0,0,5,66,0,16,0,11,0,0,0");
    E("    db 10,0,16,0,9,0,0,0,54,0,0,5,130,0,16,0");
    E("    db 11,0,0,0,10,0,16,0,10,0,0,0,17,0,0,7");
    E("    db 66,0,16,0,5,0,0,0,70,14,16,0,3,0,0,0");
    E("    db 70,14,16,0,11,0,0,0,54,0,0,5,18,0,16,0");
    E("    db 10,0,0,0,26,0,16,0,7,0,0,0,54,0,0,5");
    E("    db 34,0,16,0,10,0,0,0,26,0,16,0,8,0,0,0");
    E("    db 54,0,0,5,66,0,16,0,10,0,0,0,26,0,16,0");
    E("    db 9,0,0,0,17,0,0,7,130,0,16,0,6,0,0,0");
    E("    db 70,14,16,0,3,0,0,0,70,14,16,0,10,0,0,0");
    E("    db 14,0,0,7,66,0,16,0,5,0,0,0,42,0,16,0");
    E("    db 5,0,0,0,26,0,16,0,5,0,0,0,56,0,0,7");
    E("    db 18,0,16,0,7,0,0,0,42,0,16,0,5,0,0,0");
    E("    db 1,64,0,0,0,0,0,63,14,0,0,7,34,0,16,0");
    E("    db 5,0,0,0,58,0,16,0,6,0,0,0,26,0,16,0");
    E("    db 5,0,0,0,56,0,0,7,34,0,16,0,5,0,0,0");
    E("    db 26,0,16,0,5,0,0,0,1,64,0,0,0,0,0,63");
    E("    db 54,0,0,6,34,0,16,0,7,0,0,0,26,0,16,128");
    E("    db 65,0,0,0,5,0,0,0,0,0,0,10,98,0,16,0");
    E("    db 5,0,0,0,6,1,16,0,7,0,0,0,2,64,0,0");
    E("    db 0,0,0,0,0,0,0,63,0,0,0,63,0,0,0,0");
    E("    db 49,0,0,10,194,0,16,0,7,0,0,0,86,9,16,0");
    E("    db 5,0,0,0,2,64,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,49,0,0,10,98,0,16,0");
    E("    db 5,0,0,0,2,64,0,0,0,0,0,0,0,0,128,63");
    E("    db 0,0,128,63,0,0,0,0,86,6,16,0,5,0,0,0");
    E("    db 60,0,0,7,34,0,16,0,5,0,0,0,26,0,16,0");
    E("    db 5,0,0,0,42,0,16,0,7,0,0,0,60,0,0,7");
    E("    db 34,0,16,0,5,0,0,0,58,0,16,0,7,0,0,0");
    E("    db 26,0,16,0,5,0,0,0,60,0,0,7,34,0,16,0");
    E("    db 5,0,0,0,42,0,16,0,5,0,0,0,26,0,16,0");
    E("    db 5,0,0,0,31,0,0,3,26,0,16,0,5,0,0,0");
    E("    db 43,0,0,5,66,0,16,0,8,0,0,0,42,0,16,0");
    E("    db 2,0,0,0,0,0,0,7,18,0,16,0,5,0,0,0");
    E("    db 10,0,16,0,5,0,0,0,1,64,0,0,111,18,3,186");
    E("    db 0,0,0,10,50,0,16,0,8,0,0,0,70,0,16,0");
    E("    db 7,0,0,0,2,64,0,0,0,128,255,62,0,128,255,62");
    E("    db 0,0,0,0,0,0,0,0,71,0,0,141,2,2,0,128");
    E("    db 67,85,21,0,34,0,16,0,5,0,0,0,70,2,16,0");
    E("    db 8,0,0,0,6,112,16,0,2,0,0,0,0,96,16,0");
    E("    db 2,0,0,0,10,0,16,0,5,0,0,0,0,0,0,10");
    E("    db 50,0,16,0,8,0,0,0,70,0,16,0,7,0,0,0");
    E("    db 2,64,0,0,0,0,0,63,0,128,255,62,0,0,0,0");
    E("    db 0,0,0,0,71,0,0,141,2,2,0,128,67,85,21,0");
    E("    db 66,0,16,0,5,0,0,0,70,2,16,0,8,0,0,0");
    E("    db 6,112,16,0,2,0,0,0,0,96,16,0,2,0,0,0");
    E("    db 10,0,16,0,5,0,0,0,0,0,0,7,34,0,16,0");
    E("    db 5,0,0,0,42,0,16,0,5,0,0,0,26,0,16,0");
    E("    db 5,0,0,0,0,0,0,10,50,0,16,0,8,0,0,0");
    E("    db 70,0,16,0,7,0,0,0,2,64,0,0,0,64,0,63");
    E("    db 0,128,255,62,0,0,0,0,0,0,0,0,71,0,0,141");
    E("    db 2,2,0,128,67,85,21,0,66,0,16,0,5,0,0,0");
    E("    db 70,2,16,0,8,0,0,0,6,112,16,0,2,0,0,0");
    E("    db 0,96,16,0,2,0,0,0,10,0,16,0,5,0,0,0");
    E("    db 0,0,0,7,34,0,16,0,5,0,0,0,42,0,16,0");
    E("    db 5,0,0,0,26,0,16,0,5,0,0,0,0,0,0,10");
    E("    db 50,0,16,0,8,0,0,0,70,0,16,0,7,0,0,0");
    E("    db 2,64,0,0,0,128,255,62,0,0,0,63,0,0,0,0");
    E("    db 0,0,0,0,71,0,0,141,2,2,0,128,67,85,21,0");
    E("    db 66,0,16,0,5,0,0,0,70,2,16,0,8,0,0,0");
    E("    db 6,112,16,0,2,0,0,0,0,96,16,0,2,0,0,0");
    E("    db 10,0,16,0,5,0,0,0,0,0,0,7,34,0,16,0");
    E("    db 5,0,0,0,42,0,16,0,5,0,0,0,26,0,16,0");
    E("    db 5,0,0,0,0,0,0,10,50,0,16,0,8,0,0,0");
    E("    db 70,0,16,0,7,0,0,0,2,64,0,0,0,0,0,63");
    E("    db 0,0,0,63,0,0,0,0,0,0,0,0,71,0,0,141");
    E("    db 2,2,0,128,67,85,21,0,66,0,16,0,5,0,0,0");
    E("    db 70,2,16,0,8,0,0,0,6,112,16,0,2,0,0,0");
    E("    db 0,96,16,0,2,0,0,0,10,0,16,0,5,0,0,0");
    E("    db 0,0,0,7,34,0,16,0,5,0,0,0,42,0,16,0");
    E("    db 5,0,0,0,26,0,16,0,5,0,0,0,0,0,0,10");
    E("    db 50,0,16,0,8,0,0,0,70,0,16,0,7,0,0,0");
    E("    db 2,64,0,0,0,64,0,63,0,0,0,63,0,0,0,0");
    E("    db 0,0,0,0,71,0,0,141,2,2,0,128,67,85,21,0");
    E("    db 66,0,16,0,5,0,0,0,70,2,16,0,8,0,0,0");
    E("    db 6,112,16,0,2,0,0,0,0,96,16,0,2,0,0,0");
    E("    db 10,0,16,0,5,0,0,0,0,0,0,7,34,0,16,0");
    E("    db 5,0,0,0,42,0,16,0,5,0,0,0,26,0,16,0");
    E("    db 5,0,0,0,0,0,0,10,50,0,16,0,8,0,0,0");
    E("    db 70,0,16,0,7,0,0,0,2,64,0,0,0,128,255,62");
    E("    db 0,64,0,63,0,0,0,0,0,0,0,0,71,0,0,141");
    E("    db 2,2,0,128,67,85,21,0,66,0,16,0,5,0,0,0");
    E("    db 70,2,16,0,8,0,0,0,6,112,16,0,2,0,0,0");
    E("    db 0,96,16,0,2,0,0,0,10,0,16,0,5,0,0,0");
    E("    db 0,0,0,7,34,0,16,0,5,0,0,0,42,0,16,0");
    E("    db 5,0,0,0,26,0,16,0,5,0,0,0,0,0,0,10");
    E("    db 50,0,16,0,8,0,0,0,70,0,16,0,7,0,0,0");
    E("    db 2,64,0,0,0,0,0,63,0,64,0,63,0,0,0,0");
    E("    db 0,0,0,0,71,0,0,141,2,2,0,128,67,85,21,0");
    E("    db 66,0,16,0,5,0,0,0,70,2,16,0,8,0,0,0");
    E("    db 6,112,16,0,2,0,0,0,0,96,16,0,2,0,0,0");
    E("    db 10,0,16,0,5,0,0,0,0,0,0,7,34,0,16,0");
    E("    db 5,0,0,0,42,0,16,0,5,0,0,0,26,0,16,0");
    E("    db 5,0,0,0,0,0,0,10,50,0,16,0,8,0,0,0");
    E("    db 70,0,16,0,7,0,0,0,2,64,0,0,0,64,0,63");
    E("    db 0,64,0,63,0,0,0,0,0,0,0,0,71,0,0,141");
    E("    db 2,2,0,128,67,85,21,0,18,0,16,0,5,0,0,0");
    E("    db 70,2,16,0,8,0,0,0,6,112,16,0,2,0,0,0");
    E("    db 0,96,16,0,2,0,0,0,10,0,16,0,5,0,0,0");
    E("    db 0,0,0,7,18,0,16,0,5,0,0,0,10,0,16,0");
    E("    db 5,0,0,0,26,0,16,0,5,0,0,0,56,0,0,7");
    E("    db 18,0,16,0,5,0,0,0,10,0,16,0,5,0,0,0");
    E("    db 1,64,0,0,57,142,227,61,18,0,0,1,54,0,0,5");
    E("    db 18,0,16,0,5,0,0,0,1,64,0,0,0,0,128,63");
    E("    db 21,0,0,1,18,0,0,1,54,0,0,5,18,0,16,0");
    E("    db 5,0,0,0,1,64,0,0,0,0,128,63,21,0,0,1");
    E("    db 18,0,0,1,31,0,4,3,58,0,16,0,1,0,0,0");
    E("    db 31,0,0,3,10,0,16,0,2,0,0,0,54,0,0,5");
    E("    db 18,0,16,0,5,0,0,0,26,0,16,0,2,0,0,0");
    E("    db 18,0,0,1,54,0,0,5,18,0,16,0,5,0,0,0");
    E("    db 1,64,0,0,0,0,128,63,21,0,0,1,18,0,0,1");
    E("    db 54,0,0,5,18,0,16,0,5,0,0,0,1,64,0,0");
    E("    db 0,0,128,63,21,0,0,1,21,0,0,1,18,0,0,1");
    E("    db 54,0,0,5,18,0,16,0,5,0,0,0,1,64,0,0");
    E("    db 0,0,128,63,21,0,0,1,56,0,0,7,130,0,16,0");
    E("    db 2,0,0,0,58,0,16,0,2,0,0,0,58,0,16,0");
    E("    db 4,0,0,0,56,0,0,7,130,0,16,0,2,0,0,0");
    E("    db 10,0,16,0,5,0,0,0,58,0,16,0,2,0,0,0");
    E("    db 56,0,0,7,130,0,16,0,2,0,0,0,58,0,16,0");
    E("    db 2,0,0,0,1,64,0,0,0,0,64,64,54,0,0,5");
    E("    db 18,0,16,0,6,0,0,0,58,0,16,0,5,0,0,0");
    E("    db 50,0,0,9,114,0,16,0,4,0,0,0,246,15,16,0");
    E("    db 2,0,0,0,70,2,16,0,6,0,0,0,70,2,16,0");
    E("    db 4,0,0,0,30,0,0,7,66,0,16,0,2,0,0,0");
    E("    db 42,0,16,0,2,0,0,0,1,64,0,0,1,0,0,0");
    E("    db 22,0,0,1,0,0,0,10,114,0,16,0,1,0,0,0");
    E("    db 70,2,16,0,4,0,0,0,2,64,0,0,0,0,112,62");
    E("    db 0,0,112,62,0,0,112,62,0,0,0,0,56,0,0,7");
    E("    db 114,32,16,0,0,0,0,0,70,2,16,0,0,0,0,0");
    E("    db 70,2,16,0,1,0,0,0,54,0,0,5,130,32,16,0");
    E("    db 0,0,0,0,58,16,16,0,2,0,0,0,62,0,0,1");
    E("    db 21,0,0,1,54,0,0,5,130,32,16,0,0,0,0,0");
    E("    db 58,16,16,0,2,0,0,0,54,0,0,5,114,32,16,0");
    E("    db 0,0,0,0,70,2,16,0,0,0,0,0,62,0,0,1");
    E("    db 83,84,65,84,148,0,0,0,194,0,0,0,12,0,0,0");
    E("    db 0,0,0,0,8,0,0,0,91,0,0,0,2,0,0,0");
    E("    db 7,0,0,0,9,0,0,0,9,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 2,0,0,0,6,0,0,0,18,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,38,0,0,0,1,0,0,0,1,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0");
    E("_gpu_ps_blob_len equ 7900");
    E("align 16");
    // --- tessellation-path DXBC blobs: tess VS (control-point pass-through),
    // Hull (tri, distance-adaptive, crack-free, frustum-cull), Domain (displace +
    // normal + project). Shared 224B cbuffer tail with the render cbuffer.
    E("_gpu_tvs_blob:  ; 988 bytes DXBC");
    E("    db 68,88,66,67,190,32,95,145,255,98,229,21,240,60,166,245");
    E("    db 242,124,145,196,1,0,0,0,220,3,0,0,5,0,0,0");
    E("    db 52,0,0,0,160,0,0,0,96,1,0,0,32,2,0,0");
    E("    db 64,3,0,0,82,68,69,70,100,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,60,0,0,0,0,5,254,255");
    E("    db 8,1,0,0,60,0,0,0,82,68,49,49,60,0,0,0");
    E("    db 24,0,0,0,32,0,0,0,40,0,0,0,36,0,0,0");
    E("    db 12,0,0,0,0,0,0,0,77,105,99,114,111,115,111,102");
    E("    db 116,32,40,82,41,32,72,76,83,76,32,83,104,97,100,101");
    E("    db 114,32,67,111,109,112,105,108,101,114,32,49,48,46,49,0");
    E("    db 73,83,71,78,184,0,0,0,6,0,0,0,8,0,0,0");
    E("    db 152,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 0,0,0,0,7,7,0,0,161,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,1,0,0,0,3,3,0,0");
    E("    db 170,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 2,0,0,0,15,15,0,0,161,0,0,0,1,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,3,0,0,0,1,1,0,0");
    E("    db 161,0,0,0,2,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 4,0,0,0,1,1,0,0,176,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,5,0,0,0,7,7,0,0");
    E("    db 80,79,83,73,84,73,79,78,0,84,69,88,67,79,79,82");
    E("    db 68,0,67,79,76,79,82,0,78,79,82,77,65,76,0,171");
    E("    db 79,83,71,78,184,0,0,0,6,0,0,0,8,0,0,0");
    E("    db 152,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 0,0,0,0,7,8,0,0,161,0,0,0,2,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,0,0,0,0,8,7,0,0");
    E("    db 161,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 1,0,0,0,3,12,0,0,161,0,0,0,1,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,1,0,0,0,4,11,0,0");
    E("    db 170,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 2,0,0,0,15,0,0,0,176,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,3,0,0,0,7,8,0,0");
    E("    db 80,79,83,73,84,73,79,78,0,84,69,88,67,79,79,82");
    E("    db 68,0,67,79,76,79,82,0,78,79,82,77,65,76,0,171");
    E("    db 83,72,69,88,24,1,0,0,80,0,1,0,70,0,0,0");
    E("    db 106,8,0,1,95,0,0,3,114,16,16,0,0,0,0,0");
    E("    db 95,0,0,3,50,16,16,0,1,0,0,0,95,0,0,3");
    E("    db 242,16,16,0,2,0,0,0,95,0,0,3,18,16,16,0");
    E("    db 3,0,0,0,95,0,0,3,18,16,16,0,4,0,0,0");
    E("    db 95,0,0,3,114,16,16,0,5,0,0,0,101,0,0,3");
    E("    db 114,32,16,0,0,0,0,0,101,0,0,3,130,32,16,0");
    E("    db 0,0,0,0,101,0,0,3,50,32,16,0,1,0,0,0");
    E("    db 101,0,0,3,66,32,16,0,1,0,0,0,101,0,0,3");
    E("    db 242,32,16,0,2,0,0,0,101,0,0,3,114,32,16,0");
    E("    db 3,0,0,0,54,0,0,5,114,32,16,0,0,0,0,0");
    E("    db 70,18,16,0,0,0,0,0,54,0,0,5,130,32,16,0");
    E("    db 0,0,0,0,10,16,16,0,4,0,0,0,54,0,0,5");
    E("    db 50,32,16,0,1,0,0,0,70,16,16,0,1,0,0,0");
    E("    db 54,0,0,5,66,32,16,0,1,0,0,0,10,16,16,0");
    E("    db 3,0,0,0,54,0,0,5,242,32,16,0,2,0,0,0");
    E("    db 70,30,16,0,2,0,0,0,54,0,0,5,114,32,16,0");
    E("    db 3,0,0,0,70,18,16,0,5,0,0,0,62,0,0,1");
    E("    db 83,84,65,84,148,0,0,0,7,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,12,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0");
    E("_gpu_tvs_blob_len equ 988");

    E("_gpu_ths_blob:  ; 4544 bytes DXBC");
    E("    db 68,88,66,67,3,185,60,74,70,74,51,56,130,225,223,46");
    E("    db 143,255,154,59,1,0,0,0,192,17,0,0,6,0,0,0");
    E("    db 56,0,0,0,0,5,0,0,192,5,0,0,128,6,0,0");
    E("    db 20,7,0,0,36,17,0,0,82,68,69,70,192,4,0,0");
    E("    db 1,0,0,0,96,0,0,0,1,0,0,0,60,0,0,0");
    E("    db 0,5,83,72,8,1,0,0,150,4,0,0,82,68,49,49");
    E("    db 60,0,0,0,24,0,0,0,32,0,0,0,40,0,0,0");
    E("    db 36,0,0,0,12,0,0,0,0,0,0,0,92,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,1,0,0,0,1,0,0,0,67,0,171,171");
    E("    db 92,0,0,0,17,0,0,0,120,0,0,0,224,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,32,3,0,0,0,0,0,0");
    E("    db 64,0,0,0,2,0,0,0,52,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 88,3,0,0,64,0,0,0,12,0,0,0,0,0,0,0");
    E("    db 104,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,140,3,0,0,76,0,0,0");
    E("    db 4,0,0,0,0,0,0,0,156,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 192,3,0,0,80,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 156,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,204,3,0,0,84,0,0,0");
    E("    db 8,0,0,0,0,0,0,0,224,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 4,4,0,0,92,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 156,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,11,4,0,0,96,0,0,0");
    E("    db 64,0,0,0,0,0,0,0,52,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 19,4,0,0,160,0,0,0,12,0,0,0,0,0,0,0");
    E("    db 104,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,26,4,0,0,172,0,0,0");
    E("    db 4,0,0,0,0,0,0,0,156,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 37,4,0,0,176,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 52,4,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,88,4,0,0,180,0,0,0");
    E("    db 12,0,0,0,0,0,0,0,104,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 94,4,0,0,192,0,0,0,4,0,0,0,2,0,0,0");
    E("    db 156,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,104,4,0,0,196,0,0,0");
    E("    db 4,0,0,0,2,0,0,0,156,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 112,4,0,0,200,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 156,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,122,4,0,0,204,0,0,0");
    E("    db 4,0,0,0,0,0,0,0,156,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 133,4,0,0,208,0,0,0,8,0,0,0,0,0,0,0");
    E("    db 224,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,143,4,0,0,216,0,0,0");
    E("    db 8,0,0,0,0,0,0,0,224,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 118,105,101,119,112,114,111,106,0,102,108,111,97,116,52,120");
    E("    db 52,0,171,171,2,0,3,0,4,0,4,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,41,3,0,0,102,111,103,67,111,108,111,114");
    E("    db 0,102,108,111,97,116,51,0,1,0,3,0,1,0,3,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,97,3,0,0,102,111,103,83");
    E("    db 116,97,114,116,0,102,108,111,97,116,0,171,0,0,3,0");
    E("    db 1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,149,3,0,0");
    E("    db 102,111,103,73,110,118,82,97,110,103,101,0,105,110,118,84");
    E("    db 101,120,68,105,109,115,0,102,108,111,97,116,50,0,171,171");
    E("    db 1,0,3,0,1,0,2,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 215,3,0,0,115,117,110,65,110,103,0,108,105,103,104,116");
    E("    db 86,80,0,99,97,109,80,111,115,0,115,104,97,100,111,119");
    E("    db 80,97,115,115,0,108,105,103,104,116,67,111,117,110,116,0");
    E("    db 105,110,116,0,0,0,2,0,1,0,1,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,48,4,0,0,95,108,112,97,100,0,116,101");
    E("    db 115,115,83,99,97,108,101,0,116,101,115,115,77,97,120,0");
    E("    db 100,105,115,112,83,99,97,108,101,0,117,115,101,78,111,114");
    E("    db 109,77,97,112,0,100,105,115,112,84,101,120,101,108,0,95");
    E("    db 116,112,97,100,50,0,77,105,99,114,111,115,111,102,116,32");
    E("    db 40,82,41,32,72,76,83,76,32,83,104,97,100,101,114,32");
    E("    db 67,111,109,112,105,108,101,114,32,49,48,46,49,0,171,171");
    E("    db 73,83,71,78,184,0,0,0,6,0,0,0,8,0,0,0");
    E("    db 152,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 0,0,0,0,7,7,0,0,161,0,0,0,2,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,0,0,0,0,8,8,0,0");
    E("    db 161,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 1,0,0,0,3,3,0,0,161,0,0,0,1,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,1,0,0,0,4,4,0,0");
    E("    db 170,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 2,0,0,0,15,15,0,0,176,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,3,0,0,0,7,7,0,0");
    E("    db 80,79,83,73,84,73,79,78,0,84,69,88,67,79,79,82");
    E("    db 68,0,67,79,76,79,82,0,78,79,82,77,65,76,0,171");
    E("    db 79,83,71,78,184,0,0,0,6,0,0,0,8,0,0,0");
    E("    db 152,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 0,0,0,0,7,8,0,0,161,0,0,0,2,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,0,0,0,0,8,7,0,0");
    E("    db 161,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 1,0,0,0,3,12,0,0,161,0,0,0,1,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,1,0,0,0,4,11,0,0");
    E("    db 170,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 2,0,0,0,15,0,0,0,176,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,3,0,0,0,7,8,0,0");
    E("    db 80,79,83,73,84,73,79,78,0,84,69,88,67,79,79,82");
    E("    db 68,0,67,79,76,79,82,0,78,79,82,77,65,76,0,171");
    E("    db 80,67,83,71,140,0,0,0,4,0,0,0,8,0,0,0");
    E("    db 104,0,0,0,0,0,0,0,13,0,0,0,3,0,0,0");
    E("    db 0,0,0,0,1,14,0,0,104,0,0,0,1,0,0,0");
    E("    db 13,0,0,0,3,0,0,0,1,0,0,0,1,14,0,0");
    E("    db 104,0,0,0,2,0,0,0,13,0,0,0,3,0,0,0");
    E("    db 2,0,0,0,1,14,0,0,118,0,0,0,0,0,0,0");
    E("    db 14,0,0,0,3,0,0,0,3,0,0,0,1,14,0,0");
    E("    db 83,86,95,84,101,115,115,70,97,99,116,111,114,0,83,86");
    E("    db 95,73,110,115,105,100,101,84,101,115,115,70,97,99,116,111");
    E("    db 114,0,171,171,83,72,69,88,8,10,0,0,80,0,3,0");
    E("    db 130,2,0,0,113,0,0,1,147,24,0,1,148,24,0,1");
    E("    db 149,16,0,1,150,24,0,1,151,24,0,1,106,8,0,1");
    E("    db 89,0,0,4,70,142,32,0,0,0,0,0,13,0,0,0");
    E("    db 115,0,0,1,95,0,0,4,114,144,33,0,3,0,0,0");
    E("    db 0,0,0,0,103,0,0,4,18,32,16,0,0,0,0,0");
    E("    db 17,0,0,0,103,0,0,4,18,32,16,0,1,0,0,0");
    E("    db 18,0,0,0,103,0,0,4,18,32,16,0,2,0,0,0");
    E("    db 19,0,0,0,103,0,0,4,18,32,16,0,3,0,0,0");
    E("    db 20,0,0,0,104,0,0,2,3,0,0,0,56,0,0,9");
    E("    db 242,0,16,0,0,0,0,0,70,142,32,0,0,0,0,0");
    E("    db 1,0,0,0,86,149,33,0,0,0,0,0,0,0,0,0");
    E("    db 50,0,0,11,242,0,16,0,0,0,0,0,6,144,33,0");
    E("    db 0,0,0,0,0,0,0,0,70,142,32,0,0,0,0,0");
    E("    db 0,0,0,0,70,14,16,0,0,0,0,0,50,0,0,11");
    E("    db 242,0,16,0,0,0,0,0,166,154,33,0,0,0,0,0");
    E("    db 0,0,0,0,70,142,32,0,0,0,0,0,2,0,0,0");
    E("    db 70,14,16,0,0,0,0,0,0,0,0,8,242,0,16,0");
    E("    db 0,0,0,0,70,14,16,0,0,0,0,0,70,142,32,0");
    E("    db 0,0,0,0,3,0,0,0,50,0,0,9,18,0,16,0");
    E("    db 1,0,0,0,58,0,16,0,0,0,0,0,1,64,0,0");
    E("    db 102,102,166,63,1,64,0,0,23,183,209,56,49,0,0,8");
    E("    db 98,0,16,0,1,0,0,0,6,1,16,0,0,0,0,0");
    E("    db 6,0,16,128,65,0,0,0,1,0,0,0,49,0,0,7");
    E("    db 50,0,16,0,0,0,0,0,6,0,16,0,1,0,0,0");
    E("    db 70,0,16,0,0,0,0,0,60,0,0,7,18,0,16,0");
    E("    db 0,0,0,0,10,0,16,0,0,0,0,0,26,0,16,0");
    E("    db 1,0,0,0,60,0,0,7,18,0,16,0,0,0,0,0");
    E("    db 42,0,16,0,1,0,0,0,10,0,16,0,0,0,0,0");
    E("    db 60,0,0,7,18,0,16,0,0,0,0,0,26,0,16,0");
    E("    db 0,0,0,0,10,0,16,0,0,0,0,0,49,0,0,7");
    E("    db 34,0,16,0,0,0,0,0,42,0,16,0,0,0,0,0");
    E("    db 1,64,0,0,0,0,0,0,60,0,0,7,18,0,16,0");
    E("    db 0,0,0,0,26,0,16,0,0,0,0,0,10,0,16,0");
    E("    db 0,0,0,0,49,0,0,7,34,0,16,0,0,0,0,0");
    E("    db 58,0,16,0,0,0,0,0,42,0,16,0,0,0,0,0");
    E("    db 60,0,0,7,18,0,16,0,0,0,0,0,26,0,16,0");
    E("    db 0,0,0,0,10,0,16,0,0,0,0,0,56,0,0,9");
    E("    db 242,0,16,0,1,0,0,0,70,142,32,0,0,0,0,0");
    E("    db 1,0,0,0,86,149,33,0,1,0,0,0,0,0,0,0");
    E("    db 50,0,0,11,242,0,16,0,1,0,0,0,6,144,33,0");
    E("    db 1,0,0,0,0,0,0,0,70,142,32,0,0,0,0,0");
    E("    db 0,0,0,0,70,14,16,0,1,0,0,0,50,0,0,11");
    E("    db 242,0,16,0,1,0,0,0,166,154,33,0,1,0,0,0");
    E("    db 0,0,0,0,70,142,32,0,0,0,0,0,2,0,0,0");
    E("    db 70,14,16,0,1,0,0,0,0,0,0,8,242,0,16,0");
    E("    db 1,0,0,0,70,14,16,0,1,0,0,0,70,142,32,0");
    E("    db 0,0,0,0,3,0,0,0,50,0,0,9,34,0,16,0");
    E("    db 0,0,0,0,58,0,16,0,1,0,0,0,1,64,0,0");
    E("    db 102,102,166,63,1,64,0,0,23,183,209,56,49,0,0,8");
    E("    db 194,0,16,0,0,0,0,0,6,4,16,0,1,0,0,0");
    E("    db 86,5,16,128,65,0,0,0,0,0,0,0,49,0,0,7");
    E("    db 50,0,16,0,1,0,0,0,86,5,16,0,0,0,0,0");
    E("    db 70,0,16,0,1,0,0,0,60,0,0,7,34,0,16,0");
    E("    db 0,0,0,0,42,0,16,0,0,0,0,0,10,0,16,0");
    E("    db 1,0,0,0,60,0,0,7,34,0,16,0,0,0,0,0");
    E("    db 58,0,16,0,0,0,0,0,26,0,16,0,0,0,0,0");
    E("    db 60,0,0,7,34,0,16,0,0,0,0,0,26,0,16,0");
    E("    db 1,0,0,0,26,0,16,0,0,0,0,0,49,0,0,7");
    E("    db 66,0,16,0,0,0,0,0,42,0,16,0,1,0,0,0");
    E("    db 1,64,0,0,0,0,0,0,60,0,0,7,34,0,16,0");
    E("    db 0,0,0,0,42,0,16,0,0,0,0,0,26,0,16,0");
    E("    db 0,0,0,0,49,0,0,7,66,0,16,0,0,0,0,0");
    E("    db 58,0,16,0,1,0,0,0,42,0,16,0,1,0,0,0");
    E("    db 60,0,0,7,34,0,16,0,0,0,0,0,42,0,16,0");
    E("    db 0,0,0,0,26,0,16,0,0,0,0,0,56,0,0,9");
    E("    db 242,0,16,0,1,0,0,0,70,142,32,0,0,0,0,0");
    E("    db 1,0,0,0,86,149,33,0,2,0,0,0,0,0,0,0");
    E("    db 50,0,0,11,242,0,16,0,1,0,0,0,6,144,33,0");
    E("    db 2,0,0,0,0,0,0,0,70,142,32,0,0,0,0,0");
    E("    db 0,0,0,0,70,14,16,0,1,0,0,0,50,0,0,11");
    E("    db 242,0,16,0,1,0,0,0,166,154,33,0,2,0,0,0");
    E("    db 0,0,0,0,70,142,32,0,0,0,0,0,2,0,0,0");
    E("    db 70,14,16,0,1,0,0,0,0,0,0,8,242,0,16,0");
    E("    db 1,0,0,0,70,14,16,0,1,0,0,0,70,142,32,0");
    E("    db 0,0,0,0,3,0,0,0,50,0,0,9,66,0,16,0");
    E("    db 0,0,0,0,58,0,16,0,1,0,0,0,1,64,0,0");
    E("    db 102,102,166,63,1,64,0,0,23,183,209,56,49,0,0,8");
    E("    db 50,0,16,0,2,0,0,0,70,0,16,0,1,0,0,0");
    E("    db 166,10,16,128,65,0,0,0,0,0,0,0,49,0,0,7");
    E("    db 194,0,16,0,0,0,0,0,166,10,16,0,0,0,0,0");
    E("    db 6,4,16,0,1,0,0,0,60,0,0,7,66,0,16,0");
    E("    db 0,0,0,0,42,0,16,0,0,0,0,0,10,0,16,0");
    E("    db 2,0,0,0,60,0,0,7,66,0,16,0,0,0,0,0");
    E("    db 26,0,16,0,2,0,0,0,42,0,16,0,0,0,0,0");
    E("    db 60,0,0,7,66,0,16,0,0,0,0,0,58,0,16,0");
    E("    db 0,0,0,0,42,0,16,0,0,0,0,0,49,0,0,7");
    E("    db 130,0,16,0,0,0,0,0,42,0,16,0,1,0,0,0");
    E("    db 1,64,0,0,0,0,0,0,60,0,0,7,66,0,16,0");
    E("    db 0,0,0,0,58,0,16,0,0,0,0,0,42,0,16,0");
    E("    db 0,0,0,0,49,0,0,7,130,0,16,0,0,0,0,0");
    E("    db 58,0,16,0,1,0,0,0,42,0,16,0,1,0,0,0");
    E("    db 60,0,0,7,66,0,16,0,0,0,0,0,58,0,16,0");
    E("    db 0,0,0,0,42,0,16,0,0,0,0,0,1,0,0,7");
    E("    db 18,0,16,0,0,0,0,0,26,0,16,0,0,0,0,0");
    E("    db 10,0,16,0,0,0,0,0,1,0,0,7,18,0,16,0");
    E("    db 0,0,0,0,42,0,16,0,0,0,0,0,10,0,16,0");
    E("    db 0,0,0,0,31,0,4,3,10,0,16,0,0,0,0,0");
    E("    db 54,0,0,5,18,32,16,0,0,0,0,0,1,64,0,0");
    E("    db 0,0,0,0,54,0,0,5,18,32,16,0,1,0,0,0");
    E("    db 1,64,0,0,0,0,0,0,54,0,0,5,18,32,16,0");
    E("    db 2,0,0,0,1,64,0,0,0,0,0,0,54,0,0,5");
    E("    db 18,32,16,0,3,0,0,0,1,64,0,0,0,0,0,0");
    E("    db 62,0,0,1,21,0,0,1,0,0,0,9,114,0,16,0");
    E("    db 0,0,0,0,70,146,33,0,2,0,0,0,0,0,0,0");
    E("    db 70,146,33,0,1,0,0,0,0,0,0,0,54,0,0,6");
    E("    db 18,0,16,0,1,0,0,0,58,128,32,0,0,0,0,0");
    E("    db 0,0,0,0,54,0,0,6,34,0,16,0,1,0,0,0");
    E("    db 58,128,32,0,0,0,0,0,1,0,0,0,54,0,0,6");
    E("    db 66,0,16,0,1,0,0,0,58,128,32,0,0,0,0,0");
    E("    db 2,0,0,0,56,0,0,7,114,0,16,0,0,0,0,0");
    E("    db 70,2,16,0,0,0,0,0,70,2,16,0,1,0,0,0");
    E("    db 16,0,0,10,18,0,16,0,0,0,0,0,70,2,16,0");
    E("    db 0,0,0,0,2,64,0,0,0,0,0,63,0,0,0,63");
    E("    db 0,0,0,63,0,0,0,0,0,0,0,8,18,0,16,0");
    E("    db 0,0,0,0,10,0,16,0,0,0,0,0,58,128,32,0");
    E("    db 0,0,0,0,3,0,0,0,52,0,0,7,18,0,16,0");
    E("    db 0,0,0,0,10,0,16,0,0,0,0,0,1,64,0,0");
    E("    db 23,183,209,56,14,0,0,8,18,0,16,0,0,0,0,0");
    E("    db 10,128,32,0,0,0,0,0,12,0,0,0,10,0,16,0");
    E("    db 0,0,0,0,52,0,0,7,18,0,16,0,0,0,0,0");
    E("    db 10,0,16,0,0,0,0,0,1,64,0,0,0,0,128,63");
    E("    db 0,0,0,9,226,0,16,0,0,0,0,0,6,153,33,0");
    E("    db 0,0,0,0,0,0,0,0,6,153,33,0,2,0,0,0");
    E("    db 0,0,0,0,56,0,0,7,226,0,16,0,0,0,0,0");
    E("    db 6,9,16,0,1,0,0,0,86,14,16,0,0,0,0,0");
    E("    db 16,0,0,10,34,0,16,0,0,0,0,0,150,7,16,0");
    E("    db 0,0,0,0,2,64,0,0,0,0,0,63,0,0,0,63");
    E("    db 0,0,0,63,0,0,0,0,0,0,0,8,34,0,16,0");
    E("    db 0,0,0,0,26,0,16,0,0,0,0,0,58,128,32,0");
    E("    db 0,0,0,0,3,0,0,0,52,0,0,7,34,0,16,0");
    E("    db 0,0,0,0,26,0,16,0,0,0,0,0,1,64,0,0");
    E("    db 23,183,209,56,14,0,0,8,34,0,16,0,0,0,0,0");
    E("    db 10,128,32,0,0,0,0,0,12,0,0,0,26,0,16,0");
    E("    db 0,0,0,0,52,0,0,7,34,0,16,0,0,0,0,0");
    E("    db 26,0,16,0,0,0,0,0,1,64,0,0,0,0,128,63");
    E("    db 0,0,0,9,114,0,16,0,2,0,0,0,70,146,33,0");
    E("    db 1,0,0,0,0,0,0,0,70,146,33,0,0,0,0,0");
    E("    db 0,0,0,0,56,0,0,7,114,0,16,0,1,0,0,0");
    E("    db 70,2,16,0,1,0,0,0,70,2,16,0,2,0,0,0");
    E("    db 16,0,0,10,66,0,16,0,0,0,0,0,70,2,16,0");
    E("    db 1,0,0,0,2,64,0,0,0,0,0,63,0,0,0,63");
    E("    db 0,0,0,63,0,0,0,0,0,0,0,8,66,0,16,0");
    E("    db 0,0,0,0,42,0,16,0,0,0,0,0,58,128,32,0");
    E("    db 0,0,0,0,3,0,0,0,52,0,0,7,66,0,16,0");
    E("    db 0,0,0,0,42,0,16,0,0,0,0,0,1,64,0,0");
    E("    db 23,183,209,56,14,0,0,8,66,0,16,0,0,0,0,0");
    E("    db 10,128,32,0,0,0,0,0,12,0,0,0,42,0,16,0");
    E("    db 0,0,0,0,52,0,0,7,66,0,16,0,0,0,0,0");
    E("    db 42,0,16,0,0,0,0,0,1,64,0,0,0,0,128,63");
    E("    db 51,0,0,8,114,0,16,0,0,0,0,0,70,2,16,0");
    E("    db 0,0,0,0,86,133,32,0,0,0,0,0,12,0,0,0");
    E("    db 0,0,0,7,130,0,16,0,0,0,0,0,26,0,16,0");
    E("    db 0,0,0,0,10,0,16,0,0,0,0,0,0,0,0,7");
    E("    db 130,0,16,0,0,0,0,0,42,0,16,0,0,0,0,0");
    E("    db 58,0,16,0,0,0,0,0,56,0,0,7,18,32,16,0");
    E("    db 3,0,0,0,58,0,16,0,0,0,0,0,1,64,0,0");
    E("    db 171,170,170,62,54,0,0,5,18,32,16,0,0,0,0,0");
    E("    db 10,0,16,0,0,0,0,0,54,0,0,5,18,32,16,0");
    E("    db 1,0,0,0,26,0,16,0,0,0,0,0,54,0,0,5");
    E("    db 18,32,16,0,2,0,0,0,42,0,16,0,0,0,0,0");
    E("    db 62,0,0,1,83,84,65,84,148,0,0,0,83,0,0,0");
    E("    db 3,0,0,0,0,0,0,0,5,0,0,0,52,0,0,0");
    E("    db 0,0,0,0,17,0,0,0,2,0,0,0,1,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,10,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,10,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,3,0,0,0,3,0,0,0");
    E("    db 2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("_gpu_ths_blob_len equ 4544");

    E("_gpu_tds_blob:  ; 4472 bytes DXBC");
    E("    db 68,88,66,67,171,22,239,203,172,64,237,253,129,168,158,16");
    E("    db 250,251,237,152,1,0,0,0,120,17,0,0,6,0,0,0");
    E("    db 56,0,0,0,120,5,0,0,56,6,0,0,204,6,0,0");
    E("    db 208,7,0,0,220,16,0,0,82,68,69,70,56,5,0,0");
    E("    db 1,0,0,0,216,0,0,0,4,0,0,0,60,0,0,0");
    E("    db 0,5,83,68,8,1,0,0,14,5,0,0,82,68,49,49");
    E("    db 60,0,0,0,24,0,0,0,32,0,0,0,40,0,0,0");
    E("    db 36,0,0,0,12,0,0,0,0,0,0,0,188,0,0,0");
    E("    db 3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,1,0,0,0,196,0,0,0");
    E("    db 2,0,0,0,5,0,0,0,4,0,0,0,255,255,255,255");
    E("    db 3,0,0,0,1,0,0,0,13,0,0,0,204,0,0,0");
    E("    db 2,0,0,0,5,0,0,0,4,0,0,0,255,255,255,255");
    E("    db 4,0,0,0,1,0,0,0,13,0,0,0,212,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,1,0,0,0,1,0,0,0,115,109,112,68");
    E("    db 105,115,112,0,100,105,115,112,77,97,112,0,110,111,114,109");
    E("    db 77,97,112,0,67,0,171,171,212,0,0,0,17,0,0,0");
    E("    db 240,0,0,0,224,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 152,3,0,0,0,0,0,0,64,0,0,0,2,0,0,0");
    E("    db 172,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,208,3,0,0,64,0,0,0");
    E("    db 12,0,0,0,0,0,0,0,224,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 4,4,0,0,76,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 20,4,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,56,4,0,0,80,0,0,0");
    E("    db 4,0,0,0,0,0,0,0,20,4,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 68,4,0,0,84,0,0,0,8,0,0,0,2,0,0,0");
    E("    db 88,4,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,124,4,0,0,92,0,0,0");
    E("    db 4,0,0,0,0,0,0,0,20,4,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 131,4,0,0,96,0,0,0,64,0,0,0,2,0,0,0");
    E("    db 172,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,139,4,0,0,160,0,0,0");
    E("    db 12,0,0,0,0,0,0,0,224,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 146,4,0,0,172,0,0,0,4,0,0,0,2,0,0,0");
    E("    db 20,4,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,157,4,0,0,176,0,0,0");
    E("    db 4,0,0,0,0,0,0,0,172,4,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 208,4,0,0,180,0,0,0,12,0,0,0,0,0,0,0");
    E("    db 224,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,214,4,0,0,192,0,0,0");
    E("    db 4,0,0,0,0,0,0,0,20,4,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 224,4,0,0,196,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 20,4,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,232,4,0,0,200,0,0,0");
    E("    db 4,0,0,0,2,0,0,0,20,4,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 242,4,0,0,204,0,0,0,4,0,0,0,2,0,0,0");
    E("    db 20,4,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,253,4,0,0,208,0,0,0");
    E("    db 8,0,0,0,2,0,0,0,88,4,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 7,5,0,0,216,0,0,0,8,0,0,0,0,0,0,0");
    E("    db 88,4,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,118,105,101,119,112,114,111,106");
    E("    db 0,102,108,111,97,116,52,120,52,0,171,171,2,0,3,0");
    E("    db 4,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,161,3,0,0");
    E("    db 102,111,103,67,111,108,111,114,0,102,108,111,97,116,51,0");
    E("    db 1,0,3,0,1,0,3,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 217,3,0,0,102,111,103,83,116,97,114,116,0,102,108,111");
    E("    db 97,116,0,171,0,0,3,0,1,0,1,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,13,4,0,0,102,111,103,73,110,118,82,97");
    E("    db 110,103,101,0,105,110,118,84,101,120,68,105,109,115,0,102");
    E("    db 108,111,97,116,50,0,171,171,1,0,3,0,1,0,2,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,79,4,0,0,115,117,110,65");
    E("    db 110,103,0,108,105,103,104,116,86,80,0,99,97,109,80,111");
    E("    db 115,0,115,104,97,100,111,119,80,97,115,115,0,108,105,103");
    E("    db 104,116,67,111,117,110,116,0,105,110,116,0,0,0,2,0");
    E("    db 1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,168,4,0,0");
    E("    db 95,108,112,97,100,0,116,101,115,115,83,99,97,108,101,0");
    E("    db 116,101,115,115,77,97,120,0,100,105,115,112,83,99,97,108");
    E("    db 101,0,117,115,101,78,111,114,109,77,97,112,0,100,105,115");
    E("    db 112,84,101,120,101,108,0,95,116,112,97,100,50,0,77,105");
    E("    db 99,114,111,115,111,102,116,32,40,82,41,32,72,76,83,76");
    E("    db 32,83,104,97,100,101,114,32,67,111,109,112,105,108,101,114");
    E("    db 32,49,48,46,49,0,171,171,73,83,71,78,184,0,0,0");
    E("    db 6,0,0,0,8,0,0,0,152,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,0,0,0,0,7,7,0,0");
    E("    db 161,0,0,0,2,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 0,0,0,0,8,8,0,0,161,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,1,0,0,0,3,3,0,0");
    E("    db 161,0,0,0,1,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 1,0,0,0,4,4,0,0,170,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,2,0,0,0,15,15,0,0");
    E("    db 176,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 3,0,0,0,7,7,0,0,80,79,83,73,84,73,79,78");
    E("    db 0,84,69,88,67,79,79,82,68,0,67,79,76,79,82,0");
    E("    db 78,79,82,77,65,76,0,171,80,67,83,71,140,0,0,0");
    E("    db 4,0,0,0,8,0,0,0,104,0,0,0,0,0,0,0");
    E("    db 13,0,0,0,3,0,0,0,0,0,0,0,1,0,0,0");
    E("    db 104,0,0,0,1,0,0,0,13,0,0,0,3,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,104,0,0,0,2,0,0,0");
    E("    db 13,0,0,0,3,0,0,0,2,0,0,0,1,0,0,0");
    E("    db 118,0,0,0,0,0,0,0,14,0,0,0,3,0,0,0");
    E("    db 3,0,0,0,1,0,0,0,83,86,95,84,101,115,115,70");
    E("    db 97,99,116,111,114,0,83,86,95,73,110,115,105,100,101,84");
    E("    db 101,115,115,70,97,99,116,111,114,0,171,171,79,83,71,78");
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
    E("    db 83,72,69,88,4,9,0,0,80,0,4,0,65,2,0,0");
    E("    db 147,24,0,1,149,16,0,1,106,8,0,1,89,0,0,4");
    E("    db 70,142,32,0,0,0,0,0,14,0,0,0,90,0,0,3");
    E("    db 0,96,16,0,1,0,0,0,88,24,0,4,0,112,16,0");
    E("    db 3,0,0,0,85,85,0,0,88,24,0,4,0,112,16,0");
    E("    db 4,0,0,0,85,85,0,0,95,0,0,2,114,192,1,0");
    E("    db 95,0,0,4,114,144,33,0,3,0,0,0,0,0,0,0");
    E("    db 95,0,0,4,130,144,33,0,3,0,0,0,0,0,0,0");
    E("    db 95,0,0,4,50,144,33,0,3,0,0,0,1,0,0,0");
    E("    db 95,0,0,4,66,144,33,0,3,0,0,0,1,0,0,0");
    E("    db 95,0,0,4,242,144,33,0,3,0,0,0,2,0,0,0");
    E("    db 95,0,0,4,114,144,33,0,3,0,0,0,3,0,0,0");
    E("    db 103,0,0,4,242,32,16,0,0,0,0,0,1,0,0,0");
    E("    db 101,0,0,3,50,32,16,0,1,0,0,0,101,0,0,3");
    E("    db 66,32,16,0,1,0,0,0,101,0,0,3,130,32,16,0");
    E("    db 1,0,0,0,101,0,0,3,242,32,16,0,2,0,0,0");
    E("    db 101,0,0,3,114,32,16,0,3,0,0,0,101,0,0,3");
    E("    db 130,32,16,0,3,0,0,0,101,0,0,3,242,32,16,0");
    E("    db 4,0,0,0,101,0,0,3,114,32,16,0,5,0,0,0");
    E("    db 104,0,0,2,5,0,0,0,56,0,0,7,114,0,16,0");
    E("    db 0,0,0,0,86,197,1,0,70,146,33,0,1,0,0,0");
    E("    db 0,0,0,0,50,0,0,9,114,0,16,0,0,0,0,0");
    E("    db 70,146,33,0,0,0,0,0,0,0,0,0,6,192,1,0");
    E("    db 70,2,16,0,0,0,0,0,50,0,0,9,114,0,16,0");
    E("    db 0,0,0,0,70,146,33,0,2,0,0,0,0,0,0,0");
    E("    db 166,202,1,0,70,2,16,0,0,0,0,0,56,0,0,7");
    E("    db 50,0,16,0,1,0,0,0,86,197,1,0,70,144,33,0");
    E("    db 1,0,0,0,1,0,0,0,50,0,0,9,50,0,16,0");
    E("    db 1,0,0,0,70,144,33,0,0,0,0,0,1,0,0,0");
    E("    db 6,192,1,0,70,0,16,0,1,0,0,0,50,0,0,9");
    E("    db 50,0,16,0,1,0,0,0,70,144,33,0,2,0,0,0");
    E("    db 1,0,0,0,166,202,1,0,70,0,16,0,1,0,0,0");
    E("    db 56,0,0,7,242,0,16,0,2,0,0,0,86,197,1,0");
    E("    db 70,158,33,0,1,0,0,0,2,0,0,0,50,0,0,9");
    E("    db 242,0,16,0,2,0,0,0,70,158,33,0,0,0,0,0");
    E("    db 2,0,0,0,6,192,1,0,70,14,16,0,2,0,0,0");
    E("    db 50,0,0,9,242,32,16,0,2,0,0,0,70,158,33,0");
    E("    db 2,0,0,0,2,0,0,0,166,202,1,0,70,14,16,0");
    E("    db 2,0,0,0,56,0,0,7,114,0,16,0,2,0,0,0");
    E("    db 86,197,1,0,70,146,33,0,1,0,0,0,3,0,0,0");
    E("    db 50,0,0,9,114,0,16,0,2,0,0,0,70,146,33,0");
    E("    db 0,0,0,0,3,0,0,0,6,192,1,0,70,2,16,0");
    E("    db 2,0,0,0,50,0,0,9,114,0,16,0,2,0,0,0");
    E("    db 70,146,33,0,2,0,0,0,3,0,0,0,166,202,1,0");
    E("    db 70,2,16,0,2,0,0,0,16,0,0,7,130,0,16,0");
    E("    db 0,0,0,0,70,2,16,0,2,0,0,0,70,2,16,0");
    E("    db 2,0,0,0,68,0,0,5,130,0,16,0,0,0,0,0");
    E("    db 58,0,16,0,0,0,0,0,56,0,0,7,114,0,16,0");
    E("    db 2,0,0,0,246,15,16,0,0,0,0,0,70,2,16,0");
    E("    db 2,0,0,0,56,0,0,8,194,0,16,0,1,0,0,0");
    E("    db 6,4,16,0,1,0,0,0,86,137,32,0,0,0,0,0");
    E("    db 5,0,0,0,72,0,0,141,194,0,0,128,67,85,21,0");
    E("    db 130,0,16,0,0,0,0,0,230,10,16,0,1,0,0,0");
    E("    db 150,115,16,0,3,0,0,0,0,96,16,0,1,0,0,0");
    E("    db 1,64,0,0,0,0,0,0,56,0,0,8,130,0,16,0");
    E("    db 2,0,0,0,58,0,16,0,0,0,0,0,42,128,32,0");
    E("    db 0,0,0,0,12,0,0,0,50,0,0,9,114,0,16,0");
    E("    db 0,0,0,0,70,2,16,0,2,0,0,0,246,15,16,0");
    E("    db 2,0,0,0,70,2,16,0,0,0,0,0,49,0,0,8");
    E("    db 18,0,16,0,2,0,0,0,1,64,0,0,0,0,0,63");
    E("    db 58,128,32,0,0,0,0,0,12,0,0,0,31,0,4,3");
    E("    db 10,0,16,0,2,0,0,0,72,0,0,141,194,0,0,128");
    E("    db 67,85,21,0,114,0,16,0,2,0,0,0,230,10,16,0");
    E("    db 1,0,0,0,70,126,16,0,4,0,0,0,0,96,16,0");
    E("    db 1,0,0,0,1,64,0,0,0,0,0,0,50,0,0,15");
    E("    db 114,0,16,0,2,0,0,0,70,2,16,0,2,0,0,0");
    E("    db 2,64,0,0,0,0,0,64,0,0,0,64,0,0,0,64");
    E("    db 0,0,0,0,2,64,0,0,0,0,128,191,0,0,128,191");
    E("    db 0,0,128,191,0,0,0,0,16,0,0,7,130,0,16,0");
    E("    db 2,0,0,0,70,2,16,0,2,0,0,0,70,2,16,0");
    E("    db 2,0,0,0,68,0,0,5,130,0,16,0,2,0,0,0");
    E("    db 58,0,16,0,2,0,0,0,56,0,0,7,114,32,16,0");
    E("    db 5,0,0,0,246,15,16,0,2,0,0,0,70,2,16,0");
    E("    db 2,0,0,0,18,0,0,1,54,0,0,6,34,0,16,0");
    E("    db 2,0,0,0,10,128,32,0,0,0,0,0,13,0,0,0");
    E("    db 54,0,0,8,82,0,16,0,2,0,0,0,2,64,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 50,0,0,10,50,0,16,0,3,0,0,0,70,0,16,0");
    E("    db 1,0,0,0,150,133,32,0,0,0,0,0,5,0,0,0");
    E("    db 150,5,16,0,2,0,0,0,72,0,0,141,194,0,0,128");
    E("    db 67,85,21,0,130,0,16,0,2,0,0,0,70,0,16,0");
    E("    db 3,0,0,0,150,115,16,0,3,0,0,0,0,96,16,0");
    E("    db 1,0,0,0,1,64,0,0,0,0,0,0,54,0,0,5");
    E("    db 18,0,16,0,3,0,0,0,1,64,0,0,0,0,0,0");
    E("    db 54,0,0,6,34,0,16,0,3,0,0,0,26,128,32,0");
    E("    db 0,0,0,0,13,0,0,0,50,0,0,10,50,0,16,0");
    E("    db 1,0,0,0,70,0,16,0,1,0,0,0,150,133,32,0");
    E("    db 0,0,0,0,5,0,0,0,70,0,16,0,3,0,0,0");
    E("    db 72,0,0,141,194,0,0,128,67,85,21,0,18,0,16,0");
    E("    db 1,0,0,0,70,0,16,0,1,0,0,0,70,126,16,0");
    E("    db 3,0,0,0,0,96,16,0,1,0,0,0,1,64,0,0");
    E("    db 0,0,0,0,0,0,0,8,34,0,16,0,1,0,0,0");
    E("    db 58,0,16,128,65,0,0,0,0,0,0,0,58,0,16,0");
    E("    db 2,0,0,0,56,0,0,8,66,0,16,0,3,0,0,0");
    E("    db 26,0,16,0,1,0,0,0,42,128,32,0,0,0,0,0");
    E("    db 12,0,0,0,0,0,0,8,130,0,16,0,0,0,0,0");
    E("    db 58,0,16,128,65,0,0,0,0,0,0,0,10,0,16,0");
    E("    db 1,0,0,0,56,0,0,8,18,0,16,0,3,0,0,0");
    E("    db 58,0,16,0,0,0,0,0,42,128,32,0,0,0,0,0");
    E("    db 12,0,0,0,54,0,0,6,34,0,16,0,3,0,0,0");
    E("    db 26,128,32,0,0,0,0,0,13,0,0,0,56,0,0,8");
    E("    db 82,0,16,0,4,0,0,0,166,8,16,0,3,0,0,0");
    E("    db 86,132,32,0,0,0,0,0,13,0,0,0,54,0,0,5");
    E("    db 34,0,16,0,4,0,0,0,1,64,0,0,0,0,0,0");
    E("    db 50,0,0,10,114,0,16,0,2,0,0,0,70,2,16,0");
    E("    db 3,0,0,0,70,2,16,0,2,0,0,0,70,2,16,128");
    E("    db 65,0,0,0,4,0,0,0,16,0,0,7,130,0,16,0");
    E("    db 0,0,0,0,70,2,16,0,2,0,0,0,70,2,16,0");
    E("    db 2,0,0,0,68,0,0,5,130,0,16,0,0,0,0,0");
    E("    db 58,0,16,0,0,0,0,0,56,0,0,7,114,32,16,0");
    E("    db 5,0,0,0,246,15,16,0,0,0,0,0,70,2,16,0");
    E("    db 2,0,0,0,21,0,0,1,49,0,0,8,130,0,16,0");
    E("    db 0,0,0,0,1,64,0,0,0,0,0,63,58,128,32,0");
    E("    db 0,0,0,0,10,0,0,0,56,0,0,8,242,0,16,0");
    E("    db 2,0,0,0,86,5,16,0,0,0,0,0,70,142,32,0");
    E("    db 0,0,0,0,7,0,0,0,50,0,0,10,242,0,16,0");
    E("    db 2,0,0,0,6,0,16,0,0,0,0,0,70,142,32,0");
    E("    db 0,0,0,0,6,0,0,0,70,14,16,0,2,0,0,0");
    E("    db 50,0,0,10,242,0,16,0,2,0,0,0,166,10,16,0");
    E("    db 0,0,0,0,70,142,32,0,0,0,0,0,8,0,0,0");
    E("    db 70,14,16,0,2,0,0,0,0,0,0,8,242,0,16,0");
    E("    db 2,0,0,0,70,14,16,0,2,0,0,0,70,142,32,0");
    E("    db 0,0,0,0,9,0,0,0,56,0,0,8,242,0,16,0");
    E("    db 3,0,0,0,86,5,16,0,0,0,0,0,70,142,32,0");
    E("    db 0,0,0,0,1,0,0,0,50,0,0,10,242,0,16,0");
    E("    db 3,0,0,0,6,0,16,0,0,0,0,0,70,142,32,0");
    E("    db 0,0,0,0,0,0,0,0,70,14,16,0,3,0,0,0");
    E("    db 50,0,0,10,242,0,16,0,3,0,0,0,166,10,16,0");
    E("    db 0,0,0,0,70,142,32,0,0,0,0,0,2,0,0,0");
    E("    db 70,14,16,0,3,0,0,0,0,0,0,8,242,0,16,0");
    E("    db 3,0,0,0,70,14,16,0,3,0,0,0,70,142,32,0");
    E("    db 0,0,0,0,3,0,0,0,55,0,0,9,242,32,16,0");
    E("    db 0,0,0,0,246,15,16,0,0,0,0,0,70,14,16,0");
    E("    db 2,0,0,0,70,14,16,0,3,0,0,0,55,0,0,12");
    E("    db 242,32,16,0,4,0,0,0,246,15,16,0,0,0,0,0");
    E("    db 2,64,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,128,63,70,14,16,0,2,0,0,0,55,0,0,9");
    E("    db 130,32,16,0,3,0,0,0,58,0,16,0,0,0,0,0");
    E("    db 1,64,0,0,0,0,0,0,58,0,16,0,3,0,0,0");
    E("    db 54,0,0,6,66,32,16,0,1,0,0,0,42,144,33,0");
    E("    db 0,0,0,0,1,0,0,0,54,0,0,6,130,32,16,0");
    E("    db 1,0,0,0,58,144,33,0,0,0,0,0,0,0,0,0");
    E("    db 54,0,0,5,50,32,16,0,1,0,0,0,230,10,16,0");
    E("    db 1,0,0,0,54,0,0,5,114,32,16,0,3,0,0,0");
    E("    db 70,2,16,0,0,0,0,0,62,0,0,1,83,84,65,84");
    E("    db 148,0,0,0,64,0,0,0,5,0,0,0,0,0,0,0");
    E("    db 16,0,0,0,43,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 2,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 10,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0");
    E("_gpu_tds_blob_len equ 4472");

    // --- physics compute-shader DXBC blobs (cs_5_0): 4 entry points ---
    // integrate/clear/resolve/apply. StructuredBuffer Bodies (stride 96, UAV u0 +
    // SRV t0), ImpulseAccum (stride 32, UAV u1), PhysicsConstants cbuffer (192B, b0).
    E("align 16");
    E("_gpu_cs_integrate_blob:  ; 5628 bytes DXBC (physics compute: main (integrate + static collision))");
    E("    db 68,88,66,67,82,223,119,33,196,224,171,184,192,242,217,137");
    E("    db 214,181,116,60,1,0,0,0,252,21,0,0,5,0,0,0");
    E("    db 52,0,0,0,240,6,0,0,0,7,0,0,16,7,0,0");
    E("    db 96,21,0,0,82,68,69,70,180,6,0,0,2,0,0,0");
    E("    db 148,0,0,0,2,0,0,0,60,0,0,0,0,5,83,67");
    E("    db 0,1,0,0,140,6,0,0,82,68,49,49,60,0,0,0");
    E("    db 24,0,0,0,32,0,0,0,40,0,0,0,36,0,0,0");
    E("    db 12,0,0,0,0,0,0,0,124,0,0,0,6,0,0,0");
    E("    db 6,0,0,0,1,0,0,0,96,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,131,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,66,111,100,105,101,115,0,80");
    E("    db 104,121,115,105,99,115,67,111,110,115,116,97,110,116,115,0");
    E("    db 131,0,0,0,14,0,0,0,196,0,0,0,208,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,124,0,0,0,1,0,0,0");
    E("    db 116,4,0,0,96,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 244,2,0,0,0,0,0,0,12,0,0,0,2,0,0,0");
    E("    db 4,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,40,3,0,0,12,0,0,0");
    E("    db 4,0,0,0,2,0,0,0,56,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 92,3,0,0,16,0,0,0,4,0,0,0,2,0,0,0");
    E("    db 108,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,144,3,0,0,20,0,0,0");
    E("    db 4,0,0,0,2,0,0,0,56,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 155,3,0,0,24,0,0,0,4,0,0,0,2,0,0,0");
    E("    db 56,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,167,3,0,0,28,0,0,0");
    E("    db 4,0,0,0,2,0,0,0,56,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 175,3,0,0,32,0,0,0,4,0,0,0,2,0,0,0");
    E("    db 56,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,193,3,0,0,36,0,0,0");
    E("    db 4,0,0,0,2,0,0,0,108,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 207,3,0,0,48,0,0,0,128,0,0,0,2,0,0,0");
    E("    db 224,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,4,4,0,0,176,0,0,0");
    E("    db 4,0,0,0,0,0,0,0,56,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 13,4,0,0,180,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 56,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,31,4,0,0,184,0,0,0");
    E("    db 4,0,0,0,0,0,0,0,56,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 49,4,0,0,188,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 56,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,68,4,0,0,192,0,0,0");
    E("    db 16,0,0,0,2,0,0,0,80,4,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 103,114,97,118,105,116,121,0,102,108,111,97,116,51,0,171");
    E("    db 1,0,3,0,1,0,3,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 252,2,0,0,100,101,108,116,97,84,105,109,101,0,102,108");
    E("    db 111,97,116,0,0,0,3,0,1,0,1,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,50,3,0,0,98,111,100,121,67,111,117,110");
    E("    db 116,0,100,119,111,114,100,0,0,0,19,0,1,0,1,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,102,3,0,0,108,105,110,101");
    E("    db 97,114,68,97,109,112,0,97,110,103,117,108,97,114,68,97");
    E("    db 109,112,0,103,114,111,117,110,100,89,0,103,114,111,117,110");
    E("    db 100,82,101,115,116,105,116,117,116,105,111,110,0,99,111,108");
    E("    db 108,105,100,101,114,67,111,117,110,116,0,99,111,108,108,105");
    E("    db 100,101,114,115,0,102,108,111,97,116,52,0,1,0,3,0");
    E("    db 1,0,4,0,8,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,217,3,0,0");
    E("    db 102,114,105,99,116,105,111,110,0,115,108,101,101,112,76,105");
    E("    db 110,84,104,114,101,115,104,111,108,100,0,115,108,101,101,112");
    E("    db 65,110,103,84,104,114,101,115,104,111,108,100,0,115,108,101");
    E("    db 101,112,84,105,109,101,84,104,114,101,115,104,111,108,100,0");
    E("    db 119,97,108,108,66,111,117,110,100,115,0,171,1,0,3,0");
    E("    db 1,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,217,3,0,0");
    E("    db 156,4,0,0,0,0,0,0,96,0,0,0,2,0,0,0");
    E("    db 104,6,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,36,69,108,101,109,101,110,116");
    E("    db 0,82,105,103,105,100,66,111,100,121,0,112,111,115,105,116");
    E("    db 105,111,110,0,1,0,3,0,1,0,3,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,252,2,0,0,105,110,118,77,97,115,115,0");
    E("    db 0,0,3,0,1,0,1,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 50,3,0,0,118,101,108,111,99,105,116,121,0,114,97,100");
    E("    db 105,117,115,0,111,114,105,101,110,116,97,116,105,111,110,0");
    E("    db 1,0,3,0,1,0,4,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 217,3,0,0,97,110,103,117,108,97,114,86,101,108,0,114");
    E("    db 101,115,116,105,116,117,116,105,111,110,0,102,108,97,103,115");
    E("    db 0,171,171,171,0,0,19,0,1,0,1,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,102,3,0,0,95,112,97,100,0,105,110,118");
    E("    db 73,110,101,114,116,105,97,0,115,108,101,101,112,84,105,109");
    E("    db 101,114,0,95,112,97,100,50,0,102,108,111,97,116,50,0");
    E("    db 1,0,3,0,1,0,2,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 173,5,0,0,175,4,0,0,184,4,0,0,0,0,0,0");
    E("    db 220,4,0,0,228,4,0,0,12,0,0,0,8,5,0,0");
    E("    db 184,4,0,0,16,0,0,0,17,5,0,0,228,4,0,0");
    E("    db 28,0,0,0,24,5,0,0,36,5,0,0,32,0,0,0");
    E("    db 72,5,0,0,184,4,0,0,48,0,0,0,83,5,0,0");
    E("    db 228,4,0,0,60,0,0,0,95,5,0,0,104,5,0,0");
    E("    db 64,0,0,0,140,5,0,0,184,4,0,0,68,0,0,0");
    E("    db 145,5,0,0,228,4,0,0,80,0,0,0,156,5,0,0");
    E("    db 228,4,0,0,84,0,0,0,167,5,0,0,180,5,0,0");
    E("    db 88,0,0,0,5,0,0,0,1,0,24,0,0,0,12,0");
    E("    db 216,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,165,4,0,0,77,105,99,114,111,115,111,102");
    E("    db 116,32,40,82,41,32,72,76,83,76,32,83,104,97,100,101");
    E("    db 114,32,67,111,109,112,105,108,101,114,32,49,48,46,49,0");
    E("    db 73,83,71,78,8,0,0,0,0,0,0,0,8,0,0,0");
    E("    db 79,83,71,78,8,0,0,0,0,0,0,0,8,0,0,0");
    E("    db 83,72,69,88,72,14,0,0,80,0,5,0,146,3,0,0");
    E("    db 106,8,0,1,89,8,0,4,70,142,32,0,0,0,0,0");
    E("    db 13,0,0,0,158,0,0,4,0,224,17,0,0,0,0,0");
    E("    db 96,0,0,0,95,0,0,2,18,0,2,0,104,0,0,2");
    E("    db 13,0,0,0,155,0,0,4,0,1,0,0,1,0,0,0");
    E("    db 1,0,0,0,80,0,0,7,18,0,16,0,0,0,0,0");
    E("    db 10,0,2,0,10,128,32,0,0,0,0,0,1,0,0,0");
    E("    db 31,0,4,3,10,0,16,0,0,0,0,0,62,0,0,1");
    E("    db 21,0,0,1,167,0,0,138,2,3,3,128,131,153,25,0");
    E("    db 242,0,16,0,0,0,0,0,10,0,2,0,1,64,0,0");
    E("    db 0,0,0,0,70,238,17,0,0,0,0,0,167,0,0,138");
    E("    db 2,3,3,128,131,153,25,0,242,0,16,0,1,0,0,0");
    E("    db 10,0,2,0,1,64,0,0,16,0,0,0,70,238,17,0");
    E("    db 0,0,0,0,167,0,0,138,2,3,3,128,131,153,25,0");
    E("    db 242,0,16,0,2,0,0,0,10,0,2,0,1,64,0,0");
    E("    db 32,0,0,0,70,238,17,0,0,0,0,0,167,0,0,138");
    E("    db 2,3,3,128,131,153,25,0,242,0,16,0,3,0,0,0");
    E("    db 10,0,2,0,1,64,0,0,48,0,0,0,70,238,17,0");
    E("    db 0,0,0,0,167,0,0,138,2,3,3,128,131,153,25,0");
    E("    db 18,0,16,0,4,0,0,0,10,0,2,0,1,64,0,0");
    E("    db 64,0,0,0,6,224,17,0,0,0,0,0,49,0,0,9");
    E("    db 34,0,16,0,4,0,0,0,10,128,32,0,0,0,0,0");
    E("    db 12,0,0,0,26,128,32,0,0,0,0,0,12,0,0,0");
    E("    db 0,0,0,8,18,0,16,0,5,0,0,0,58,0,16,0");
    E("    db 1,0,0,0,10,128,32,0,0,0,0,0,12,0,0,0");
    E("    db 0,0,0,9,194,0,16,0,4,0,0,0,246,15,16,128");
    E("    db 65,0,0,0,1,0,0,0,86,137,32,0,0,0,0,0");
    E("    db 12,0,0,0,52,0,0,7,18,0,16,0,6,0,0,0");
    E("    db 10,0,16,0,0,0,0,0,10,0,16,0,5,0,0,0");
    E("    db 51,0,0,7,18,0,16,0,6,0,0,0,42,0,16,0");
    E("    db 4,0,0,0,10,0,16,0,6,0,0,0,55,0,0,9");
    E("    db 18,0,16,0,0,0,0,0,26,0,16,0,4,0,0,0");
    E("    db 10,0,16,0,6,0,0,0,10,0,16,0,0,0,0,0");
    E("    db 49,0,0,8,18,0,16,0,6,0,0,0,1,64,0,0");
    E("    db 0,0,0,0,42,128,32,0,0,0,0,0,12,0,0,0");
    E("    db 51,0,0,7,34,0,16,0,6,0,0,0,26,0,16,0");
    E("    db 0,0,0,0,58,0,16,0,4,0,0,0,55,0,0,9");
    E("    db 34,0,16,0,0,0,0,0,10,0,16,0,6,0,0,0");
    E("    db 26,0,16,0,6,0,0,0,26,0,16,0,0,0,0,0");
    E("    db 1,0,0,10,98,0,16,0,6,0,0,0,6,0,16,0");
    E("    db 4,0,0,0,2,64,0,0,0,0,0,0,1,0,0,0");
    E("    db 2,0,0,0,0,0,0,0,39,0,0,7,18,0,16,0");
    E("    db 4,0,0,0,42,0,16,0,6,0,0,0,1,64,0,0");
    E("    db 0,0,0,0,32,0,0,7,34,0,16,0,6,0,0,0");
    E("    db 26,0,16,0,6,0,0,0,1,64,0,0,0,0,0,0");
    E("    db 60,0,0,7,18,0,16,0,4,0,0,0,10,0,16,0");
    E("    db 4,0,0,0,26,0,16,0,6,0,0,0,31,0,4,3");
    E("    db 10,0,16,0,4,0,0,0,168,0,0,8,50,224,17,0");
    E("    db 0,0,0,0,10,0,2,0,1,64,0,0,0,0,0,0");
    E("    db 70,0,16,0,0,0,0,0,62,0,0,1,21,0,0,1");
    E("    db 49,0,0,7,18,0,16,0,4,0,0,0,1,64,0,0");
    E("    db 0,0,0,0,58,0,16,0,0,0,0,0,31,0,4,3");
    E("    db 10,0,16,0,4,0,0,0,50,0,0,11,226,0,16,0");
    E("    db 6,0,0,0,6,137,32,0,0,0,0,0,0,0,0,0");
    E("    db 246,143,32,0,0,0,0,0,0,0,0,0,6,9,16,0");
    E("    db 1,0,0,0,50,32,0,15,50,0,16,0,7,0,0,0");
    E("    db 150,133,32,128,65,0,0,0,0,0,0,0,1,0,0,0");
    E("    db 246,143,32,0,0,0,0,0,0,0,0,0,2,64,0,0");
    E("    db 0,0,128,63,0,0,128,63,0,0,0,0,0,0,0,0");
    E("    db 56,0,0,7,226,0,16,0,8,0,0,0,86,14,16,0");
    E("    db 6,0,0,0,6,0,16,0,7,0,0,0,56,0,0,7");
    E("    db 114,0,16,0,3,0,0,0,70,2,16,0,3,0,0,0");
    E("    db 86,5,16,0,7,0,0,0,54,0,0,5,130,0,16,0");
    E("    db 0,0,0,0,42,0,16,0,0,0,0,0,50,0,0,10");
    E("    db 226,0,16,0,6,0,0,0,86,14,16,0,8,0,0,0");
    E("    db 246,143,32,0,0,0,0,0,0,0,0,0,6,13,16,0");
    E("    db 0,0,0,0,56,0,0,8,114,0,16,0,7,0,0,0");
    E("    db 70,2,16,0,3,0,0,0,246,143,32,0,0,0,0,0");
    E("    db 0,0,0,0,56,0,0,7,114,0,16,0,9,0,0,0");
    E("    db 150,4,16,0,2,0,0,0,38,9,16,0,7,0,0,0");
    E("    db 50,0,0,10,114,0,16,0,9,0,0,0,150,4,16,0");
    E("    db 7,0,0,0,38,9,16,0,2,0,0,0,70,2,16,128");
    E("    db 65,0,0,0,9,0,0,0,50,0,0,9,114,0,16,0");
    E("    db 9,0,0,0,246,15,16,0,2,0,0,0,70,2,16,0");
    E("    db 7,0,0,0,70,2,16,0,9,0,0,0,16,0,0,7");
    E("    db 130,0,16,0,9,0,0,0,70,2,16,0,7,0,0,0");
    E("    db 70,2,16,0,2,0,0,0,50,0,0,12,242,0,16,0");
    E("    db 7,0,0,0,70,14,16,0,9,0,0,0,2,64,0,0");
    E("    db 0,0,0,63,0,0,0,63,0,0,0,63,0,0,0,191");
    E("    db 70,14,16,0,2,0,0,0,17,0,0,7,18,0,16,0");
    E("    db 4,0,0,0,70,14,16,0,7,0,0,0,70,14,16,0");
    E("    db 7,0,0,0,68,0,0,5,18,0,16,0,4,0,0,0");
    E("    db 10,0,16,0,4,0,0,0,56,0,0,7,242,0,16,0");
    E("    db 2,0,0,0,6,0,16,0,4,0,0,0,70,14,16,0");
    E("    db 7,0,0,0,0,0,0,8,18,0,16,0,7,0,0,0");
    E("    db 58,0,16,0,1,0,0,0,58,128,32,0,0,0,0,0");
    E("    db 1,0,0,0,0,0,0,8,18,0,16,0,4,0,0,0");
    E("    db 42,0,16,128,65,0,0,0,6,0,0,0,10,0,16,0");
    E("    db 7,0,0,0,49,0,0,7,18,0,16,0,4,0,0,0");
    E("    db 1,64,0,0,0,0,0,0,10,0,16,0,4,0,0,0");
    E("    db 49,0,0,7,18,0,16,0,9,0,0,0,42,0,16,0");
    E("    db 8,0,0,0,1,64,0,0,0,0,0,0,51,0,0,8");
    E("    db 34,0,16,0,9,0,0,0,58,0,16,0,3,0,0,0");
    E("    db 10,128,32,0,0,0,0,0,2,0,0,0,56,0,0,8");
    E("    db 66,0,16,0,9,0,0,0,42,0,16,128,65,0,0,0");
    E("    db 8,0,0,0,26,0,16,0,9,0,0,0,56,0,0,10");
    E("    db 162,0,16,0,9,0,0,0,86,13,16,0,8,0,0,0");
    E("    db 2,64,0,0,0,0,0,0,72,225,122,63,0,0,0,0");
    E("    db 72,225,122,63,55,0,0,9,226,0,16,0,7,0,0,0");
    E("    db 6,0,16,0,9,0,0,0,86,14,16,0,9,0,0,0");
    E("    db 86,14,16,0,8,0,0,0,54,0,0,5,18,0,16,0");
    E("    db 8,0,0,0,42,0,16,0,6,0,0,0,55,0,0,9");
    E("    db 242,0,16,0,7,0,0,0,6,0,16,0,4,0,0,0");
    E("    db 70,14,16,0,7,0,0,0,70,14,16,0,8,0,0,0");
    E("    db 84,0,0,8,18,0,16,0,4,0,0,0,26,128,32,0");
    E("    db 0,0,0,0,2,0,0,0,1,64,0,0,8,0,0,0");
    E("    db 0,0,0,7,66,0,16,0,6,0,0,0,58,0,16,0");
    E("    db 3,0,0,0,1,64,0,0,0,0,128,63,54,0,0,5");
    E("    db 18,0,16,0,8,0,0,0,26,0,16,0,6,0,0,0");
    E("    db 54,0,0,5,18,0,16,0,9,0,0,0,10,0,16,0");
    E("    db 7,0,0,0,54,0,0,5,66,0,16,0,0,0,0,0");
    E("    db 58,0,16,0,6,0,0,0,54,0,0,5,226,0,16,0");
    E("    db 8,0,0,0,166,7,16,0,7,0,0,0,54,0,0,5");
    E("    db 18,0,16,0,10,0,0,0,1,64,0,0,0,0,0,0");
    E("    db 48,0,0,1,80,0,0,7,34,0,16,0,10,0,0,0");
    E("    db 10,0,16,0,10,0,0,0,10,0,16,0,4,0,0,0");
    E("    db 3,0,4,3,26,0,16,0,10,0,0,0,54,0,0,5");
    E("    db 18,0,16,0,0,0,0,0,10,0,16,0,8,0,0,0");
    E("    db 54,0,0,5,34,0,16,0,0,0,0,0,10,0,16,0");
    E("    db 9,0,0,0,0,0,0,11,226,0,16,0,10,0,0,0");
    E("    db 6,9,16,0,0,0,0,0,6,137,32,134,65,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,10,0,16,0,10,0,0,0");
    E("    db 16,0,0,7,18,0,16,0,11,0,0,0,150,7,16,0");
    E("    db 10,0,0,0,150,7,16,0,10,0,0,0,75,0,0,5");
    E("    db 18,0,16,0,11,0,0,0,10,0,16,0,11,0,0,0");
    E("    db 0,0,0,10,34,0,16,0,11,0,0,0,58,0,16,0");
    E("    db 1,0,0,0,58,128,32,6,0,0,0,0,3,0,0,0");
    E("    db 10,0,16,0,10,0,0,0,49,0,0,7,66,0,16,0");
    E("    db 11,0,0,0,10,0,16,0,11,0,0,0,26,0,16,0");
    E("    db 11,0,0,0,49,0,0,7,130,0,16,0,11,0,0,0");
    E("    db 1,64,0,0,172,197,39,55,10,0,16,0,11,0,0,0");
    E("    db 1,0,0,7,66,0,16,0,11,0,0,0,58,0,16,0");
    E("    db 11,0,0,0,42,0,16,0,11,0,0,0,14,0,0,7");
    E("    db 226,0,16,0,10,0,0,0,86,14,16,0,10,0,0,0");
    E("    db 6,0,16,0,11,0,0,0,0,0,0,8,18,0,16,0");
    E("    db 11,0,0,0,10,0,16,128,65,0,0,0,11,0,0,0");
    E("    db 26,0,16,0,11,0,0,0,50,0,0,9,114,0,16,0");
    E("    db 12,0,0,0,150,7,16,0,10,0,0,0,6,0,16,0");
    E("    db 11,0,0,0,70,2,16,0,0,0,0,0,54,0,0,5");
    E("    db 114,0,16,0,8,0,0,0,118,14,16,0,8,0,0,0");
    E("    db 16,0,0,7,18,0,16,0,11,0,0,0,70,2,16,0");
    E("    db 8,0,0,0,150,7,16,0,10,0,0,0,49,0,0,7");
    E("    db 34,0,16,0,11,0,0,0,10,0,16,0,11,0,0,0");
    E("    db 1,64,0,0,0,0,0,0,56,0,0,7,18,0,16,0");
    E("    db 11,0,0,0,42,0,16,0,6,0,0,0,10,0,16,0");
    E("    db 11,0,0,0,50,0,0,10,226,0,16,0,10,0,0,0");
    E("    db 6,0,16,128,65,0,0,0,11,0,0,0,86,14,16,0");
    E("    db 10,0,0,0,6,9,16,0,8,0,0,0,55,0,0,9");
    E("    db 226,0,16,0,10,0,0,0,86,5,16,0,11,0,0,0");
    E("    db 86,14,16,0,10,0,0,0,6,9,16,0,8,0,0,0");
    E("    db 55,0,0,9,98,0,16,0,8,0,0,0,166,10,16,0");
    E("    db 11,0,0,0,166,11,16,0,10,0,0,0,86,6,16,0");
    E("    db 8,0,0,0,54,0,0,5,130,0,16,0,12,0,0,0");
    E("    db 26,0,16,0,10,0,0,0,54,0,0,5,130,0,16,0");
    E("    db 0,0,0,0,10,0,16,0,8,0,0,0,55,0,0,9");
    E("    db 242,0,16,0,0,0,0,0,166,10,16,0,11,0,0,0");
    E("    db 70,14,16,0,12,0,0,0,70,14,16,0,0,0,0,0");
    E("    db 30,0,0,7,18,0,16,0,10,0,0,0,10,0,16,0");
    E("    db 10,0,0,0,1,64,0,0,1,0,0,0,54,0,0,5");
    E("    db 18,0,16,0,9,0,0,0,26,0,16,0,0,0,0,0");
    E("    db 54,0,0,5,146,0,16,0,8,0,0,0,6,12,16,0");
    E("    db 0,0,0,0,22,0,0,1,51,0,0,8,130,0,16,0");
    E("    db 0,0,0,0,58,0,16,0,3,0,0,0,58,128,32,0");
    E("    db 0,0,0,0,12,0,0,0,0,0,0,8,130,0,16,0");
    E("    db 3,0,0,0,10,0,16,0,5,0,0,0,10,0,16,128");
    E("    db 65,0,0,0,8,0,0,0,49,0,0,7,130,0,16,0");
    E("    db 3,0,0,0,1,64,0,0,0,0,0,0,58,0,16,0");
    E("    db 3,0,0,0,49,0,0,7,18,0,16,0,4,0,0,0");
    E("    db 58,0,16,0,8,0,0,0,1,64,0,0,0,0,0,0");
    E("    db 56,0,0,8,34,0,16,0,6,0,0,0,58,0,16,0");
    E("    db 0,0,0,0,58,0,16,128,65,0,0,0,8,0,0,0");
    E("    db 56,0,0,10,194,0,16,0,6,0,0,0,86,9,16,0");
    E("    db 8,0,0,0,2,64,0,0,0,0,0,0,0,0,0,0");
    E("    db 72,225,122,63,72,225,122,63,55,0,0,9,226,0,16,0");
    E("    db 5,0,0,0,6,0,16,0,4,0,0,0,86,14,16,0");
    E("    db 6,0,0,0,246,9,16,0,8,0,0,0,55,0,0,9");
    E("    db 242,0,16,0,5,0,0,0,246,15,16,0,3,0,0,0");
    E("    db 70,14,16,0,5,0,0,0,198,9,16,0,8,0,0,0");
    E("    db 0,0,0,8,130,0,16,0,3,0,0,0,42,0,16,128");
    E("    db 65,0,0,0,4,0,0,0,10,0,16,0,5,0,0,0");
    E("    db 49,0,0,7,18,0,16,0,4,0,0,0,1,64,0,0");
    E("    db 0,0,0,0,58,0,16,0,3,0,0,0,0,0,0,8");
    E("    db 18,0,16,0,7,0,0,0,58,0,16,128,65,0,0,0");
    E("    db 3,0,0,0,10,0,16,0,5,0,0,0,49,0,0,7");
    E("    db 130,0,16,0,3,0,0,0,1,64,0,0,0,0,0,0");
    E("    db 26,0,16,0,5,0,0,0,56,0,0,8,34,0,16,0");
    E("    db 6,0,0,0,58,0,16,0,0,0,0,0,26,0,16,128");
    E("    db 65,0,0,0,5,0,0,0,56,0,0,10,194,0,16,0");
    E("    db 6,0,0,0,166,14,16,0,5,0,0,0,2,64,0,0");
    E("    db 0,0,0,0,0,0,0,0,72,225,122,63,72,225,122,63");
    E("    db 55,0,0,9,226,0,16,0,7,0,0,0,246,15,16,0");
    E("    db 3,0,0,0,86,14,16,0,6,0,0,0,86,14,16,0");
    E("    db 5,0,0,0,55,0,0,9,242,0,16,0,5,0,0,0");
    E("    db 6,0,16,0,4,0,0,0,70,14,16,0,7,0,0,0");
    E("    db 70,14,16,0,5,0,0,0,55,0,0,9,242,0,16,0");
    E("    db 5,0,0,0,86,5,16,0,4,0,0,0,70,14,16,0");
    E("    db 5,0,0,0,198,9,16,0,8,0,0,0,0,0,0,8");
    E("    db 130,0,16,0,3,0,0,0,58,0,16,128,65,0,0,0");
    E("    db 4,0,0,0,10,0,16,0,9,0,0,0,49,0,0,7");
    E("    db 18,0,16,0,4,0,0,0,1,64,0,0,0,0,0,0");
    E("    db 58,0,16,0,3,0,0,0,0,0,0,8,18,0,16,0");
    E("    db 7,0,0,0,58,0,16,128,65,0,0,0,3,0,0,0");
    E("    db 10,0,16,0,9,0,0,0,49,0,0,7,130,0,16,0");
    E("    db 3,0,0,0,1,64,0,0,0,0,0,0,42,0,16,0");
    E("    db 5,0,0,0,56,0,0,8,66,0,16,0,4,0,0,0");
    E("    db 58,0,16,0,0,0,0,0,42,0,16,128,65,0,0,0");
    E("    db 5,0,0,0,56,0,0,10,162,0,16,0,4,0,0,0");
    E("    db 86,13,16,0,5,0,0,0,2,64,0,0,0,0,0,0");
    E("    db 72,225,122,63,0,0,0,0,72,225,122,63,55,0,0,9");
    E("    db 226,0,16,0,7,0,0,0,246,15,16,0,3,0,0,0");
    E("    db 86,14,16,0,4,0,0,0,86,14,16,0,5,0,0,0");
    E("    db 54,0,0,5,226,0,16,0,9,0,0,0,86,14,16,0");
    E("    db 5,0,0,0,55,0,0,9,242,0,16,0,4,0,0,0");
    E("    db 6,0,16,0,4,0,0,0,70,14,16,0,7,0,0,0");
    E("    db 70,14,16,0,9,0,0,0,55,0,0,9,242,0,16,0");
    E("    db 1,0,0,0,6,0,16,0,6,0,0,0,134,7,16,0");
    E("    db 4,0,0,0,134,7,16,0,9,0,0,0,54,0,0,5");
    E("    db 18,0,16,0,0,0,0,0,10,0,16,0,5,0,0,0");
    E("    db 54,0,0,5,34,0,16,0,0,0,0,0,10,0,16,0");
    E("    db 1,0,0,0,54,0,0,5,114,0,16,0,1,0,0,0");
    E("    db 118,14,16,0,1,0,0,0,21,0,0,1,168,0,0,8");
    E("    db 114,224,17,0,0,0,0,0,10,0,2,0,1,64,0,0");
    E("    db 0,0,0,0,70,2,16,0,0,0,0,0,168,0,0,8");
    E("    db 114,224,17,0,0,0,0,0,10,0,2,0,1,64,0,0");
    E("    db 16,0,0,0,70,2,16,0,1,0,0,0,168,0,0,8");
    E("    db 242,224,17,0,0,0,0,0,10,0,2,0,1,64,0,0");
    E("    db 32,0,0,0,70,14,16,0,2,0,0,0,168,0,0,8");
    E("    db 114,224,17,0,0,0,0,0,10,0,2,0,1,64,0,0");
    E("    db 48,0,0,0,70,2,16,0,3,0,0,0,62,0,0,1");
    E("    db 83,84,65,84,148,0,0,0,125,0,0,0,13,0,0,0");
    E("    db 0,0,0,0,1,0,0,0,61,0,0,0,3,0,0,0");
    E("    db 6,0,0,0,3,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,5,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,18,0,0,0,15,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,5,0,0,0");
    E("_gpu_cs_integrate_blob_len equ 5628");
    E("align 16");
    E("_gpu_cs_clear_blob:  ; 1932 bytes DXBC (physics compute: ClearImpulses)");
    E("    db 68,88,66,67,222,94,9,132,169,207,86,213,127,239,226,206");
    E("    db 129,115,108,56,1,0,0,0,140,7,0,0,5,0,0,0");
    E("    db 52,0,0,0,240,5,0,0,0,6,0,0,16,6,0,0");
    E("    db 240,6,0,0,82,68,69,70,180,5,0,0,2,0,0,0");
    E("    db 156,0,0,0,2,0,0,0,60,0,0,0,0,5,83,67");
    E("    db 0,1,0,0,140,5,0,0,82,68,49,49,60,0,0,0");
    E("    db 24,0,0,0,32,0,0,0,40,0,0,0,36,0,0,0");
    E("    db 12,0,0,0,0,0,0,0,124,0,0,0,6,0,0,0");
    E("    db 6,0,0,0,1,0,0,0,32,0,0,0,1,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,137,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,73,109,112,117,108,115,101,65");
    E("    db 99,99,117,109,0,80,104,121,115,105,99,115,67,111,110,115");
    E("    db 116,97,110,116,115,0,171,171,137,0,0,0,14,0,0,0");
    E("    db 204,0,0,0,208,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 124,0,0,0,1,0,0,0,124,4,0,0,32,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,252,2,0,0,0,0,0,0");
    E("    db 12,0,0,0,0,0,0,0,12,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 48,3,0,0,12,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 64,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,100,3,0,0,16,0,0,0");
    E("    db 4,0,0,0,2,0,0,0,116,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 152,3,0,0,20,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 64,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,163,3,0,0,24,0,0,0");
    E("    db 4,0,0,0,0,0,0,0,64,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 175,3,0,0,28,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 64,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,183,3,0,0,32,0,0,0");
    E("    db 4,0,0,0,0,0,0,0,64,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 201,3,0,0,36,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 116,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,215,3,0,0,48,0,0,0");
    E("    db 128,0,0,0,0,0,0,0,232,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 12,4,0,0,176,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 64,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,21,4,0,0,180,0,0,0");
    E("    db 4,0,0,0,0,0,0,0,64,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 39,4,0,0,184,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 64,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,57,4,0,0,188,0,0,0");
    E("    db 4,0,0,0,0,0,0,0,64,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 76,4,0,0,192,0,0,0,16,0,0,0,0,0,0,0");
    E("    db 88,4,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,103,114,97,118,105,116,121,0");
    E("    db 102,108,111,97,116,51,0,171,1,0,3,0,1,0,3,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,4,3,0,0,100,101,108,116");
    E("    db 97,84,105,109,101,0,102,108,111,97,116,0,0,0,3,0");
    E("    db 1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,58,3,0,0");
    E("    db 98,111,100,121,67,111,117,110,116,0,100,119,111,114,100,0");
    E("    db 0,0,19,0,1,0,1,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 110,3,0,0,108,105,110,101,97,114,68,97,109,112,0,97");
    E("    db 110,103,117,108,97,114,68,97,109,112,0,103,114,111,117,110");
    E("    db 100,89,0,103,114,111,117,110,100,82,101,115,116,105,116,117");
    E("    db 116,105,111,110,0,99,111,108,108,105,100,101,114,67,111,117");
    E("    db 110,116,0,99,111,108,108,105,100,101,114,115,0,102,108,111");
    E("    db 97,116,52,0,1,0,3,0,1,0,4,0,8,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,225,3,0,0,102,114,105,99,116,105,111,110");
    E("    db 0,115,108,101,101,112,76,105,110,84,104,114,101,115,104,111");
    E("    db 108,100,0,115,108,101,101,112,65,110,103,84,104,114,101,115");
    E("    db 104,111,108,100,0,115,108,101,101,112,84,105,109,101,84,104");
    E("    db 114,101,115,104,111,108,100,0,119,97,108,108,66,111,117,110");
    E("    db 100,115,0,171,1,0,3,0,1,0,4,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,225,3,0,0,164,4,0,0,0,0,0,0");
    E("    db 32,0,0,0,2,0,0,0,104,5,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 36,69,108,101,109,101,110,116,0,73,109,112,117,108,115,101");
    E("    db 65,99,99,117,109,69,110,116,114,121,0,108,105,110,101,97");
    E("    db 114,73,109,112,117,108,115,101,0,171,171,171,1,0,3,0");
    E("    db 1,0,3,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,4,3,0,0");
    E("    db 95,112,97,100,48,0,171,171,0,0,3,0,1,0,1,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,58,3,0,0,97,110,103,117");
    E("    db 108,97,114,73,109,112,117,108,115,101,0,95,112,97,100,49");
    E("    db 0,171,171,171,191,4,0,0,208,4,0,0,0,0,0,0");
    E("    db 244,4,0,0,252,4,0,0,12,0,0,0,32,5,0,0");
    E("    db 208,4,0,0,16,0,0,0,47,5,0,0,252,4,0,0");
    E("    db 28,0,0,0,5,0,0,0,1,0,8,0,0,0,4,0");
    E("    db 56,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,173,4,0,0,77,105,99,114,111,115,111,102");
    E("    db 116,32,40,82,41,32,72,76,83,76,32,83,104,97,100,101");
    E("    db 114,32,67,111,109,112,105,108,101,114,32,49,48,46,49,0");
    E("    db 73,83,71,78,8,0,0,0,0,0,0,0,8,0,0,0");
    E("    db 79,83,71,78,8,0,0,0,0,0,0,0,8,0,0,0");
    E("    db 83,72,69,88,216,0,0,0,80,0,5,0,54,0,0,0");
    E("    db 106,8,0,1,89,0,0,4,70,142,32,0,0,0,0,0");
    E("    db 2,0,0,0,158,0,0,4,0,224,17,0,1,0,0,0");
    E("    db 32,0,0,0,95,0,0,2,18,0,2,0,104,0,0,2");
    E("    db 1,0,0,0,155,0,0,4,0,1,0,0,1,0,0,0");
    E("    db 1,0,0,0,80,0,0,7,18,0,16,0,0,0,0,0");
    E("    db 10,0,2,0,10,128,32,0,0,0,0,0,1,0,0,0");
    E("    db 31,0,4,3,10,0,16,0,0,0,0,0,62,0,0,1");
    E("    db 21,0,0,1,168,0,0,11,114,224,17,0,1,0,0,0");
    E("    db 10,0,2,0,1,64,0,0,0,0,0,0,2,64,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 168,0,0,11,114,224,17,0,1,0,0,0,10,0,2,0");
    E("    db 1,64,0,0,16,0,0,0,2,64,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,62,0,0,1");
    E("    db 83,84,65,84,148,0,0,0,7,0,0,0,1,0,0,0");
    E("    db 0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,2,0,0,0,1,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,2,0,0,0");
    E("_gpu_cs_clear_blob_len equ 1932");
    E("align 16");
    E("_gpu_cs_resolve_blob:  ; 7340 bytes DXBC (physics compute: ResolveBodyPairs (body-vs-body))");
    E("    db 68,88,66,67,239,198,88,165,102,139,46,143,246,69,180,69");
    E("    db 216,132,33,226,1,0,0,0,172,28,0,0,5,0,0,0");
    E("    db 52,0,0,0,244,7,0,0,4,8,0,0,20,8,0,0");
    E("    db 16,28,0,0,82,68,69,70,184,7,0,0,3,0,0,0");
    E("    db 200,0,0,0,3,0,0,0,60,0,0,0,0,5,83,67");
    E("    db 0,1,0,0,144,7,0,0,82,68,49,49,60,0,0,0");
    E("    db 24,0,0,0,32,0,0,0,40,0,0,0,36,0,0,0");
    E("    db 12,0,0,0,0,0,0,0,156,0,0,0,5,0,0,0");
    E("    db 6,0,0,0,1,0,0,0,96,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,167,0,0,0,6,0,0,0");
    E("    db 6,0,0,0,1,0,0,0,32,0,0,0,1,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,180,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,66,111,100,105,101,115,82,101");
    E("    db 97,100,0,73,109,112,117,108,115,101,65,99,99,117,109,0");
    E("    db 80,104,121,115,105,99,115,67,111,110,115,116,97,110,116,115");
    E("    db 0,171,171,171,180,0,0,0,14,0,0,0,16,1,0,0");
    E("    db 208,0,0,0,0,0,0,0,0,0,0,0,156,0,0,0");
    E("    db 1,0,0,0,192,4,0,0,96,0,0,0,0,0,0,0");
    E("    db 3,0,0,0,167,0,0,0,1,0,0,0,216,6,0,0");
    E("    db 32,0,0,0,0,0,0,0,3,0,0,0,64,3,0,0");
    E("    db 0,0,0,0,12,0,0,0,0,0,0,0,80,3,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,116,3,0,0,12,0,0,0,4,0,0,0");
    E("    db 2,0,0,0,132,3,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,168,3,0,0");
    E("    db 16,0,0,0,4,0,0,0,2,0,0,0,184,3,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,220,3,0,0,20,0,0,0,4,0,0,0");
    E("    db 0,0,0,0,132,3,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,231,3,0,0");
    E("    db 24,0,0,0,4,0,0,0,0,0,0,0,132,3,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,243,3,0,0,28,0,0,0,4,0,0,0");
    E("    db 0,0,0,0,132,3,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,251,3,0,0");
    E("    db 32,0,0,0,4,0,0,0,0,0,0,0,132,3,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,13,4,0,0,36,0,0,0,4,0,0,0");
    E("    db 0,0,0,0,184,3,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,27,4,0,0");
    E("    db 48,0,0,0,128,0,0,0,0,0,0,0,44,4,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,80,4,0,0,176,0,0,0,4,0,0,0");
    E("    db 2,0,0,0,132,3,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,89,4,0,0");
    E("    db 180,0,0,0,4,0,0,0,0,0,0,0,132,3,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,107,4,0,0,184,0,0,0,4,0,0,0");
    E("    db 0,0,0,0,132,3,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,125,4,0,0");
    E("    db 188,0,0,0,4,0,0,0,0,0,0,0,132,3,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,144,4,0,0,192,0,0,0,16,0,0,0");
    E("    db 0,0,0,0,156,4,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,103,114,97,118");
    E("    db 105,116,121,0,102,108,111,97,116,51,0,171,1,0,3,0");
    E("    db 1,0,3,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,72,3,0,0");
    E("    db 100,101,108,116,97,84,105,109,101,0,102,108,111,97,116,0");
    E("    db 0,0,3,0,1,0,1,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 126,3,0,0,98,111,100,121,67,111,117,110,116,0,100,119");
    E("    db 111,114,100,0,0,0,19,0,1,0,1,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,178,3,0,0,108,105,110,101,97,114,68,97");
    E("    db 109,112,0,97,110,103,117,108,97,114,68,97,109,112,0,103");
    E("    db 114,111,117,110,100,89,0,103,114,111,117,110,100,82,101,115");
    E("    db 116,105,116,117,116,105,111,110,0,99,111,108,108,105,100,101");
    E("    db 114,67,111,117,110,116,0,99,111,108,108,105,100,101,114,115");
    E("    db 0,102,108,111,97,116,52,0,1,0,3,0,1,0,4,0");
    E("    db 8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,37,4,0,0,102,114,105,99");
    E("    db 116,105,111,110,0,115,108,101,101,112,76,105,110,84,104,114");
    E("    db 101,115,104,111,108,100,0,115,108,101,101,112,65,110,103,84");
    E("    db 104,114,101,115,104,111,108,100,0,115,108,101,101,112,84,105");
    E("    db 109,101,84,104,114,101,115,104,111,108,100,0,119,97,108,108");
    E("    db 66,111,117,110,100,115,0,171,1,0,3,0,1,0,4,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,37,4,0,0,232,4,0,0");
    E("    db 0,0,0,0,96,0,0,0,2,0,0,0,180,6,0,0");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,36,69,108,101,109,101,110,116,0,82,105,103");
    E("    db 105,100,66,111,100,121,0,112,111,115,105,116,105,111,110,0");
    E("    db 1,0,3,0,1,0,3,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 72,3,0,0,105,110,118,77,97,115,115,0,0,0,3,0");
    E("    db 1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,126,3,0,0");
    E("    db 118,101,108,111,99,105,116,121,0,114,97,100,105,117,115,0");
    E("    db 111,114,105,101,110,116,97,116,105,111,110,0,1,0,3,0");
    E("    db 1,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,37,4,0,0");
    E("    db 97,110,103,117,108,97,114,86,101,108,0,114,101,115,116,105");
    E("    db 116,117,116,105,111,110,0,102,108,97,103,115,0,171,171,171");
    E("    db 0,0,19,0,1,0,1,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 178,3,0,0,95,112,97,100,0,105,110,118,73,110,101,114");
    E("    db 116,105,97,0,115,108,101,101,112,84,105,109,101,114,0,95");
    E("    db 112,97,100,50,0,102,108,111,97,116,50,0,1,0,3,0");
    E("    db 1,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,249,5,0,0");
    E("    db 251,4,0,0,4,5,0,0,0,0,0,0,40,5,0,0");
    E("    db 48,5,0,0,12,0,0,0,84,5,0,0,4,5,0,0");
    E("    db 16,0,0,0,93,5,0,0,48,5,0,0,28,0,0,0");
    E("    db 100,5,0,0,112,5,0,0,32,0,0,0,148,5,0,0");
    E("    db 4,5,0,0,48,0,0,0,159,5,0,0,48,5,0,0");
    E("    db 60,0,0,0,171,5,0,0,180,5,0,0,64,0,0,0");
    E("    db 216,5,0,0,4,5,0,0,68,0,0,0,221,5,0,0");
    E("    db 48,5,0,0,80,0,0,0,232,5,0,0,48,5,0,0");
    E("    db 84,0,0,0,243,5,0,0,0,6,0,0,88,0,0,0");
    E("    db 5,0,0,0,1,0,24,0,0,0,12,0,36,6,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 241,4,0,0,232,4,0,0,0,0,0,0,32,0,0,0");
    E("    db 2,0,0,0,108,7,0,0,0,0,0,0,255,255,255,255");
    E("    db 0,0,0,0,255,255,255,255,0,0,0,0,73,109,112,117");
    E("    db 108,115,101,65,99,99,117,109,69,110,116,114,121,0,108,105");
    E("    db 110,101,97,114,73,109,112,117,108,115,101,0,95,112,97,100");
    E("    db 48,0,97,110,103,117,108,97,114,73,109,112,117,108,115,101");
    E("    db 0,95,112,97,100,49,0,171,18,7,0,0,4,5,0,0");
    E("    db 0,0,0,0,32,7,0,0,48,5,0,0,12,0,0,0");
    E("    db 38,7,0,0,4,5,0,0,16,0,0,0,53,7,0,0");
    E("    db 48,5,0,0,28,0,0,0,5,0,0,0,1,0,8,0");
    E("    db 0,0,4,0,60,7,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,7,0,0,77,105,99,114");
    E("    db 111,115,111,102,116,32,40,82,41,32,72,76,83,76,32,83");
    E("    db 104,97,100,101,114,32,67,111,109,112,105,108,101,114,32,49");
    E("    db 48,46,49,0,73,83,71,78,8,0,0,0,0,0,0,0");
    E("    db 8,0,0,0,79,83,71,78,8,0,0,0,0,0,0,0");
    E("    db 8,0,0,0,83,72,69,88,244,19,0,0,80,0,5,0");
    E("    db 253,4,0,0,106,8,0,1,89,0,0,4,70,142,32,0");
    E("    db 0,0,0,0,12,0,0,0,162,0,0,4,0,112,16,0");
    E("    db 0,0,0,0,96,0,0,0,158,0,0,4,0,224,17,0");
    E("    db 1,0,0,0,32,0,0,0,95,0,0,2,18,32,2,0");
    E("    db 95,0,0,2,18,0,2,0,104,0,0,2,20,0,0,0");
    E("    db 160,0,0,5,0,240,17,0,0,0,0,0,64,0,0,0");
    E("    db 0,1,0,0,155,0,0,4,0,1,0,0,1,0,0,0");
    E("    db 1,0,0,0,79,0,0,7,18,0,16,0,0,0,0,0");
    E("    db 10,0,2,0,10,128,32,0,0,0,0,0,1,0,0,0");
    E("    db 31,0,4,3,10,0,16,0,0,0,0,0,167,0,0,138");
    E("    db 2,3,3,128,131,153,25,0,242,0,16,0,1,0,0,0");
    E("    db 10,0,2,0,1,64,0,0,0,0,0,0,70,126,16,0");
    E("    db 0,0,0,0,167,0,0,138,2,3,3,128,131,153,25,0");
    E("    db 242,0,16,0,2,0,0,0,10,0,2,0,1,64,0,0");
    E("    db 16,0,0,0,70,126,16,0,0,0,0,0,167,0,0,138");
    E("    db 2,3,3,128,131,153,25,0,242,0,16,0,3,0,0,0");
    E("    db 10,0,2,0,1,64,0,0,48,0,0,0,70,126,16,0");
    E("    db 0,0,0,0,167,0,0,138,2,3,3,128,131,153,25,0");
    E("    db 34,0,16,0,0,0,0,0,10,0,2,0,1,64,0,0");
    E("    db 64,0,0,0,6,112,16,0,0,0,0,0,167,0,0,138");
    E("    db 2,3,3,128,131,153,25,0,66,0,16,0,0,0,0,0");
    E("    db 10,0,2,0,1,64,0,0,80,0,0,0,6,112,16,0");
    E("    db 0,0,0,0,1,0,0,7,34,0,16,0,0,0,0,0");
    E("    db 26,0,16,0,0,0,0,0,1,64,0,0,1,0,0,0");
    E("    db 39,0,0,7,34,0,16,0,0,0,0,0,26,0,16,0");
    E("    db 0,0,0,0,1,64,0,0,0,0,0,0,18,0,0,1");
    E("    db 54,0,0,8,242,0,16,0,1,0,0,0,2,64,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 54,0,0,8,242,0,16,0,2,0,0,0,2,64,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 54,0,0,8,242,0,16,0,3,0,0,0,2,64,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 54,0,0,8,98,0,16,0,0,0,0,0,2,64,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 21,0,0,1,30,0,0,8,130,0,16,0,0,0,0,0");
    E("    db 10,128,32,0,0,0,0,0,1,0,0,0,1,64,0,0");
    E("    db 255,0,0,0,85,0,0,7,130,0,16,0,0,0,0,0");
    E("    db 58,0,16,0,0,0,0,0,1,64,0,0,8,0,0,0");
    E("    db 52,0,0,8,18,0,16,0,4,0,0,0,58,128,32,0");
    E("    db 0,0,0,0,0,0,0,0,1,64,0,0,172,197,39,55");
    E("    db 54,0,0,8,242,0,16,0,5,0,0,0,2,64,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 54,0,0,8,98,0,16,0,6,0,0,0,2,64,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 54,0,0,6,34,0,16,0,4,0,0,0,10,128,32,0");
    E("    db 0,0,0,0,1,0,0,0,54,0,0,5,66,0,16,0");
    E("    db 4,0,0,0,26,0,16,0,0,0,0,0,54,0,0,5");
    E("    db 130,0,16,0,4,0,0,0,1,64,0,0,0,0,0,0");
    E("    db 48,0,0,1,80,0,0,7,130,0,16,0,6,0,0,0");
    E("    db 58,0,16,0,4,0,0,0,58,0,16,0,0,0,0,0");
    E("    db 3,0,4,3,58,0,16,0,6,0,0,0,140,0,0,10");
    E("    db 130,0,16,0,6,0,0,0,1,64,0,0,24,0,0,0");
    E("    db 1,64,0,0,8,0,0,0,58,0,16,0,4,0,0,0");
    E("    db 10,32,2,0,79,0,0,7,18,0,16,0,7,0,0,0");
    E("    db 58,0,16,0,6,0,0,0,26,0,16,0,4,0,0,0");
    E("    db 31,0,4,3,10,0,16,0,7,0,0,0,167,0,0,139");
    E("    db 2,3,3,128,131,153,25,0,242,0,16,0,7,0,0,0");
    E("    db 58,0,16,0,6,0,0,0,1,64,0,0,0,0,0,0");
    E("    db 70,126,16,0,0,0,0,0,167,0,0,139,2,3,3,128");
    E("    db 131,153,25,0,242,0,16,0,8,0,0,0,58,0,16,0");
    E("    db 6,0,0,0,1,64,0,0,16,0,0,0,70,126,16,0");
    E("    db 0,0,0,0,167,0,0,139,2,3,3,128,131,153,25,0");
    E("    db 242,0,16,0,9,0,0,0,58,0,16,0,6,0,0,0");
    E("    db 1,64,0,0,48,0,0,0,70,126,16,0,0,0,0,0");
    E("    db 167,0,0,139,2,3,3,128,131,153,25,0,34,0,16,0");
    E("    db 10,0,0,0,58,0,16,0,6,0,0,0,1,64,0,0");
    E("    db 64,0,0,0,6,112,16,0,0,0,0,0,167,0,0,139");
    E("    db 2,3,3,128,131,153,25,0,130,0,16,0,11,0,0,0");
    E("    db 58,0,16,0,6,0,0,0,1,64,0,0,80,0,0,0");
    E("    db 6,112,16,0,0,0,0,0,54,0,0,5,114,0,16,0");
    E("    db 12,0,0,0,70,2,16,0,7,0,0,0,54,0,0,5");
    E("    db 130,0,16,0,12,0,0,0,58,0,16,0,8,0,0,0");
    E("    db 54,0,0,5,114,0,16,0,7,0,0,0,70,2,16,0");
    E("    db 8,0,0,0,54,0,0,5,114,0,16,0,11,0,0,0");
    E("    db 70,2,16,0,9,0,0,0,54,0,0,5,18,0,16,0");
    E("    db 10,0,0,0,58,0,16,0,9,0,0,0,18,0,0,1");
    E("    db 54,0,0,8,242,0,16,0,12,0,0,0,2,64,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 54,0,0,8,242,0,16,0,7,0,0,0,2,64,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 54,0,0,8,242,0,16,0,11,0,0,0,2,64,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 54,0,0,8,50,0,16,0,10,0,0,0,2,64,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 21,0,0,1,168,0,0,8,242,240,17,0,0,0,0,0");
    E("    db 10,32,2,0,1,64,0,0,0,0,0,0,70,14,16,0");
    E("    db 12,0,0,0,168,0,0,8,242,240,17,0,0,0,0,0");
    E("    db 10,32,2,0,1,64,0,0,16,0,0,0,70,14,16,0");
    E("    db 7,0,0,0,168,0,0,8,242,240,17,0,0,0,0,0");
    E("    db 10,32,2,0,1,64,0,0,32,0,0,0,70,14,16,0");
    E("    db 11,0,0,0,168,0,0,8,50,240,17,0,0,0,0,0");
    E("    db 10,32,2,0,1,64,0,0,48,0,0,0,70,0,16,0");
    E("    db 10,0,0,0,190,24,0,1,31,0,4,3,42,0,16,0");
    E("    db 4,0,0,0,41,0,0,7,130,0,16,0,6,0,0,0");
    E("    db 58,0,16,0,4,0,0,0,1,64,0,0,8,0,0,0");
    E("    db 30,0,0,8,18,0,16,0,7,0,0,0,26,0,16,0");
    E("    db 4,0,0,0,58,0,16,128,65,0,0,0,6,0,0,0");
    E("    db 84,0,0,7,18,0,16,0,7,0,0,0,10,0,16,0");
    E("    db 7,0,0,0,1,64,0,0,0,1,0,0,54,0,0,5");
    E("    db 226,0,16,0,7,0,0,0,6,9,16,0,5,0,0,0");
    E("    db 54,0,0,5,98,0,16,0,8,0,0,0,86,6,16,0");
    E("    db 6,0,0,0,54,0,0,5,18,0,16,0,8,0,0,0");
    E("    db 58,0,16,0,5,0,0,0,54,0,0,5,130,0,16,0");
    E("    db 8,0,0,0,1,64,0,0,0,0,0,0,48,0,0,1");
    E("    db 80,0,0,7,18,0,16,0,9,0,0,0,58,0,16,0");
    E("    db 8,0,0,0,10,0,16,0,7,0,0,0,3,0,4,3");
    E("    db 10,0,16,0,9,0,0,0,30,0,0,7,18,0,16,0");
    E("    db 9,0,0,0,58,0,16,0,6,0,0,0,58,0,16,0");
    E("    db 8,0,0,0,32,0,0,6,18,0,16,0,9,0,0,0");
    E("    db 10,0,16,0,9,0,0,0,10,0,2,0,31,0,4,3");
    E("    db 10,0,16,0,9,0,0,0,30,0,0,7,18,0,16,0");
    E("    db 9,0,0,0,58,0,16,0,8,0,0,0,1,64,0,0");
    E("    db 1,0,0,0,54,0,0,5,130,0,16,0,8,0,0,0");
    E("    db 10,0,16,0,9,0,0,0,7,0,0,1,21,0,0,1");
    E("    db 167,0,0,9,242,0,16,0,9,0,0,0,58,0,16,0");
    E("    db 8,0,0,0,1,64,0,0,0,0,0,0,70,254,17,0");
    E("    db 0,0,0,0,167,0,0,9,242,0,16,0,10,0,0,0");
    E("    db 58,0,16,0,8,0,0,0,1,64,0,0,16,0,0,0");
    E("    db 70,254,17,0,0,0,0,0,167,0,0,9,242,0,16,0");
    E("    db 11,0,0,0,58,0,16,0,8,0,0,0,1,64,0,0");
    E("    db 32,0,0,0,70,254,17,0,0,0,0,0,167,0,0,9");
    E("    db 50,0,16,0,12,0,0,0,58,0,16,0,8,0,0,0");
    E("    db 1,64,0,0,48,0,0,0,70,240,17,0,0,0,0,0");
    E("    db 1,0,0,7,34,0,16,0,12,0,0,0,26,0,16,0");
    E("    db 12,0,0,0,1,64,0,0,1,0,0,0,31,0,0,3");
    E("    db 26,0,16,0,12,0,0,0,30,0,0,7,34,0,16,0");
    E("    db 12,0,0,0,58,0,16,0,8,0,0,0,1,64,0,0");
    E("    db 1,0,0,0,54,0,0,5,130,0,16,0,8,0,0,0");
    E("    db 26,0,16,0,12,0,0,0,7,0,0,1,21,0,0,1");
    E("    db 0,0,0,8,114,0,16,0,9,0,0,0,70,2,16,0");
    E("    db 1,0,0,0,70,2,16,128,65,0,0,0,9,0,0,0");
    E("    db 16,0,0,7,34,0,16,0,12,0,0,0,70,2,16,0");
    E("    db 9,0,0,0,70,2,16,0,9,0,0,0,75,0,0,5");
    E("    db 34,0,16,0,12,0,0,0,26,0,16,0,12,0,0,0");
    E("    db 0,0,0,7,66,0,16,0,12,0,0,0,58,0,16,0");
    E("    db 2,0,0,0,58,0,16,0,9,0,0,0,29,0,0,7");
    E("    db 130,0,16,0,12,0,0,0,26,0,16,0,12,0,0,0");
    E("    db 42,0,16,0,12,0,0,0,29,0,0,7,18,0,16,0");
    E("    db 13,0,0,0,1,64,0,0,172,197,39,55,26,0,16,0");
    E("    db 12,0,0,0,60,0,0,7,130,0,16,0,12,0,0,0");
    E("    db 58,0,16,0,12,0,0,0,10,0,16,0,13,0,0,0");
    E("    db 31,0,4,3,58,0,16,0,12,0,0,0,30,0,0,7");
    E("    db 130,0,16,0,12,0,0,0,58,0,16,0,8,0,0,0");
    E("    db 1,64,0,0,1,0,0,0,54,0,0,5,130,0,16,0");
    E("    db 8,0,0,0,58,0,16,0,12,0,0,0,7,0,0,1");
    E("    db 21,0,0,1,14,0,0,7,114,0,16,0,9,0,0,0");
    E("    db 70,2,16,0,9,0,0,0,86,5,16,0,12,0,0,0");
    E("    db 56,0,0,8,114,0,16,0,13,0,0,0,246,15,16,0");
    E("    db 2,0,0,0,38,9,16,128,65,0,0,0,9,0,0,0");
    E("    db 56,0,0,7,114,0,16,0,14,0,0,0,246,15,16,0");
    E("    db 9,0,0,0,150,4,16,0,9,0,0,0,56,0,0,7");
    E("    db 114,0,16,0,15,0,0,0,38,9,16,0,3,0,0,0");
    E("    db 38,9,16,0,13,0,0,0,50,0,0,10,114,0,16,0");
    E("    db 15,0,0,0,150,4,16,0,3,0,0,0,70,2,16,0");
    E("    db 13,0,0,0,70,2,16,128,65,0,0,0,15,0,0,0");
    E("    db 0,0,0,7,114,0,16,0,15,0,0,0,70,2,16,0");
    E("    db 2,0,0,0,70,2,16,0,15,0,0,0,56,0,0,7");
    E("    db 114,0,16,0,16,0,0,0,38,9,16,0,11,0,0,0");
    E("    db 70,2,16,0,14,0,0,0,50,0,0,10,114,0,16,0");
    E("    db 11,0,0,0,150,4,16,0,11,0,0,0,150,4,16,0");
    E("    db 14,0,0,0,70,2,16,128,65,0,0,0,16,0,0,0");
    E("    db 0,0,0,7,114,0,16,0,10,0,0,0,70,2,16,0");
    E("    db 10,0,0,0,70,2,16,0,11,0,0,0,0,0,0,8");
    E("    db 114,0,16,0,10,0,0,0,70,2,16,128,65,0,0,0");
  #include "../shaders/gpu_shader_blobs.inc"  E("    db 10,0,0,0,70,2,16,0,15,0,0,0,16,0,0,7");
    E("    db 130,0,16,0,9,0,0,0,70,2,16,0,10,0,0,0");
    E("    db 70,2,16,0,9,0,0,0,29,0,0,7,18,0,16,0");
    E("    db 11,0,0,0,58,0,16,0,9,0,0,0,1,64,0,0");
    E("    db 0,0,0,0,31,0,4,3,10,0,16,0,11,0,0,0");
    E("    db 30,0,0,7,18,0,16,0,11,0,0,0,58,0,16,0");
    E("    db 8,0,0,0,1,64,0,0,1,0,0,0,54,0,0,5");
    E("    db 130,0,16,0,8,0,0,0,10,0,16,0,11,0,0,0");
    E("    db 7,0,0,1,21,0,0,1,56,0,0,7,114,0,16,0");
    E("    db 11,0,0,0,70,2,16,0,9,0,0,0,38,9,16,0");
    E("    db 13,0,0,0,50,0,0,10,114,0,16,0,11,0,0,0");
    E("    db 150,4,16,0,13,0,0,0,150,4,16,0,9,0,0,0");
    E("    db 70,2,16,128,65,0,0,0,11,0,0,0,56,0,0,7");
    E("    db 114,0,16,0,11,0,0,0,166,10,16,0,0,0,0,0");
    E("    db 70,2,16,0,11,0,0,0,56,0,0,7,114,0,16,0");
    E("    db 15,0,0,0,38,9,16,0,13,0,0,0,70,2,16,0");
    E("    db 11,0,0,0,50,0,0,10,114,0,16,0,11,0,0,0");
    E("    db 38,9,16,0,11,0,0,0,70,2,16,0,13,0,0,0");
    E("    db 70,2,16,128,65,0,0,0,15,0,0,0,16,0,0,7");
    E("    db 18,0,16,0,11,0,0,0,70,2,16,0,11,0,0,0");
    E("    db 70,2,16,0,9,0,0,0,56,0,0,7,114,0,16,0");
    E("    db 15,0,0,0,70,2,16,0,9,0,0,0,70,2,16,0");
    E("    db 14,0,0,0,50,0,0,10,114,0,16,0,15,0,0,0");
    E("    db 38,9,16,0,14,0,0,0,150,4,16,0,9,0,0,0");
    E("    db 70,2,16,128,65,0,0,0,15,0,0,0,56,0,0,7");
    E("    db 114,0,16,0,15,0,0,0,246,15,16,#include "../shaders/gpu_shader_blobs.inc"0,11,0,0,0");
    E("    db 70,2,16,0,15,0,0,0,56,0,0,7,114,0,16,0");
    E("    db 16,0,0,0,70,2,16,0,14,0,0,0,70,2,16,0");
    E("    db 15,0,0,0,50,0,0,10,114,0,16,0,15,0,0,0");
    E("    db 38,9,16,0,15,0,0,0,150,4,16,0,14,0,0,0");
    E("    db 70,2,16,128,65,0,0,0,16,0,0,0,16,0,0,7");
    E("    db 34,0,16,0,11,0,0,0,70,2,16,0,15,0,0,0");
    E("    db 70,2,16,0,9,0,0,0,0,0,0,7,130,0,16,0");
    E("    db 10,0,0,0,58,0,16,0,1,0,0,0,58,0,16,0");
    E("    db 10,0,0,0,0,0,0,7,18,0,16,0,11,0,0,0");
    E("    db 10,0,16,0,11,0,0,0,58,0,16,0,10,0,0,0");
    E("    db 0,0,0,7,18,0,16,0,11,0,0,0,26,0,16,0");
    E("    db 11,0,0,0,10,0,16,0,11,0,0,0,29,0,0,7");
    E("    db 34,0,16,0,11,0,0,0,1,64,0,0,119,204,43,50");
    E("    db 10,0,16,0,11,0,0,0,31,0,4,3,26,0,16,0");
    E("    db 11,0,0,0,30,0,0,7,34,0,16,0,11,0,0,0");
    E("    db 58,0,16,0,8,0,0,0,1,64,0,0,1,0,0,0");
    E("    db 54,0,0,5,130,0,16,0,8,0,0,0,26,0,16,0");
    E("    db 11,0,0,0,7,0,0,1,21,0,0,1,51,0,0,7");
    E("    db 34,0,16,0,11,0,0,0,58,0,16,0,3,0,0,0");
    E("    db 10,0,16,0,12,0,0,0,0,0,0,7,34,0,16,0");
    E("    db 11,0,0,0,26,0,16,0,11,0,0,0,1,64,0,0");
    E("    db 0,0,128,63,56,0,0,8,34,0,16,0,11,0,0,0");
    E("    db 58,0,16,0,9,0,0,0,26,0,16,128,65,0,0,0");
    E("    db 11,0,0,0,14,0,0,7,34,0,16,0,11,0,0,0");
    E("    db 26,0,16,0,11,0,0,0,10,0,16,0,11,0,0,0");
    E("    db 56,0,0,7,114,0,16,0,15,0,0,0,150,4,16,0");
    E("    db 9,0,0,0,86,5,16,0,11,0,0,0,50,0,0,9");
    E("    db 114,0,16,0,16,0,0,0,38,9,16,0,15,0,0,0");
    E("    db 246,15,16,0,1,0,0,0,150,7,16,0,7,0,0,0");
    E("    db 56,0,0,7,114,0,16,0,17,0,0,0,70,2,16,0");
    E("    db 13,0,0,0,70,2,16,0,15,0,0,0,50,0,0,10");
    E("    db 114,0,16,0,15,0,0,0,38,9,16,0,13,0,0,0");
    E("    db 150,4,16,0,15,0,0,0,70,2,16,128,65,0,0,0");
    E("    db 17,0,0,0,50,0,0,9,114,0,16,0,15,0,0,0");
    E("    db 166,10,16,0,0,0,0,0,70,2,16,0,15,0,0,0");
    E("    db 70,2,16,0,8,0,0,0,50,0,0,10,114,0,16,0");
    E("    db 17,0,0,0,246,15,16,128,65,0,0,0,9,0,0,0");
    E("    db 150,4,16,0,9,0,0,0,150,4,16,0,10,0,0,0");
    E("    db 16,0,0,7,130,0,16,0,9,0,0,0,70,2,16,0");
    E("    db 17,0,0,0,70,2,16,0,17,0,0,0,75,0,0,5");
    E("    db 130,0,16,0,9,0,0,0,58,0,16,0,9,0,0,0");
    E("    db 49,0,0,7,66,0,16,0,11,0,0,0,1,64,0,0");
    E("    db 172,197,39,55,58,0,16,0,9,0,0,0,14,0,0,7");
    E("    db 114,0,16,0,17,0,0,0,70,2,16,0,17,0,0,0");
    E("    db 246,15,16,0,9,0,0,0,56,0,0,7,114,0,16,0");
    E("    db 18,0,0,0,38,9,16,0,13,0,0,0,38,9,16,0");
    E("    db 17,0,0,0,50,0,0,10,114,0,16,0,18,0,0,0");
    E("    db 150,4,16,0,13,0,0,0,70,2,16,0,17,0,0,0");
    E("    db 70,2,16,128,65,0,0,0,18,0,0,0,56,0,0,7");
    E("    db 114,0,16,0,18,0,0,0,166,10,16,0,0,0,0,0");
    E("    db 70,2,16,0,18,0,0,0,56,0,0,7,114,0,16,0");
    E("    db 19,0,0,0,38,9,16,0,13,0,0,0,70,2,16,0");
    E("    db 18,0,0,0,50,0,0,10,114,0,16,0,18,0,0,0");
    E("    db 38,9,16,0,18,0,0,0,70,2,16,0,13,0,0,0");
    E("    db 70,2,16,128,65,0,0,0,19,0,0,0,16,0,0,7");
    E("    db 130,0,16,0,9,0,0,0,150,4,16,0,18,0,0,0");
    E("    db 70,2,16,0,17,0,0,0,56,0,0,7,114,0,16,0");
    E("    db 18,0,0,0,70,2,16,0,14,0,0,0,38,9,16,0");
    E("    db 17,0,0,0,50,0,0,10,114,0,16,0,18,0,0,0");
    E("    db 38,9,16,0,14,0,0,0,70,2,16,0,17,0,0,0");
    E("    db 70,2,16,128,65,0,0,0,18,0,0,0,56,0,0,7");
    E("    db 114,0,16,0,18,0,0,0,246,15,16,0,11,0,0,0");
    E("    db 70,2,16,0,18,0,0,0,56,0,0,7,114,0,16,0");
    E("    db 19,0,0,0,70,2,16,0,14,0,0,0,70,2,16,0");
    E("    db 18,0,0,0,50,0,0,10,114,0,16,0,14,0,0,0");
    E("    db 38,9,16,0,18,0,0,0,150,4,16,0,14,0,0,0");
    E("    db 70,2,16,128,65,0,0,0,19,0,0,0,16,0,0,7");
    E("    db 130,0,16,0,11,0,0,0,150,4,16,0,14,0,0,0");
    E("    db 70,2,16,0,17,0,0,0,0,0,0,7,130,0,16,0");
    E("    db 9,0,0,0,58,0,16,0,9,0,0,0,58,0,16,0");
    E("    db 10,0,0,0,0,0,0,7,130,0,16,0,9,0,0,0");
    E("    db 58,0,16,0,11,0,0,0,58,0,16,0,9,0,0,0");
    E("    db 49,0,0,7,130,0,16,0,10,0,0,0,1,64,0,0");
    E("    db 119,204,43,50,58,0,16,0,9,0,0,0,56,0,0,9");
    E("    db 34,0,16,0,11,0,0,0,26,0,16,128,129,0,0,0");
    E("    db 11,0,0,0,10,128,32,0,0,0,0,0,11,0,0,0");
    E("    db 16,0,0,7,18,0,16,0,10,0,0,0,150,4,16,0");
    E("    db 10,0,0,0,70,2,16,0,17,0,0,0,14,0,0,8");
    E("    db 130,0,16,0,9,0,0,0,10,0,16,128,65,0,0,0");
    E("    db 10,0,0,0,58,0,16,0,9,0,0,0,52,0,0,8");
    E("    db 130,0,16,0,9,0,0,0,26,0,16,128,65,0,0,0");
    E("    db 11,0,0,0,58,0,16,0,9,0,0,0,51,0,0,7");
    E("    db 130,0,16,0,9,0,0,0,26,0,16,0,11,0,0,0");
    E("    db 58,0,16,0,9,0,0,0,56,0,0,7,114,0,16,0");
    E("    db 10,0,0,0,70,2,16,0,17,0,0,0,246,15,16,0");
    E("    db 9,0,0,0,50,0,0,9,114,0,16,0,14,0,0,0");
    E("    db 38,9,16,0,10,0,0,0,246,15,16,0,1,0,0,0");
    E("    db 70,2,16,0,16,0,0,0,56,0,0,7,114,0,16,0");
    E("    db 17,0,0,0,70,2,16,0,10,0,0,0,70,2,16,0");
    E("    db 13,0,0,0,50,0,0,10,114,0,16,0,10,0,0,0");
    E("    db 38,9,16,0,13,0,0,0,150,4,16,0,10,0,0,0");
    E("    db 70,2,16,128,65,0,0,0,17,0,0,0,50,0,0,9");
    E("    db 114,0,16,0,10,0,0,0,166,10,16,0,0,0,0,0");
    E("    db 70,2,16,0,10,0,0,0,70,2,16,0,15,0,0,0");
    E("    db 55,0,0,9,114,0,16,0,13,0,0,0,246,15,16,0");
    E("    db 10,0,0,0,70,2,16,0,14,0,0,0,70,2,16,0");
    E("    db 16,0,0,0,55,0,0,9,114,0,16,0,10,0,0,0");
    E("    db 246,15,16,0,10,0,0,0,70,2,16,0,10,0,0,0");
    E("    db 70,2,16,0,15,0,0,0,55,0,0,9,114,0,16,0");
    E("    db 13,0,0,0,166,10,16,0,11,0,0,0,70,2,16,0");
    E("    db 13,0,0,0,70,2,16,0,16,0,0,0,55,0,0,9");
    E("    db 114,0,16,0,8,0,0,0,166,10,16,0,11,0,0,0");
    E("    db 70,2,16,0,10,0,0,0,70,2,16,0,15,0,0,0");
    E("    db 0,0,0,8,130,0,16,0,9,0,0,0,26,0,16,128");
    E("    db 65,0,0,0,12,0,0,0,42,0,16,0,12,0,0,0");
    E("    db 56,0,0,7,130,0,16,0,9,0,0,0,58,0,16,0");
    E("    db 9,0,0,0,1,64,0,0,205,204,76,62,14,0,0,7");
    E("    db 130,0,16,0,9,0,0,0,58,0,16,0,9,0,0,0");
    E("    db 10,0,16,0,4,0,0,0,56,0,0,7,114,0,16,0");
    E("    db 9,0,0,0,246,15,16,0,9,0,0,0,70,2,16,0");
    E("    db 9,0,0,0,56,0,0,7,114,0,16,0,9,0,0,0");
    E("    db 246,15,16,0,1,0,0,0,70,2,16,0,9,0,0,0");
    E("    db 50,0,0,9,226,0,16,0,7,0,0,0,6,9,16,0");
    E("    db 9,0,0,0,6,0,16,0,11,0,0,0,6,9,16,0");
    E("    db 13,0,0,0,30,0,0,7,130,0,16,0,8,0,0,0");
    E("    db 58,0,16,0,8,0,0,0,1,64,0,0,1,0,0,0");
    E("    db 22,0,0,1,54,0,0,5,114,0,16,0,5,0,0,0");
    E("    db 150,7,16,0,7,0,0,0,54,0,0,5,98,0,16,0");
    E("    db 6,0,0,0,86,6,16,0,8,0,0,0,54,0,0,5");
    E("    db 130,0,16,0,5,0,0,0,10,0,16,0,8,0,0,0");
    E("    db 21,0,0,1,190,24,0,1,30,0,0,7,130,0,16,0");
    E("    db 4,0,0,0,58,0,16,0,4,0,0,0,1,64,0,0");
    E("    db 1,0,0,0,22,0,0,1,31,0,4,3,10,0,16,0");
    E("    db 0,0,0,0,168,0,0,8,114,224,17,0,1,0,0,0");
    E("    db 10,0,2,0,1,64,0,0,0,0,0,0,70,2,16,0");
    E("    db 5,0,0,0,54,0,0,5,18,0,16,0,6,0,0,0");
    E("    db 58,0,16,0,5,0,0,0,168,0,0,8,114,224,17,0");
    E("    db 1,0,0,0,10,0,2,0,1,64,0,0,16,0,0,0");
    E("    db 70,2,16,0,6,0,0,0,21,0,0,1,62,0,0,1");
    E("    db 83,84,65,84,148,0,0,0,192,0,0,0,20,0,0,0");
    E("    db 0,0,0,0,2,0,0,0,80,0,0,0,13,0,0,0");
    E("    db 9,0,0,0,8,0,0,0,11,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,10,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,31,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 2,0,0,0,0,0,0,0,2,0,0,0");
    E("_gpu_cs_resolve_blob_len equ 7340");
    E("align 16");
    E("_gpu_cs_apply_blob:  ; 3748 bytes DXBC (physics compute: ApplyImpulses (commit + sleep))");
    E("    db 68,88,66,67,27,61,104,27,191,227,150,237,73,68,31,109");
    E("    db 214,235,190,225,1,0,0,0,164,14,0,0,5,0,0,0");
    E("    db 52,0,0,0,240,7,0,0,0,8,0,0,16,8,0,0");
    E("    db 8,14,0,0,82,68,69,70,180,7,0,0,3,0,0,0");
    E("    db 196,0,0,0,3,0,0,0,60,0,0,0,0,5,83,67");
    E("    db 0,1,0,0,140,7,0,0,82,68,49,49,60,0,0,0");
    E("    db 24,0,0,0,32,0,0,0,40,0,0,0,36,0,0,0");
    E("    db 12,0,0,0,0,0,0,0,156,0,0,0,6,0,0,0");
    E("    db 6,0,0,0,1,0,0,0,96,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,163,0,0,0,6,0,0,0");
    E("    db 6,0,0,0,1,0,0,0,32,0,0,0,1,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,176,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 1,0,0,0,1,0,0,0,66,111,100,105,101,115,0,73");
    E("    db 109,112,117,108,115,101,65,99,99,117,109,0,80,104,121,115");
    E("    db 105,99,115,67,111,110,115,116,97,110,116,115,0,171,171,171");
    E("    db 176,0,0,0,14,0,0,0,12,1,0,0,208,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,156,0,0,0,1,0,0,0");
    E("    db 188,4,0,0,96,0,0,0,0,0,0,0,3,0,0,0");
    E("    db 163,0,0,0,1,0,0,0,212,6,0,0,32,0,0,0");
    E("    db 0,0,0,0,3,0,0,0,60,3,0,0,0,0,0,0");
    E("    db 12,0,0,0,0,0,0,0,76,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 112,3,0,0,12,0,0,0,4,0,0,0,2,0,0,0");
    E("    db 128,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,164,3,0,0,16,0,0,0");
    E("    db 4,0,0,0,2,0,0,0,180,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 216,3,0,0,20,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 128,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,227,3,0,0,24,0,0,0");
    E("    db 4,0,0,0,0,0,0,0,128,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 239,3,0,0,28,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 128,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,247,3,0,0,32,0,0,0");
    E("    db 4,0,0,0,0,0,0,0,128,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 9,4,0,0,36,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 180,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,23,4,0,0,48,0,0,0");
    E("    db 128,0,0,0,0,0,0,0,40,4,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 76,4,0,0,176,0,0,0,4,0,0,0,0,0,0,0");
    E("    db 128,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,85,4,0,0,180,0,0,0");
    E("    db 4,0,0,0,2,0,0,0,128,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 103,4,0,0,184,0,0,0,4,0,0,0,2,0,0,0");
    E("    db 128,3,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,121,4,0,0,188,0,0,0");
    E("    db 4,0,0,0,2,0,0,0,128,3,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 140,4,0,0,192,0,0,0,16,0,0,0,0,0,0,0");
    E("    db 152,4,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,103,114,97,118,105,116,121,0");
    E("    db 102,108,111,97,116,51,0,171,1,0,3,0,1,0,3,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,68,3,0,0,100,101,108,116");
    E("    db 97,84,105,109,101,0,102,108,111,97,116,0,0,0,3,0");
    E("    db 1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,122,3,0,0");
    E("    db 98,111,100,121,67,111,117,110,116,0,100,119,111,114,100,0");
    E("    db 0,0,19,0,1,0,1,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 174,3,0,0,108,105,110,101,97,114,68,97,109,112,0,97");
    E("    db 110,103,117,108,97,114,68,97,109,112,0,103,114,111,117,110");
    E("    db 100,89,0,103,114,111,117,110,100,82,101,115,116,105,116,117");
    E("    db 116,105,111,110,0,99,111,108,108,105,100,101,114,67,111,117");
    E("    db 110,116,0,99,111,108,108,105,100,101,114,115,0,102,108,111");
    E("    db 97,116,52,0,1,0,3,0,1,0,4,0,8,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,33,4,0,0,102,114,105,99,116,105,111,110");
    E("    db 0,115,108,101,101,112,76,105,110,84,104,114,101,115,104,111");
    E("    db 108,100,0,115,108,101,101,112,65,110,103,84,104,114,101,115");
    E("    db 104,111,108,100,0,115,108,101,101,112,84,105,109,101,84,104");
    E("    db 114,101,115,104,111,108,100,0,119,97,108,108,66,111,117,110");
    E("    db 100,115,0,171,1,0,3,0,1,0,4,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,33,4,0,0,228,4,0,0,0,0,0,0");
    E("    db 96,0,0,0,2,0,0,0,176,6,0,0,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 36,69,108,101,109,101,110,116,0,82,105,103,105,100,66,111");
    E("    db 100,121,0,112,111,115,105,116,105,111,110,0,1,0,3,0");
    E("    db 1,0,3,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,68,3,0,0");
    E("    db 105,110,118,77,97,115,115,0,0,0,3,0,1,0,1,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,122,3,0,0,118,101,108,111");
    E("    db 99,105,116,121,0,114,97,100,105,117,115,0,111,114,105,101");
    E("    db 110,116,97,116,105,111,110,0,1,0,3,0,1,0,4,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,33,4,0,0,97,110,103,117");
    E("    db 108,97,114,86,101,108,0,114,101,115,116,105,116,117,116,105");
    E("    db 111,110,0,102,108,97,103,115,0,171,171,171,0,0,19,0");
    E("    db 1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,174,3,0,0");
    E("    db 95,112,97,100,0,105,110,118,73,110,101,114,116,105,97,0");
    E("    db 115,108,101,101,112,84,105,109,101,114,0,95,112,97,100,50");
    E("    db 0,102,108,111,97,116,50,0,1,0,3,0,1,0,2,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,245,5,0,0,247,4,0,0");
    E("    db 0,5,0,0,0,0,0,0,36,5,0,0,44,5,0,0");
    E("    db 12,0,0,0,80,5,0,0,0,5,0,0,16,0,0,0");
    E("    db 89,5,0,0,44,5,0,0,28,0,0,0,96,5,0,0");
    E("    db 108,5,0,0,32,0,0,0,144,5,0,0,0,5,0,0");
    E("    db 48,0,0,0,155,5,0,0,44,5,0,0,60,0,0,0");
    E("    db 167,5,0,0,176,5,0,0,64,0,0,0,212,5,0,0");
    E("    db 0,5,0,0,68,0,0,0,217,5,0,0,44,5,0,0");
    E("    db 80,0,0,0,228,5,0,0,44,5,0,0,84,0,0,0");
    E("    db 239,5,0,0,252,5,0,0,88,0,0,0,5,0,0,0");
    E("    db 1,0,24,0,0,0,12,0,32,6,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,237,4,0,0");
    E("    db 228,4,0,0,0,0,0,0,32,0,0,0,2,0,0,0");
    E("    db 104,7,0,0,0,0,0,0,255,255,255,255,0,0,0,0");
    E("    db 255,255,255,255,0,0,0,0,73,109,112,117,108,115,101,65");
    E("    db 99,99,117,109,69,110,116,114,121,0,108,105,110,101,97,114");
    E("    db 73,109,112,117,108,115,101,0,95,112,97,100,48,0,97,110");
    E("    db 103,117,108,97,114,73,109,112,117,108,115,101,0,95,112,97");
    E("    db 100,49,0,171,14,7,0,0,0,5,0,0,0,0,0,0");
    E("    db 28,7,0,0,44,5,0,0,12,0,0,0,34,7,0,0");
    E("    db 0,5,0,0,16,0,0,0,49,7,0,0,44,5,0,0");
    E("    db 28,0,0,0,5,0,0,0,1,0,8,0,0,0,4,0");
    E("    db 56,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,252,6,0,0,77,105,99,114,111,115,111,102");
    E("    db 116,32,40,82,41,32,72,76,83,76,32,83,104,97,100,101");
    E("    db 114,32,67,111,109,112,105,108,101,114,32,49,48,46,49,0");
    E("    db 73,83,71,78,8,0,0,0,0,0,0,0,8,0,0,0");
    E("    db 79,83,71,78,8,0,0,0,0,0,0,0,8,0,0,0");
    E("    db 83,72,69,88,240,5,0,0,80,0,5,0,124,1,0,0");
    E("    db 106,8,0,1,89,0,0,4,70,142,32,0,0,0,0,0");
    E("    db 12,0,0,0,158,0,0,4,0,224,17,0,0,0,0,0");
    E("    db 96,0,0,0,158,0,0,4,0,224,17,0,1,0,0,0");
    E("    db 32,0,0,0,95,0,0,2,18,0,2,0,104,0,0,2");
    E("    db 9,0,0,0,155,0,0,4,0,1,0,0,1,0,0,0");
    E("    db 1,0,0,0,80,0,0,7,18,0,16,0,0,0,0,0");
    E("    db 10,0,2,0,10,128,32,0,0,0,0,0,1,0,0,0");
    E("    db 31,0,4,3,10,0,16,0,0,0,0,0,62,0,0,1");
    E("    db 21,0,0,1,167,0,0,138,2,3,3,128,131,153,25,0");
    E("    db 242,0,16,0,0,0,0,0,10,0,2,0,1,64,0,0");
    E("    db 12,0,0,0,150,227,17,0,0,0,0,0,167,0,0,138");
    E("    db 2,3,3,128,131,153,25,0,114,0,16,0,1,0,0,0");
    E("    db 10,0,2,0,1,64,0,0,48,0,0,0,70,226,17,0");
    E("    db 0,0,0,0,167,0,0,138,2,3,3,128,131,153,25,0");
    E("    db 18,0,16,0,2,0,0,0,10,0,2,0,1,64,0,0");
    E("    db 64,0,0,0,6,224,17,0,0,0,0,0,167,0,0,138");
    E("    db 2,3,3,128,131,153,25,0,34,0,16,0,2,0,0,0");
    E("    db 10,0,2,0,1,64,0,0,84,0,0,0,6,224,17,0");
    E("    db 0,0,0,0,1,0,0,7,130,0,16,0,1,0,0,0");
    E("    db 10,0,16,0,2,0,0,0,1,64,0,0,1,0,0,0");
    E("    db 31,0,0,3,58,0,16,0,1,0,0,0,62,0,0,1");
    E("    db 21,0,0,1,167,0,0,138,2,3,1,128,131,153,25,0");
    E("    db 114,0,16,0,3,0,0,0,10,0,2,0,1,64,0,0");
    E("    db 0,0,0,0,70,226,17,0,1,0,0,0,167,0,0,138");
    E("    db 2,3,1,128,131,153,25,0,114,0,16,0,4,0,0,0");
    E("    db 10,0,2,0,1,64,0,0,16,0,0,0,70,226,17,0");
    E("    db 1,0,0,0,1,0,0,7,130,0,16,0,1,0,0,0");
    E("    db 10,0,16,0,2,0,0,0,1,64,0,0,2,0,0,0");
    E("    db 31,0,4,3,58,0,16,0,1,0,0,0,16,0,0,7");
    E("    db 130,0,16,0,1,0,0,0,70,2,16,0,3,0,0,0");
    E("    db 70,2,16,0,3,0,0,0,75,0,0,5,130,0,16,0");
    E("    db 1,0,0,0,58,0,16,0,1,0,0,0,16,0,0,7");
    E("    db 130,0,16,0,3,0,0,0,70,2,16,0,4,0,0,0");
    E("    db 70,2,16,0,4,0,0,0,75,0,0,5,130,0,16,0");
    E("    db 3,0,0,0,58,0,16,0,3,0,0,0,0,0,0,7");
    E("    db 130,0,16,0,1,0,0,0,58,0,16,0,1,0,0,0");
    E("    db 58,0,16,0,3,0,0,0,49,0,0,7,130,0,16,0");
    E("    db 1,0,0,0,1,64,0,0,23,183,209,56,58,0,16,0");
    E("    db 1,0,0,0,31,0,4,3,58,0,16,0,1,0,0,0");
    E("    db 1,0,0,7,18,0,16,0,2,0,0,0,10,0,16,0");
    E("    db 2,0,0,0,1,64,0,0,253,255,255,255,18,0,0,1");
    E("    db 62,0,0,1,21,0,0,1,54,0,0,5,34,0,16,0");
    E("    db 2,0,0,0,1,64,0,0,0,0,0,0,21,0,0,1");
    E("    db 49,0,0,7,130,0,16,0,1,0,0,0,1,64,0,0");
    E("    db 0,0,0,0,58,0,16,0,0,0,0,0,0,0,0,7");
    E("    db 114,0,16,0,3,0,0,0,70,2,16,0,0,0,0,0");
    E("    db 70,2,16,0,3,0,0,0,0,0,0,7,178,0,16,0");
    E("    db 4,0,0,0,150,1,16,0,1,0,0,0,150,1,16,0");
    E("    db 4,0,0,0,16,0,0,7,18,0,16,0,5,0,0,0");
    E("    db 70,2,16,0,3,0,0,0,70,2,16,0,3,0,0,0");
    E("    db 16,0,0,7,34,0,16,0,5,0,0,0,70,3,16,0");
    E("    db 4,0,0,0,70,3,16,0,4,0,0,0,75,0,0,5");
    E("    db 50,0,16,0,5,0,0,0,70,0,16,0,5,0,0,0");
    E("    db 49,0,0,8,50,0,16,0,5,0,0,0,70,0,16,0");
    E("    db 5,0,0,0,150,133,32,0,0,0,0,0,11,0,0,0");
    E("    db 1,0,0,7,18,0,16,0,5,0,0,0,26,0,16,0");
    E("    db 5,0,0,0,10,0,16,0,5,0,0,0,0,0,0,8");
    E("    db 130,0,16,0,6,0,0,0,26,0,16,0,2,0,0,0");
    E("    db 58,128,32,0,0,0,0,0,0,0,0,0,49,0,0,8");
    E("    db 34,0,16,0,5,0,0,0,58,128,32,0,0,0,0,0");
    E("    db 11,0,0,0,58,0,16,0,6,0,0,0,60,0,0,7");
    E("    db 66,0,16,0,7,0,0,0,10,0,16,0,2,0,0,0");
    E("    db 1,64,0,0,2,0,0,0,54,0,0,5,130,0,16,0");
    E("    db 3,0,0,0,58,0,16,0,4,0,0,0,55,0,0,12");
    E("    db 242,0,16,0,8,0,0,0,86,5,16,0,5,0,0,0");
    E("    db 2,64,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,70,14,16,0,3,0,0,0,54,0,0,8");
    E("    db 50,0,16,0,7,0,0,0,2,64,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,54,0,0,5");
    E("    db 66,0,16,0,4,0,0,0,10,0,16,0,2,0,0,0");
    E("    db 55,0,0,9,114,0,16,0,6,0,0,0,86,5,16,0");
    E("    db 5,0,0,0,70,2,16,0,7,0,0,0,70,2,16,0");
    E("    db 4,0,0,0,55,0,0,9,242,0,16,0,3,0,0,0");
    E("    db 6,0,16,0,5,0,0,0,70,14,16,0,8,0,0,0");
    E("    db 70,14,16,0,3,0,0,0,54,0,0,5,130,0,16,0");
    E("    db 4,0,0,0,1,64,0,0,0,0,0,0,55,0,0,9");
    E("    db 242,0,16,0,4,0,0,0,6,0,16,0,5,0,0,0");
    E("    db 70,14,16,0,6,0,0,0,70,14,16,0,4,0,0,0");
    E("    db 54,0,0,5,130,0,16,0,0,0,0,0,10,0,16,0");
    E("    db 1,0,0,0,55,0,0,9,242,0,16,0,0,0,0,0");
    E("    db 246,15,16,0,1,0,0,0,54,9,16,0,3,0,0,0");
    E("    db 54,9,16,0,0,0,0,0,54,0,0,5,194,0,16,0");
    E("    db 2,0,0,0,86,9,16,0,1,0,0,0,55,0,0,9");
    E("    db 242,0,16,0,1,0,0,0,246,15,16,0,1,0,0,0");
    E("    db 70,14,16,0,4,0,0,0,230,4,16,0,2,0,0,0");
    E("    db 168,0,0,8,114,224,17,0,0,0,0,0,10,0,2,0");
    E("    db 1,64,0,0,16,0,0,0,150,7,16,0,0,0,0,0");
    E("    db 54,0,0,5,98,0,16,0,0,0,0,0,6,1,16,0");
    E("    db 1,0,0,0,168,0,0,8,114,224,17,0,0,0,0,0");
    E("    db 10,0,2,0,1,64,0,0,48,0,0,0,70,2,16,0");
    E("    db 0,0,0,0,168,0,0,8,18,224,17,0,0,0,0,0");
    E("    db 10,0,2,0,1,64,0,0,64,0,0,0,42,0,16,0");
    E("    db 1,0,0,0,168,0,0,8,18,224,17,0,0,0,0,0");
    E("    db 10,0,2,0,1,64,0,0,84,0,0,0,58,0,16,0");
    E("    db 1,0,0,0,62,0,0,1,83,84,65,84,148,0,0,0");
    E("    db 58,0,0,0,9,0,0,0,0,0,0,0,1,0,0,0");
    E("    db 15,0,0,0,0,0,0,0,6,0,0,0,5,0,0,0");
    E("    db 4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,6,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,8,0,0,0");
    E("    db 6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    E("    db 4,0,0,0");
    E("_gpu_cs_apply_blob_len equ 3748");
*/
// This closing brace ends the gpu_data() section for the shader blob embeddings.
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
    E("BUFDESC_MISC       equ 16");           // D3D11_BUFFER_DESC.MiscFlags");
    E("BUFDESC_STRIDE     equ 20");           // D3D11_BUFFER_DESC.StructureByteStride");
    E("MISC_STRUCTURED    equ 0x40");         // D3D11_RESOURCE_MISC_BUFFER_STRUCTURED");
    E("LIGHT_STRIDE       equ 32");           // sizeof(Light): pos3+color3+range+castShadows");
    E("SRV_DIM_BUFFER     equ 1");            // D3D11_SRV_DIMENSION_BUFFER");
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

    // --- Tessellation stages (non-fatal: on failure the patch path is disabled
    // but the direct path still works, so these do NOT jump to .pl_fail; they
    // leave _gpu_tvs/ths/tds zero, and present skips patch draws when any is 0).
    // Device vtable: CreateVertexShader 0x60, CreateHullShader 0x80,
    // CreateDomainShader 0x88 (arith-verified vs CreatePixelShader 0x78 /
    // CreateComputeShader 0x90). ---
    // CreateVertexShader(tvs_blob, len, NULL, &_gpu_tvs)
    E("    lea  rax, [_gpu_tvs]");
    E("    mov  qword [rsp+0x20], rax");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [_gpu_tvs_blob]");
    E("    mov  r8,  _gpu_tvs_blob_len");
    E("    xor  r9,  r9");
    E("    call [rax + 0x60]");
    // CreateHullShader(ths_blob, len, NULL, &_gpu_ths)
    E("    lea  rax, [_gpu_ths]");
    E("    mov  qword [rsp+0x20], rax");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [_gpu_ths_blob]");
    E("    mov  r8,  _gpu_ths_blob_len");
    E("    xor  r9,  r9");
    E("    call [rax + 0x80]");
    // CreateDomainShader(tds_blob, len, NULL, &_gpu_tds)
    E("    lea  rax, [_gpu_tds]");
    E("    mov  qword [rsp+0x20], rax");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [_gpu_tds_blob]");
    E("    mov  r8,  _gpu_tds_blob_len");
    E("    xor  r9,  r9");
    E("    call [rax + 0x88]");

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

    // Constant buffer: 224 bytes, dynamic. Layout: viewproj(64) + fog tail(32,
    // incl invTexDims) + lightVP(64 at off 96) + camPos.xyz+shadowPass(16 at
    // off 160) + lightCount(4 at off 176)+pad(12) = 192, then the tess tail @192:
    // tessScale,tessMax,dispScale,useNormMap (16) + dispTexel.xy+pad2 (16) = 224.
    // 224 is a 16-byte multiple. The direct render VS/PS ignore the tail.
    E("    mov  dword [rsp+0x100+BUFDESC_BYTEWIDTH], 224");
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

    // Fourth sampler: linear + WRAP, used as an s0 override on patch (terrain) draws
    // so a tiled color texture repeats across the surface with filtering. Non-fatal:
    // on failure patch draws fall back to the point+clamp s0 (no tiling).
    // ADDR_WRAP = 1.
    E("    lea  rdi, [rsp+0x150]");
    E("    xor  eax, eax");
    E("    mov  ecx, 13");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x150+0], FILTER_LINEAR");
    E("    mov  dword [rsp+0x150+4], 1            ; ADDR_WRAP (U)");
    E("    mov  dword [rsp+0x150+8], 1            ; ADDR_WRAP (V)");
    E("    mov  dword [rsp+0x150+12], 1           ; ADDR_WRAP (W)");
    E("    mov  dword [rsp+0x150+48], 0x7f7fffff  ; MaxLOD = FLT_MAX");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x150]");
    E("    lea  r8,  [_gpu_sampler_wrap]");
    E("    call [rax + 0xb8]");
    // non-fatal: leave _gpu_sampler_wrap=0 on failure.

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

    // Wireframe raster (FILL_WIREFRAME=2, CULL_NONE=1) for the tessellation debug
    // view: patch draws bind this so every generated triangle's edges show, making
    // distance-adaptive subdivision directly visible. Non-fatal: if it fails, patch
    // draws just render solid (via _gpu_raster).
    E("    lea  rdi, [rsp+0x150]");
    E("    xor  eax, eax");
    E("    mov  ecx, 10");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x150+0], 2");    // FILL_WIREFRAME
    E("    mov  dword [rsp+0x150+4], 1");    // CULL_NONE (show all edges)
    E("    mov  dword [rsp+0x150+8], 0");
    E("    mov  dword [rsp+0x150+24], 1");   // DepthClipEnable
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x150]");
    E("    lea  r8,  [_gpu_raster_wire]");
    E("    call [rax + 0xb0]                 ; CreateRasterizerState (wireframe)");
    // non-fatal: leave _gpu_raster_wire=0 on failure, patch draws use solid.

    // Shadow-pass raster (FILL_SOLID=3, CULL_NONE=1) with HARDWARE slope-scaled depth
    // bias: renders ALL faces (no silhouette loss) and pushes stored depth away from
    // the light proportional to polygon slope, so grazing surfaces get more offset --
    // killing the acne banding without dropping the near/top of the cast shadow.
    // RASTERIZER_DESC: DepthBias@12(int), DepthBiasClamp@16(f32), SlopeScaledDepthBias@20(f32).
    E("    lea  rdi, [rsp+0x150]");
    E("    xor  eax, eax");
    E("    mov  ecx, 10");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x150+0], 3");    // FILL_SOLID
    E("    mov  dword [rsp+0x150+4], 1");    // CULL_NONE (both faces cast)
    E("    mov  dword [rsp+0x150+8], 0");    // FrontCounterClockwise = FALSE
    E("    mov  dword [rsp+0x150+12], 200"); // DepthBias (constant, in depth units)
    E("    mov  dword [rsp+0x150+20], 0x40800000"); // SlopeScaledDepthBias = 4.0f
    E("    mov  dword [rsp+0x150+24], 1");   // DepthClipEnable
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x150]");
    E("    lea  r8,  [_gpu_raster_shadow]");
    E("    call [rax + 0xb0]                 ; CreateRasterizerState (shadow CULL_NONE + slope bias)");
    // non-fatal: leave _gpu_raster_shadow=0 on failure; depth pass uses CULL_BACK.

    // Particle raster (FILL_SOLID=3, CULL_NONE=1, no bias): billboards are camera-
    // facing quads whose fixed corner winding is not orientation-tested, so back-face
    // culling would drop them entirely. CULL_NONE renders both faces.
    E("    lea  rdi, [rsp+0x150]");
    E("    xor  eax, eax");
    E("    mov  ecx, 10");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x150+0], 3");    // FILL_SOLID
    E("    mov  dword [rsp+0x150+4], 1");    // CULL_NONE
    E("    mov  dword [rsp+0x150+8], 0");    // FrontCounterClockwise = FALSE
    E("    mov  dword [rsp+0x150+24], 1");   // DepthClipEnable
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x150]");
    E("    lea  r8,  [_gpu_raster_none]");
    E("    call [rax + 0xb0]                 ; CreateRasterizerState (particle CULL_NONE)");
    // non-fatal: leave _gpu_raster_none=0 on failure; particle draw falls back to CULL_BACK.

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
    // Depth-read-only state for transparent particles: DepthEnable=1, WriteMask=ZERO,
    // Func=LESS. Solid geometry still occludes particles (test on), but overlapping
    // billboards do not write depth, so they accumulate instead of z-fighting.
    E("    lea  rdi, [rsp+0x100]");
    E("    xor  eax, eax");
    E("    mov  ecx, 13");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x100+0], 1        ; DepthEnable");
    E("    mov  dword [rsp+0x100+4], 0        ; DepthWriteMask = ZERO");
    E("    mov  dword [rsp+0x100+8], 2        ; DepthFunc = LESS");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x100]");
    E("    lea  r8,  [_gpu_dsstate_read]");
    E("    call [rax + 0xA8]                  ; CreateDepthStencilState (read-only)");
    E("    test eax, eax");
    E("    jnz  .pl_fail");

    // --- Shadow map ARRAY: 1024x1024 R32_TYPELESS x SHADOW_MAX(16) slices. Each
    // slice is one light's depth map (slice == light index). Viewable as a depth
    // target (per-slice DSV, shadow pass) and a Texture2DArray SRV (main pass). ---
    E("SHADOW_MAX  equ 32");
    E("SHADOW_RES  equ 1024");
    // TEXTURE2D_DESC (44 bytes) at rsp+0x100. ArraySize = SHADOW_MAX.
    E("    lea  rdi, [rsp+0x100]");
    E("    xor  eax, eax");
    E("    mov  ecx, 11");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x100+0], SHADOW_RES  ; Width");
    E("    mov  dword [rsp+0x100+4], SHADOW_RES  ; Height");
    E("    mov  dword [rsp+0x100+8], 1        ; MipLevels");
    E("    mov  dword [rsp+0x100+12], SHADOW_MAX ; ArraySize (one slice per light)");
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
    E("    call [rax + 0x28]                  ; CreateTexture2D (array)");
    E("    test eax, eax");
    E("    jnz  .pl_fail");
    // Per-slice DSVs: DSV_DIMENSION_TEXTURE2DARRAY(4). Desc: Format@0=D32(0x28),
    // ViewDim@4=4, Flags@8=0, Tex2DArray.MipSlice@12=0, FirstArraySlice@16=i,
    // ArraySize@20=1. Loop i=0..SHADOW_MAX-1 -> _gpu_shadow_dsv[i].
    E("    xor  r12, r12                      ; slice index i");
    E(".pl_shadow_dsv:");
    E("    lea  rdi, [rsp+0x100]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x100+0], 0x28     ; D32_FLOAT");
    E("    mov  dword [rsp+0x100+4], 4        ; DSV_DIMENSION_TEXTURE2DARRAY");
    E("    mov  dword [rsp+0x100+8], 0        ; Flags");
    E("    mov  dword [rsp+0x100+12], 0       ; MipSlice");
    E("    mov  [rsp+0x100+16], r12d          ; FirstArraySlice = i");
    E("    mov  dword [rsp+0x100+20], 1       ; ArraySize = 1");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    mov  rdx, [_gpu_shadowtex]");
    E("    lea  r8,  [rsp+0x100]");
    E("    lea  r9,  [_gpu_shadow_dsv]");
    E("    lea  r9,  [r9 + r12*8]             ; &_gpu_shadow_dsv[i]");
    E("    call [rax + 0x50]                  ; CreateDepthStencilView (slice i)");
    E("    test eax, eax");
    E("    jnz  .pl_fail");
    E("    inc  r12");
    E("    cmp  r12, SHADOW_MAX");
    E("    jl   .pl_shadow_dsv");
    // Array SRV: SRV_DIMENSION_TEXTURE2DARRAY(5). Desc: Format@0=R32F(0x29),
    // ViewDim@4=5, Tex2DArray.MostDetailedMip@8=0, MipLevels@12=1,
    // FirstArraySlice@16=0, ArraySize@20=SHADOW_MAX.
    E("    lea  rdi, [rsp+0x100]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x100+0], 0x29     ; R32_FLOAT");
    E("    mov  dword [rsp+0x100+4], 5        ; SRV_DIMENSION_TEXTURE2DARRAY");
    E("    mov  dword [rsp+0x100+8], 0        ; MostDetailedMip");
    E("    mov  dword [rsp+0x100+12], 1       ; MipLevels");
    E("    mov  dword [rsp+0x100+16], 0       ; FirstArraySlice");
    E("    mov  dword [rsp+0x100+20], SHADOW_MAX ; ArraySize");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    mov  rdx, [_gpu_shadowtex]");
    E("    lea  r8,  [rsp+0x100]");
    E("    lea  r9,  [_gpu_shadow_srv]");
    E("    call [rax + 0x38]                  ; CreateShaderResourceView (array)");
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
    E("GPU_VTX_STRIDE   equ 64");         // float vertex: pos3 + uv2 + col4 + slice + flag + nrm3 + pad2 (16 f32)
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

// _slag_gpu_upload_lights: (re)create the structured light buffer + SRV on demand,
// map/copy _gpu_lights_cnt Light structs (32B each) from _gpu_lights_ptr, and bind
// the SRV at PS t1. No-op (binds NULL at t1) when count is 0 or no ptr. Uses a
// scratch SRV/BUFFER_DESC at [rsp+0x100]; caller reserves >= 0x120 stack + shadow.
static void emit_gpu_upload_lights(Codegen *cg) {
    E("; --- _slag_gpu_upload_lights ---");
    E("_slag_gpu_upload_lights:");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    sub  rsp, 0x120");
    E("    mov  rbx, [_gpu_device]");
    E("    test rbx, rbx");
    E("    jz   .ul_ret");
    // count == 0 or ptr == 0 -> bind NULL SRV at t1, done.
    E("    mov  rax, [_gpu_lights_cnt]");
    E("    test rax, rax");
    E("    jz   .ul_bind_null");
    E("    mov  rax, [_gpu_lights_ptr]");
    E("    test rax, rax");
    E("    jz   .ul_bind_null");
    // Need (re)create if buffer missing OR capacity < count.
    E("    mov  rax, [_gpu_lights_cnt]");
    E("    cmp  rax, [_gpu_lights_cap]");
    E("    jbe  .ul_have_buf");
    // --- (re)create buffer + SRV sized to the current count -------------------
    // Release old SRV + buffer if present.
    E("    mov  rcx, [_gpu_lights_srv]");
    E("    test rcx, rcx");
    E("    jz   .ul_no_old_srv");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]                  ; SRV Release");
    E("    mov  qword [_gpu_lights_srv], 0");
    E(".ul_no_old_srv:");
    E("    mov  rcx, [_gpu_lights_buf]");
    E("    test rcx, rcx");
    E("    jz   .ul_no_old_buf");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]                  ; buffer Release");
    E("    mov  qword [_gpu_lights_buf], 0");
    E(".ul_no_old_buf:");
    // BUFFER_DESC: ByteWidth = count*32, DYNAMIC, BIND_SRV, CPU_WRITE,
    // MISC_STRUCTURED, StructureByteStride = 32.
    E("    lea  rdi, [rsp+0x100]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  rax, [_gpu_lights_cnt]");
    E("    shl  rax, 5                        ; *32");
    E("    mov  dword [rsp+0x100+BUFDESC_BYTEWIDTH], eax");
    E("    mov  dword [rsp+0x100+BUFDESC_USAGE], USAGE_DYNAMIC");
    E("    mov  dword [rsp+0x100+BUFDESC_BIND], BIND_SRV");
    E("    mov  dword [rsp+0x100+BUFDESC_CPUACCESS], D3DCPU_WRITE");
    E("    mov  dword [rsp+0x100+BUFDESC_MISC], MISC_STRUCTURED");
    E("    mov  dword [rsp+0x100+BUFDESC_STRIDE], LIGHT_STRIDE");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x100]");
    E("    xor  r8,  r8                       ; pInitialData = NULL");
    E("    lea  r9,  [_gpu_lights_buf]");
    E("    call [rax + 0x18]                  ; CreateBuffer");
    E("    test eax, eax");
    E("    jnz  .ul_ret");
    // SRV_DESC: Format(0)=UNKNOWN, ViewDim(4)=BUFFER, Buffer.FirstElement(8)=0,
    // Buffer.NumElements(12)=count.
    E("    lea  rdi, [rsp+0x100]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x100+0], 0        ; DXGI_FORMAT_UNKNOWN (structured)");
    E("    mov  dword [rsp+0x100+4], SRV_DIM_BUFFER");
    E("    mov  dword [rsp+0x100+8], 0        ; FirstElement");
    E("    mov  rax, [_gpu_lights_cnt]");
    E("    mov  dword [rsp+0x100+12], eax     ; NumElements");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    mov  rdx, [_gpu_lights_buf]");
    E("    lea  r8,  [rsp+0x100]");
    E("    lea  r9,  [_gpu_lights_srv]");
    E("    call [rax + 0x38]                  ; CreateShaderResourceView");
    E("    test eax, eax");
    E("    jnz  .ul_ret");
    E("    mov  rax, [_gpu_lights_cnt]");
    E("    mov  [_gpu_lights_cap], rax");
    E(".ul_have_buf:");
    // --- Map DISCARD, copy count*32 bytes, Unmap -----------------------------
    E("    mov  r15, [_gpu_context]");
    E("    lea  r11, [rsp+0x40]");
    E("    mov  [rsp+0x28], r11");
    E("    mov  dword [rsp+0x20], 0");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_lights_buf]");
    E("    xor  r8d, r8d");
    E("    mov  r9d, MAP_WR_DISCARD");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x70]                  ; Map");
    E("    test eax, eax");
    E("    jnz  .ul_bind");
    E("    mov  rdi, [rsp+0x40]               ; mapped.pData");
    E("    mov  rsi, [_gpu_lights_ptr]");
    E("    mov  rax, [_gpu_lights_cnt]");
    E("    shl  rax, 2                        ; count*32/8 = count*4 qwords");
    E("    mov  rcx, rax");
    E("    rep  movsq");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_lights_buf]");
    E("    xor  r8d, r8d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x78]                  ; Unmap");
    E(".ul_bind:");
    // PSSetShaderResources(1, 1, &srv)
    E("    mov  r15, [_gpu_context]");
    E("    mov  rax, [_gpu_lights_srv]");
    E("    mov  [rsp+0x40], rax");
    E("    mov  rcx, r15");
    E("    mov  edx, 1                        ; StartSlot t1");
    E("    mov  r8d, 1                        ; NumViews");
    E("    lea  r9,  [rsp+0x40]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x40]                  ; PSSetShaderResources");
    E("    jmp  .ul_ret");
    E(".ul_bind_null:");
    E("    mov  r15, [_gpu_context]");
    E("    mov  qword [rsp+0x40], 0");
    E("    mov  rcx, r15");
    E("    mov  edx, 1");
    E("    mov  r8d, 1");
    E("    lea  r9,  [rsp+0x40]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x40]                  ; PSSetShaderResources(1,1,{NULL})");
    E(".ul_ret:");
    E("    add  rsp, 0x120");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    ret");
    E("");
}

// _slag_gpu_upload_lightvps: (re)create the structured lightVP-matrix buffer + SRV,
// map/copy N 64-byte matrices from _gpu_lightvp_arr, bind the SRV at PS t5. N is
// _gpu_lightvp_cnt clamped to SHADOW_MAX. No-op (NULL at t5) when count 0 or no ptr.
// Mirrors _slag_gpu_upload_lights but stride 64 (a row-major 4x4). Uses [rsp+0x100]
// scratch; caller reserves >= 0x120 stack + shadow.
static void emit_gpu_upload_lightvps(Codegen *cg) {
    E("; --- _slag_gpu_upload_lightvps ---");
    E("_slag_gpu_upload_lightvps:");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    sub  rsp, 0x120");
    E("    mov  rbx, [_gpu_device]");
    E("    test rbx, rbx");
    E("    jz   .uv_ret");
    E("    mov  rax, [_gpu_lightvp_cnt]");
    E("    test rax, rax");
    E("    jz   .uv_bind_null");
    E("    mov  rax, [_gpu_lightvp_arr]");
    E("    test rax, rax");
    E("    jz   .uv_bind_null");
    // clamp count to SHADOW_MAX
    E("    mov  rax, [_gpu_lightvp_cnt]");
    E("    cmp  rax, SHADOW_MAX");
    E("    jbe  .uv_cnt_ok");
    E("    mov  qword [_gpu_lightvp_cnt], SHADOW_MAX");
    E(".uv_cnt_ok:");
    // (re)create if buffer missing OR capacity < count
    E("    mov  rax, [_gpu_lightvp_cnt]");
    E("    cmp  rax, [_gpu_lightvp_cap]");
    E("    jbe  .uv_have_buf");
    E("    mov  rcx, [_gpu_lightvp_srv]");
    E("    test rcx, rcx");
    E("    jz   .uv_no_old_srv");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]");
    E("    mov  qword [_gpu_lightvp_srv], 0");
    E(".uv_no_old_srv:");
    E("    mov  rcx, [_gpu_lightvp_buf]");
    E("    test rcx, rcx");
    E("    jz   .uv_no_old_buf");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]");
    E("    mov  qword [_gpu_lightvp_buf], 0");
    E(".uv_no_old_buf:");
    // BUFFER_DESC: ByteWidth = count*64, DYNAMIC, BIND_SRV, CPU_WRITE, STRUCTURED, stride 64.
    E("    lea  rdi, [rsp+0x100]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  rax, [_gpu_lightvp_cnt]");
    E("    shl  rax, 6                        ; *64");
    E("    mov  dword [rsp+0x100+BUFDESC_BYTEWIDTH], eax");
    E("    mov  dword [rsp+0x100+BUFDESC_USAGE], USAGE_DYNAMIC");
    E("    mov  dword [rsp+0x100+BUFDESC_BIND], BIND_SRV");
    E("    mov  dword [rsp+0x100+BUFDESC_CPUACCESS], D3DCPU_WRITE");
    E("    mov  dword [rsp+0x100+BUFDESC_MISC], MISC_STRUCTURED");
    E("    mov  dword [rsp+0x100+BUFDESC_STRIDE], 64");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x100]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_lightvp_buf]");
    E("    call [rax + 0x18]                  ; CreateBuffer");
    E("    test eax, eax");
    E("    jnz  .uv_ret");
    E("    lea  rdi, [rsp+0x100]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x100+0], 0        ; UNKNOWN (structured)");
    E("    mov  dword [rsp+0x100+4], SRV_DIM_BUFFER");
    E("    mov  dword [rsp+0x100+8], 0        ; FirstElement");
    E("    mov  rax, [_gpu_lightvp_cnt]");
    E("    mov  dword [rsp+0x100+12], eax     ; NumElements");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    mov  rdx, [_gpu_lightvp_buf]");
    E("    lea  r8,  [rsp+0x100]");
    E("    lea  r9,  [_gpu_lightvp_srv]");
    E("    call [rax + 0x38]                  ; CreateShaderResourceView");
    E("    test eax, eax");
    E("    jnz  .uv_ret");
    E("    mov  rax, [_gpu_lightvp_cnt]");
    E("    mov  [_gpu_lightvp_cap], rax");
    E(".uv_have_buf:");
    // Map DISCARD, copy count*64 bytes, Unmap.
    E("    mov  r15, [_gpu_context]");
    E("    lea  r11, [rsp+0x40]");
    E("    mov  [rsp+0x28], r11");
    E("    mov  dword [rsp+0x20], 0");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_lightvp_buf]");
    E("    xor  r8d, r8d");
    E("    mov  r9d, MAP_WR_DISCARD");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x70]                  ; Map");
    E("    test eax, eax");
    E("    jnz  .uv_bind");
    E("    mov  rdi, [rsp+0x40]               ; mapped.pData");
    E("    mov  rsi, [_gpu_lightvp_arr]");
    E("    mov  rax, [_gpu_lightvp_cnt]");
    E("    shl  rax, 3                        ; count*64/8 = count*8 qwords");
    E("    mov  rcx, rax");
    E("    rep  movsq");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_lightvp_buf]");
    E("    xor  r8d, r8d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x78]                  ; Unmap");
    E(".uv_bind:");
    // PSSetShaderResources(5, 1, &srv)
    E("    mov  r15, [_gpu_context]");
    E("    mov  rax, [_gpu_lightvp_srv]");
    E("    mov  [rsp+0x40], rax");
    E("    mov  rcx, r15");
    E("    mov  edx, 5                        ; StartSlot t5");
    E("    mov  r8d, 1");
    E("    lea  r9,  [rsp+0x40]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x40]                  ; PSSetShaderResources(5,1,{lightvpSRV})");
    E("    jmp  .uv_ret");
    E(".uv_bind_null:");
    E("    mov  r15, [_gpu_context]");
    E("    mov  qword [rsp+0x40], 0");
    E("    mov  rcx, r15");
    E("    mov  edx, 5");
    E("    mov  r8d, 1");
    E("    lea  r9,  [rsp+0x40]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x40]                  ; PSSetShaderResources(5,1,{NULL})");
    E(".uv_ret:");
    E("    add  rsp, 0x120");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    ret");
    E("");
}

// _slag_gpu_present_frame: staged raw verts -> float vbuf, upload tex, set
// state, Draw, Present. No-op if nothing staged. Resets _gpu_stage_cnt.
static void emit_gpu_present_frame(Codegen *cg) {
    emit_gpu_upload_lights(cg);
    emit_gpu_upload_lightvps(cg);
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
    E("    imul rcx, GPU_VTX_STRIDE / 4  ; dwords = verts * stride / 4 (64B -> verts*16)");
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
    // vertexCount carries the patch flag in bit 63 (set by fill_patch_gpu). Extract
    // it, then mask it off so the count used for Draw is clean.
    E("    mov  rax, [r11 + 8]");
    E("    mov  rcx, rax");
    E("    shr  rcx, 63");
    E("    mov  [_gpu_cur_patch], rcx  ; 1 = tessellated patchlist draw");
    E("    btr  rax, 63                ; clear bit 63 -> true vertexCount");
    E("    mov  [_gpu_cur_count], rax  ; vertexCount");
    // Patch draws require all 3 tess stages to exist; if any failed to create,
    // fall back to a normal tri draw (flag cleared) so the item still renders.
    E("    cmp  qword [_gpu_cur_patch], 0");
    E("    je   .pf_patch_ok");
    E("    cmp  qword [_gpu_tvs], 0");
    E("    je   .pf_patch_off");
    E("    cmp  qword [_gpu_ths], 0");
    E("    je   .pf_patch_off");
    E("    cmp  qword [_gpu_tds], 0");
    E("    jne  .pf_patch_ok");
    E(".pf_patch_off:");
    E("    mov  qword [_gpu_cur_patch], 0");
    E(".pf_patch_ok:");
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
    // camPos @160: the HS distance factor needs it, but camPos is normally written
    // only via the lightdir path (grid_cube passes camPos as lightdir). A patch draw
    // may have no light, so for patch items copy camPos (3 f32) from the viewproj
    // buffer @160 -> cbuf @160 unconditionally. Contract: a patch draw's viewproj
    // buffer must be >=176 bytes with camPos.xyz at byte offset 160.
    E("    cmp  qword [_gpu_cur_patch], 0");
    E("    je   .pf_cb_nocam");
    E("    mov  rsi, [_gpu_viewproj]");
    E("    test rsi, rsi");
    E("    jz   .pf_cb_nocam");
    E("    add  rsi, 160");
    E("    mov  rdi, [rsp+0x40]");
    E("    add  rdi, 160");
    E("    mov  ecx, 3");
    E(".pf_cb_cam:");
    E("    mov  eax, [rsi]");
    E("    mov  [rdi], eax");
    E("    add  rsi, 4");
    E("    add  rdi, 4");
    E("    dec  ecx");
    E("    jnz  .pf_cb_cam");
    E(".pf_cb_nocam:");
    // shadowPass flag (f32) -> cbuf offset 172. 1.0 for the shadow pass, else 0.0.
    E("    mov  rdi, [rsp+0x40]");
    E("    xor  eax, eax");
    E("    cmp  qword [_gpu_shadowpass_val], 0");
    E("    je   .pf_cb_spz");
    E("    mov  eax, 0x3F800000        ; 1.0f");
    E(".pf_cb_spz:");
    E("    mov  [rdi+172], eax");
    // lightCount (i32) -> cbuf offset 176 (PS StructuredBuffer loop bound).
    E("    mov  rax, [_gpu_lights_cnt]");
    E("    mov  [rdi+176], eax");
    // Tess tail (32 bytes @192): tessScale,tessMax,dispScale,useNormMap,dispTexel.xy,
    // pad2 -- from gpu.set_tess. Zero-fill when unset so the HS clamps factor to 1
    // (no tessellation amplification) rather than reading stale cbuffer bytes.
    E("    mov  rsi, [_gpu_tess_ptr]");
    E("    test rsi, rsi");
    E("    jz   .pf_cb_notess0");
    E("    add  rdi, 192");
    E("    mov  ecx, 4                 ; 4 qwords = 32 bytes");
    E("    rep  movsq");
    E("    jmp  .pf_cb_tess0");
    E(".pf_cb_notess0:");
    E("    xor  eax, eax");
    E("    mov  [rdi+192], rax");
    E("    mov  [rdi+200], rax");
    E("    mov  [rdi+208], rax");
    E("    mov  [rdi+216], rax");
    E(".pf_cb_tess0:");
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

    // ============ PER-ITEM STAGE SWITCH (tessellated vs direct) ==========
    // The invariant block above latched the DIRECT config (trilist + _gpu_vs, no
    // HS/DS) once. Each item may switch: a patch item needs patchlist topology +
    // the tess VS/HS/DS bound and tess SRVs/cbuffer on the DS; a tri item must
    // restore trilist + _gpu_vs + NULL HS/DS. This runs every item (4-6 cheap
    // context calls) so any A->B->A ordering is correct. Context vtable (canonical
    // ID3D11DeviceContext; HS/DS block sits below the CS group at 0x218+):
    // Verified against the confirmed in-file anchor CSSetShaderResources = idx 67 =
    // 0x218: HSSetShaderResources is idx 59 = 0x1D8, and the block runs contiguously
    // HS{SRV,Shader,Samplers,CB} then DS{SRV,Shader,Samplers,CB} = idx 59..66. The
    // earlier values were each one index (8 bytes) too low, so HSSetShader dispatched
    // HSSetShaderResources and d3d11 read the shader ptr as an SRV array -> SIGSEGV.
    E("HSSetShaderResources_  equ 0x1D8");   // idx 59
    E("HSSetShader_           equ 0x1E0");   // idx 60
    E("HSSetConstantBuffers_  equ 0x1F0");   // idx 62
    E("DSSetShaderResources_  equ 0x1F8");   // idx 63
    E("DSSetShader_           equ 0x200");   // idx 64
    E("DSSetSamplers_         equ 0x208");   // idx 65
    E("DSSetConstantBuffers_  equ 0x210");   // idx 66
    E("TOPOLOGY_PATCH3        equ 35");   // D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
    E("    cmp  qword [_gpu_cur_patch], 0");
    E("    jne  .pf_bind_patch");
    // --- DIRECT (tri) item: trilist, tess VS off, HS/DS NULL ---
    E("    mov  rcx, r15");
    E("    mov  edx, TOPOLOGY_TRILIST");
    E("    mov  rax, [r15]");
    E("    call [rax + 0xC0]           ; IASetPrimitiveTopology(TRILIST)");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_vs]");
    E("    xor  r8, r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x58]           ; VSSetShader(direct vs)");
    E("    mov  rcx, r15");
    E("    xor  rdx, rdx");
    E("    xor  r8, r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + HSSetShader_]   ; HSSetShader(NULL)");
    E("    mov  rcx, r15");
    E("    xor  rdx, rdx");
    E("    xor  r8, r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + DSSetShader_]   ; DSSetShader(NULL)");
    // restore the point+clamp s0 sampler (a preceding patch draw may have set wrap)
    E("    mov  rax, [_gpu_sampler]");
    E("    mov  [rsp+0x78], rax");
    E("    mov  rcx, r15");
    E("    xor  edx, edx               ; s0");
    E("    mov  r8d, 1");
    E("    lea  r9, [rsp+0x78]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x50]           ; PSSetSamplers(0,1,{point+clamp})");
    // restore solid raster for tri items (patch items may have set wireframe)
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_raster]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x158]          ; RSSetState(solid)");
    E("    jmp  .pf_bind_done");
    E(".pf_bind_patch:");
    // --- TESSELLATED item: patchlist, tess VS + HS + DS, DS cbuffer + SRVs ---
    E("    mov  rcx, r15");
    E("    mov  edx, TOPOLOGY_PATCH3");
    E("    mov  rax, [r15]");
    E("    call [rax + 0xC0]           ; IASetPrimitiveTopology(3-CP PATCHLIST)");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_tvs]");
    E("    xor  r8, r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x58]           ; VSSetShader(tess vs)");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_ths]");
    E("    xor  r8, r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + HSSetShader_]   ; HSSetShader(ths)");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_tds]");
    E("    xor  r8, r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + DSSetShader_]   ; DSSetShader(tds)");
    // HS + DS both read b0 (viewproj, camPos, lightVP, tess tail).
    E("    mov  rax, [_gpu_cbuf]");
    E("    mov  [rsp+0x78], rax");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");
    E("    mov  r8d, 1");
    E("    lea  r9, [rsp+0x78]");
    E("    mov  rax, [r15]");
    E("    call [rax + HSSetConstantBuffers_]");
    E("    mov  rax, [_gpu_cbuf]");
    E("    mov  [rsp+0x78], rax");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");
    E("    mov  r8d, 1");
    E("    lea  r9, [rsp+0x78]");
    E("    mov  rax, [r15]");
    E("    call [rax + DSSetConstantBuffers_]");
    // DS displacement + normal SRVs at t3, t4 (unbound -> reads 0 -> zero displace).
    E("    mov  rax, [_gpu_dispsrv]");
    E("    mov  [rsp+0x78], rax");
    E("    mov  rcx, r15");
    E("    mov  edx, 3");
    E("    mov  r8d, 1");
    E("    lea  r9, [rsp+0x78]");
    E("    mov  rax, [r15]");
    E("    call [rax + DSSetShaderResources_]");
    E("    mov  rax, [_gpu_normsrv]");
    E("    mov  [rsp+0x78], rax");
    E("    mov  rcx, r15");
    E("    mov  edx, 4");
    E("    mov  r8d, 1");
    E("    lea  r9, [rsp+0x78]");
    E("    mov  rax, [r15]");
    E("    call [rax + DSSetShaderResources_]");
    // DS linear sampler at s1 (reuse the SDF-text linear+clamp sampler).
    E("    mov  rax, [_gpu_sampler_lin]");
    E("    mov  [rsp+0x78], rax");
    E("    mov  rcx, r15");
    E("    mov  edx, 1");
    E("    mov  r8d, 1");
    E("    lea  r9, [rsp+0x78]");
    E("    mov  rax, [r15]");
    E("    call [rax + DSSetSamplers_]");
    // Override PS s0 with the linear+WRAP sampler so a tiled terrain texture repeats
    // with filtering. Fall back to the point+clamp s0 if wrap failed to create.
    E("    mov  rax, [_gpu_sampler_wrap]");
    E("    test rax, rax");
    E("    jnz  .pf_s0_wrap");
    E("    mov  rax, [_gpu_sampler]");
    E(".pf_s0_wrap:");
    E("    mov  [rsp+0x78], rax");
    E("    mov  rcx, r15");
    E("    xor  edx, edx               ; s0");
    E("    mov  r8d, 1");
    E("    lea  r9, [rsp+0x78]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x50]           ; PSSetSamplers(0,1,{wrap})");
    // Solid raster for the patch draw (CULL_BACK, same as the direct path).
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_raster]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x158]          ; RSSetState(solid)");
    E(".pf_bind_done:");

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
    // viewport = 1024x1024 (shadow map slice resolution)
    E("    mov  dword [rsp+0x40], 0");
    E("    mov  dword [rsp+0x44], 0");
    E("    mov  eax, 0x44800000        ; 1024.0f");
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
    // Front-face cull for the depth pass (render BACK faces) to kill grazing acne.
    // Fall back to the normal raster if the shadow raster failed to create.
    E("    mov  rdx, [_gpu_raster_shadow]");
    E("    test rdx, rdx");
    E("    jnz  .pf_shadow_raster");
    E("    mov  rdx, [_gpu_raster]");
    E(".pf_shadow_raster:");
    E("    mov  rcx, r15");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x158]          ; RSSetState(CULL_FRONT shadow)");
    // Draw this item's geometry into the shadow map (its vertex range)
    E("    mov  rcx, r15");
    E("    mov  edx, [_gpu_cur_count]  ; vertexCount");
    E("    mov  r8d, [_gpu_cur_start]  ; StartVertexLocation");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x68]           ; Draw (shadow depth)");
    // restore CULL_BACK for the main pass (the shadow pass set CULL_FRONT)
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_raster]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x158]          ; RSSetState(_gpu_raster)");
    // restore the real pixel shader for the main pass
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_ps]");
    E("    xor  r8, r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x48]           ; PSSetShader(_gpu_ps)");
    // NOTE: the shadow SRV is bound at t2 LATER (just before the main Draw), NOT
    // here. At this point _gpu_shadowtex is still bound as the depth-stencil OUTPUT
    // (shadowDSV, set above for the depth pass); binding it as an SRV now would hit
    // the D3D input/output conflict and be silently unbound. The main-pass
    // OMSetRenderTargets releases shadowDSV first, then t2 is bound pre-Draw.
    E(".pf_shadow_skip:");

    // ================= MULTI-LIGHT SHADOW DEPTH PASSES ==================
    // When _gpu_lightvp_cnt>0, render one depth pass per lightVP matrix into its own
    // array slice. Per pass i: map cbuf, write matrix i @96 + shadowPass=1 @172, unmap;
    // NULL PS; 1024 viewport; CULL_FRONT/NONE shadow raster; bind DSV slice i; clear;
    // draw the item. r13 = light index. Runs in the same per-item context as the
    // single pass; the item's geometry is drawn into every casting light's slice.
    E("    cmp  qword [_gpu_lightvp_cnt], 0");
    E("    je   .pf_mshadow_skip");
    // unbind shadow SRV from t2 (input/output hazard) once for all passes.
    E("    xor  eax, eax");
    E("    mov  [rsp+0x90], rax");
    E("    mov  rcx, r15");
    E("    mov  edx, 2");
    E("    mov  r8d, 1");
    E("    lea  r9, [rsp+0x90]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x40]           ; PSSetShaderResources(2,1,{NULL})");
    // NULL PS (depth only).
    E("    mov  rcx, r15");
    E("    xor  rdx, rdx");
    E("    xor  r8, r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x48]           ; PSSetShader(NULL)");
    // viewport 1024x1024 (once).
    E("    mov  dword [rsp+0x40], 0");
    E("    mov  dword [rsp+0x44], 0");
    E("    mov  eax, 0x44800000        ; 1024.0f");
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
    // shadow raster (CULL_NONE + slope bias), fall back to normal.
    E("    mov  rdx, [_gpu_raster_shadow]");
    E("    test rdx, rdx");
    E("    jnz  .pf_ms_raster");
    E("    mov  rdx, [_gpu_raster]");
    E(".pf_ms_raster:");
    E("    mov  rcx, r15");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x158]          ; RSSetState(shadow)");
    // per-light loop
    E("    xor  r13, r13               ; light index i");
    E(".pf_ms_loop:");
    E("    cmp  r13, [_gpu_lightvp_cnt]");
    E("    jae  .pf_ms_done");
    // map cbuf DISCARD, write full 176B (viewproj+fog) then matrix i @96 + shadowPass=1.
    E("    lea  r11, [rsp+0x40]");
    E("    mov  [rsp+0x28], r11");
    E("    mov  dword [rsp+0x20], 0");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_cbuf]");
    E("    xor  r8d, r8d");
    E("    mov  r9d, MAP_WR_DISCARD");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x70]           ; Map");
    E("    test eax, eax");
    E("    jnz  .pf_ms_next");
    // copy viewproj+fog head (96B) from _gpu_viewproj.
    E("    mov  rdi, [rsp+0x40]");
    E("    mov  rsi, [_gpu_viewproj]");
    E("    test rsi, rsi");
    E("    jz   .pf_ms_nohead");
    E("    mov  ecx, 12");
    E("    rep  movsq");
    E(".pf_ms_nohead:");
    // write lightVP matrix i (@96, 64B) from _gpu_lightvp_arr + i*64.
    E("    mov  rsi, [_gpu_lightvp_arr]");
    E("    mov  rax, r13");
    E("    shl  rax, 6                 ; i*64");
    E("    add  rsi, rax");
    E("    mov  rdi, [rsp+0x40]");
    E("    add  rdi, 96");
    E("    mov  ecx, 8");
    E("    rep  movsq");
    // shadowPass = 1.0 @172.
    E("    mov  rdi, [rsp+0x40]");
    E("    mov  dword [rdi+172], 0x3F800000");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_cbuf]");
    E("    xor  r8d, r8d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x78]           ; Unmap");
    // OMSetRenderTargets(0,NULL, shadowDSV[i]).
    E("    lea  rax, [_gpu_shadow_dsv]");
    E("    mov  r9, [rax + r13*8]");
    E("    test r9, r9");
    E("    jz   .pf_ms_next            ; slice DSV missing -> skip this light");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");
    E("    xor  r8, r8");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x108]          ; OMSetRenderTargets(0,NULL,shadowDSV[i])");
    // clear slice i to 1.0.
    E("    mov  byte [rsp+0x20], 0");
    E("    lea  rax, [_gpu_shadow_dsv]");
    E("    mov  rdx, [rax + r13*8]");
    E("    mov  rcx, r15");
    E("    mov  r8d, 1");
    E("    mov  eax, 0x3F800000");
    E("    movd xmm3, eax");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x1A8]          ; ClearDepthStencilView(slice i,1.0)");
    // draw the item's geometry into slice i.
    E("    mov  rcx, r15");
    E("    mov  edx, [_gpu_cur_count]");
    E("    mov  r8d, [_gpu_cur_start]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x68]           ; Draw (shadow depth slice i)");
    E(".pf_ms_next:");
    E("    inc  r13");
    E("    jmp  .pf_ms_loop");
    E(".pf_ms_done:");
    // restore CULL_BACK + real PS for the main pass.
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_raster]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x158]          ; RSSetState(_gpu_raster)");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_ps]");
    E("    xor  r8, r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x48]           ; PSSetShader(_gpu_ps)");
    E(".pf_mshadow_skip:");

    // Upload the dynamic point-light set and bind it at PS t1 for the main pass.
    E("    call _slag_gpu_upload_lights");
    E("    mov  r15, [_gpu_context]           ; reload (helper used r15 internally)");
    // Upload the per-light lightVP matrix array and bind it at PS t5 (multi-light shadows).
    E("    call _slag_gpu_upload_lightvps");
    E("    mov  r15, [_gpu_context]           ; reload (helper used r15 internally)");

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
    // OMSetRenderTargets(1, &rtv, dsv) -- depth buffer now bound. This releases the
    // shadow DSV bound during the depth pass, freeing _gpu_shadowtex to be sampled
    // as the t2 SRV (bound just before the main Draw).
    E("    mov  rax, [_gpu_rtv]");
    E("    mov  [rsp+0x90], rax");
    E("    mov  rcx, r15");
    E("    mov  edx, 1");
    E("    lea  r8, [rsp+0x90]");
    E("    mov  r9, [_gpu_dsv]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x108]           ; OMSetRenderTargets(rtv,dsv)");
    // Main pass needs shadowPass=0. When a light is set the cbuffer currently holds
    // shadowPass=1 (for the shadow pass), so re-map WRITE_DISCARD and rewrite it with
    // shadowPass=0. Full re-write (~176B) is trivial vs the draws; single cbuffer,
    // no extra resource. Skipped entirely when no light (cbuffer already shadowPass=0).
    // Must ALSO rewrite in the multi-light array case (_gpu_lightvp_cnt>0): the N-pass
    // depth loop left the cbuffer holding a light matrix + shadowPass=1, and there
    // _gpu_lightproj is 0, so without this the main draw would use the stale light cbuf.
    E("    cmp  qword [_gpu_lightproj], 0");
    E("    jne  .pf_main_cb_do");
    E("    cmp  qword [_gpu_lightvp_cnt], 0");
    E("    je   .pf_main_cb_done");
    E(".pf_main_cb_do:");
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
    E("    test rsi, rsi");
    E("    jnz  .pf_main_lvp_copy");     // single-light: copy from _gpu_lightproj
    E("    mov  rsi, [_gpu_lightvp_arr]");  // array path: copy matrix 0 into cbuffer @96
    E("    test rsi, rsi");
    E("    jz   .pf_main_lvp_done");
    E(".pf_main_lvp_copy:");
    E("    mov  rdi, [rsp+0x40]        ; mapped pData (deref, not lea)");
    E("    add  rdi, 96");
    E("    mov  ecx, 8");
    E("    rep  movsq");                 // lightVP (64)
    E(".pf_main_lvp_done:");
    E("    mov  rsi, [_gpu_lightdir]");
    E("    test rsi, rsi");
    E("    jz   .pf_main_ldir_zero");  // array path: no lightdir -> zero camPos (finite fog)
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
    E("    jmp  .pf_main_ldir_done");
    E(".pf_main_ldir_zero:");
    // camPos @160 must be a finite value or the PS radial fog (length(camPos-wpos))
    // goes NaN/huge and blends every pixel to the fog color -> blank screen. The
    // array shadow path never sets _gpu_lightdir, so zero camPos here.
    E("    mov  rdi, [rsp+0x40]");
    E("    xor  eax, eax");
    E("    mov  [rdi+160], eax");
    E("    mov  [rdi+164], eax");
    E("    mov  [rdi+168], eax");
    E(".pf_main_ldir_done:");
    E("    mov  rdi, [rsp+0x40]");
    // shadowPass @172: 0.0 = main pass single-light; 2.0 = main pass MULTI-light
    // (PS reads useArray=shadowPass>1.5 and samples lightVPs[li]/slice li at t5).
    E("    mov  dword [rdi+172], 0        ; shadowPass = 0.0 (main pass, single)");
    E("    cmp  qword [_gpu_lightvp_cnt], 0");
    E("    je   .pf_sp0_done");
    E("    mov  dword [rdi+172], 0x40000000 ; 2.0f (main pass, multi-light array)");
    E(".pf_sp0_done:");
    E("    mov  rax, [_gpu_lights_cnt]");
    E("    mov  [rdi+176], eax           ; lightCount @176 (main pass)");
    // Tess tail (32 bytes @192), same as the first cbuffer write.
    E("    mov  rsi, [_gpu_tess_ptr]");
    E("    test rsi, rsi");
    E("    jz   .pf_cb_notess1");
    E("    mov  rdi, [rsp+0x40]");
    E("    add  rdi, 192");
    E("    mov  ecx, 4");
    E("    rep  movsq");
    E("    jmp  .pf_cb_tess1");
    E(".pf_cb_notess1:");
    E("    mov  rdi, [rsp+0x40]");
    E("    xor  eax, eax");
    E("    mov  [rdi+192], rax");
    E("    mov  [rdi+200], rax");
    E("    mov  [rdi+208], rax");
    E("    mov  [rdi+216], rax");
    E(".pf_cb_tess1:");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_cbuf]");
    E("    xor  r8d, r8d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x78]           ; Unmap cbuf");
    E(".pf_main_cb_done:");
    // Bind the shadow SRV at t2 for the main pass. This MUST come after the
    // main-pass OMSetRenderTargets (which released shadowDSV): _gpu_shadowtex can
    // only be a shader input once it is no longer bound as the depth-stencil output.
    // Gated on _gpu_lightproj OR _gpu_lightvp_cnt (single or multi-light) so
    // non-shadow frames leave t2 alone.
    E("    cmp  qword [_gpu_lightproj], 0");
    E("    jne  .pf_t2_bind");
    E("    cmp  qword [_gpu_lightvp_cnt], 0");
    E("    je   .pf_t2_done");
    E(".pf_t2_bind:");
    E("    mov  rax, [_gpu_shadow_srv]");
    E("    mov  [rsp+0x90], rax");
    E("    mov  rcx, r15");
    E("    mov  edx, 2");
    E("    mov  r8d, 1");
    E("    lea  r9, [rsp+0x90]");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x40]           ; PSSetShaderResources(2,1,{shadowSRV})");
    E(".pf_t2_done:");
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

    // Draw GPU particles AFTER opaque geometry, BEFORE Present, so they composite
    // over the scene with the depth buffer still bound (solid geometry occludes
    // them; overlapping puffs accumulate via depth-read-only + alpha blend). No-op
    // when the particle system was never initialized (_gpu_part_ready == 0).
    E("    sub  rsp, 0x20");
    E("    call _slag_gpu_particle_draw");
    E("    add  rsp, 0x20");
    // Restore the main vertex buffer binding the particle draw replaced, so a later
    // resubmit of resident geometry (persistence path) draws from the right buffer.
    E("    mov  r15, [_gpu_context]");

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

// _slag_gpu_physics_init / _slag_gpu_physics_step: GPU-resident rigid-body
// physics compute path. init creates the 4 compute shaders; step (re)creates the
// Bodies (UAV+SRV) / ImpulseAccum (UAV) / cbuffer on first call or when count
// grows, uploads bodies once (GPU-resident after), then runs the 4-pass solver.
// Reuses _gpu_device/_gpu_context and the BUFDESC_*/MISC_STRUCTURED equ set.
static void emit_gpu_physics(Codegen *cg) {
    E("; --- physics compute equ (vtable slots + strides) ---");
    E("BIND_UAV        equ 0x80");        // D3D11_BIND_UNORDERED_ACCESS
    E("UAV_DIM_BUFFER  equ 1");           // D3D11_UAV_DIMENSION_BUFFER
    E("RB_STRIDE       equ 96");          // sizeof(RigidBody)
    E("IMP_STRIDE      equ 32");          // sizeof(ImpulseAccumEntry)
    E("PHYS_CBSZ       equ 208");         // sizeof(PhysicsConstants) (192 + float4 wallBounds)
    E("DEV_CREATE_CS   equ 0x90");        // ID3D11Device::CreateComputeShader (vtbl idx 18)
    E("DEV_CREATE_UAV  equ 0x40");        // ID3D11Device::CreateUnorderedAccessView
    E("CTX_UPDATESUB   equ 0x180");       // ID3D11DeviceContext::UpdateSubresource (vtbl idx 48)
    E("CTX_DISPATCH    equ 0x148");
    E("CTX_CSSETSRV    equ 0x218");
    E("CTX_CSSETUAV    equ 0x220");
    E("CTX_CSSETSH     equ 0x228");
    E("CTX_CSSETCB     equ 0x238");
    E("CTX_COPYRES     equ 0x178");        // ID3D11DeviceContext::CopyResource (vtbl idx 47)
    E("USAGE_STAGING   equ 3");            // D3D11_USAGE_STAGING
    E("D3DCPU_READ     equ 0x20000");      // D3D11_CPU_ACCESS_READ
    E("MAP_READ        equ 1");            // D3D11_MAP_READ
    E("");

    // ---- _slag_gpu_physics_init: create the 4 compute shaders ----
    E("; --- _slag_gpu_physics_init ---");
    E("_slag_gpu_physics_init:");
    E("    push rbx");
    E("    push r12");
    E("    push r13");
    E("    sub  rsp, 0x30");                 // 3 push+0x30 -> 16-align; shadow+arg5 at [rsp+0x20]
    E("    cmp  qword [_gpu_phys_ready], 0");
    E("    jne  .phi_ret");
    E("    mov  rbx, [_gpu_device]");
    E("    test rbx, rbx");
    E("    jz   .phi_ret");
    // CreateComputeShader(blob, len, NULL, &dest) -- 5 args, arg5 at [rsp+0x20].
    E("    lea  rax, [_gpu_cs_integrate]");
    E("    mov  [rsp+0x20], rax");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [_gpu_cs_integrate_blob]");
    E("    mov  r8,  _gpu_cs_integrate_blob_len");
    E("    xor  r9,  r9");
    E("    call [rax + DEV_CREATE_CS]");
    E("    test eax, eax");
    E("    jnz  .phi_ret");
    E("    lea  rax, [_gpu_cs_clear]");
    E("    mov  [rsp+0x20], rax");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [_gpu_cs_clear_blob]");
    E("    mov  r8,  _gpu_cs_clear_blob_len");
    E("    xor  r9,  r9");
    E("    call [rax + DEV_CREATE_CS]");
    E("    test eax, eax");
    E("    jnz  .phi_ret");
    E("    lea  rax, [_gpu_cs_resolve]");
    E("    mov  [rsp+0x20], rax");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [_gpu_cs_resolve_blob]");
    E("    mov  r8,  _gpu_cs_resolve_blob_len");
    E("    xor  r9,  r9");
    E("    call [rax + DEV_CREATE_CS]");
    E("    test eax, eax");
    E("    jnz  .phi_ret");
    E("    lea  rax, [_gpu_cs_apply]");
    E("    mov  [rsp+0x20], rax");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [_gpu_cs_apply_blob]");
    E("    mov  r8,  _gpu_cs_apply_blob_len");
    E("    xor  r9,  r9");
    E("    call [rax + DEV_CREATE_CS]");
    E("    test eax, eax");
    E("    jnz  .phi_ret");
    E("    mov  qword [_gpu_phys_ready], 1");
    E(".phi_ret:");
    E("    add  rsp, 0x30");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbx");
    E("    ret");
    E("");

    // ---- _slag_gpu_physics_step ----  r12=bodies_ptr r13=count r14=params_ptr
    // (codegen loads these three regs before the call; see codegen.c physics_step)
    E("; --- _slag_gpu_physics_step ---  r12=bodies_ptr r13=count r14=params_ptr");
    E("_slag_gpu_physics_step:");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    push r15");
    E("    sub  rsp, 0x128");                // 4 push+0x128 -> 16-align; desc/scratch space
    E("    cmp  qword [_gpu_phys_ready], 0");
    E("    je   .phs_ret");
    E("    mov  rbx, [_gpu_device]");
    E("    test rbx, rbx");
    E("    jz   .phs_ret");
    E("    mov  r15, [_gpu_context]");
    E("    test r13, r13");                  // count<=0 -> nothing to do
    E("    jle  .phs_ret");

    // (Re)create resources if first call or count exceeds capacity.
    E("    mov  rax, [_gpu_phys_cap]");
    E("    cmp  r13, rax");
    E("    jbe  .phs_have");

    // --- Bodies buffer: DEFAULT, BIND_UAV|BIND_SRV, MISC_STRUCTURED, stride 96 ---
    E("    lea  rdi, [rsp+0x40]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  rax, r13");
    E("    imul rax, RB_STRIDE");
    E("    mov  dword [rsp+0x40+BUFDESC_BYTEWIDTH], eax");
    E("    mov  dword [rsp+0x40+BUFDESC_USAGE], USAGE_DEFAULT");
    E("    mov  dword [rsp+0x40+BUFDESC_BIND], BIND_UAV | BIND_SRV");
    E("    mov  dword [rsp+0x40+BUFDESC_CPUACCESS], 0");
    E("    mov  dword [rsp+0x40+BUFDESC_MISC], MISC_STRUCTURED");
    E("    mov  dword [rsp+0x40+BUFDESC_STRIDE], RB_STRIDE");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x40]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_phys_bodies]");
    E("    call [rax + 0x18]");              // CreateBuffer
    E("    test eax, eax");
    E("    jnz  .phs_ret");

    // Bodies UAV: FORMAT_UNKNOWN, DIM_BUFFER, FirstElement 0, NumElements count.
    E("    lea  rdi, [rsp+0x40]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x40+0], 0");     // FORMAT_UNKNOWN
    E("    mov  dword [rsp+0x40+4], UAV_DIM_BUFFER");
    E("    mov  dword [rsp+0x40+8], 0");     // FirstElement
    E("    mov  dword [rsp+0x40+12], r13d"); // NumElements
    E("    mov  dword [rsp+0x40+16], 0");    // Flags
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    mov  rdx, [_gpu_phys_bodies]");
    E("    lea  r8,  [rsp+0x40]");
    E("    lea  r9,  [_gpu_phys_uav]");
    E("    call [rax + DEV_CREATE_UAV]");
    E("    test eax, eax");
    E("    jnz  .phs_ret");

    // Bodies SRV: FORMAT_UNKNOWN, DIM_BUFFER, FirstElement 0, NumElements count.
    E("    lea  rdi, [rsp+0x40]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x40+0], 0");
    E("    mov  dword [rsp+0x40+4], SRV_DIM_BUFFER");
    E("    mov  dword [rsp+0x40+8], 0");
    E("    mov  dword [rsp+0x40+12], r13d");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    mov  rdx, [_gpu_phys_bodies]");
    E("    lea  r8,  [rsp+0x40]");
    E("    lea  r9,  [_gpu_phys_srv]");
    E("    call [rax + 0x38]");              // CreateShaderResourceView
    E("    test eax, eax");
    E("    jnz  .phs_ret");

    // --- ImpulseAccum: DEFAULT, BIND_UAV, MISC_STRUCTURED, stride 32 ---
    E("    lea  rdi, [rsp+0x40]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  rax, r13");
    E("    imul rax, IMP_STRIDE");
    E("    mov  dword [rsp+0x40+BUFDESC_BYTEWIDTH], eax");
    E("    mov  dword [rsp+0x40+BUFDESC_USAGE], USAGE_DEFAULT");
    E("    mov  dword [rsp+0x40+BUFDESC_BIND], BIND_UAV");
    E("    mov  dword [rsp+0x40+BUFDESC_MISC], MISC_STRUCTURED");
    E("    mov  dword [rsp+0x40+BUFDESC_STRIDE], IMP_STRIDE");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x40]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_phys_imp]");
    E("    call [rax + 0x18]");
    E("    test eax, eax");
    E("    jnz  .phs_ret");
    E("    lea  rdi, [rsp+0x40]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x40+0], 0");
    E("    mov  dword [rsp+0x40+4], UAV_DIM_BUFFER");
    E("    mov  dword [rsp+0x40+8], 0");
    E("    mov  dword [rsp+0x40+12], r13d");
    E("    mov  dword [rsp+0x40+16], 0");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    mov  rdx, [_gpu_phys_imp]");
    E("    lea  r8,  [rsp+0x40]");
    E("    lea  r9,  [_gpu_phys_imp_uav]");
    E("    call [rax + DEV_CREATE_UAV]");
    E("    test eax, eax");
    E("    jnz  .phs_ret");

    // --- cbuffer: DYNAMIC, BIND_CONSTANT, 192B (created once) ---
    E("    cmp  qword [_gpu_phys_cbuf], 0");
    E("    jne  .phs_cap_set");
    E("    lea  rdi, [rsp+0x40]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x40+BUFDESC_BYTEWIDTH], PHYS_CBSZ");
    E("    mov  dword [rsp+0x40+BUFDESC_USAGE], USAGE_DYNAMIC");
    E("    mov  dword [rsp+0x40+BUFDESC_BIND], BIND_CONSTANT");
    E("    mov  dword [rsp+0x40+BUFDESC_CPUACCESS], D3DCPU_WRITE");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x40]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_phys_cbuf]");
    E("    call [rax + 0x18]");
    E("    test eax, eax");
    E("    jnz  .phs_ret");
    E(".phs_cap_set:");
    E("    mov  [_gpu_phys_cap], r13");
    E("    mov  qword [_gpu_phys_bodies_ptr], 0");  // force re-upload after realloc
    E(".phs_have:");

    // Upload body data once (GPU-resident after); re-upload only if bodies_ptr
    // changed. UpdateSubresource(res,0,NULL,pSrc,0,0) -- 7 args (pSrc=arg4).
    E("    mov  rax, [_gpu_phys_bodies_ptr]");
    E("    cmp  rax, r12");
    E("    je   .phs_uploaded");
    // UpdateSubresource(this, pDstResource, DstSubresource, pDstBox=NULL,
    // pSrcData, SrcRowPitch=0, SrcDepthPitch=0):
    //   rcx=this rdx=pDstResource r8d=DstSubresource r9=pDstBox(NULL)
    //   [rsp+0x20]=pSrcData [rsp+0x28]=SrcRowPitch [rsp+0x30]=SrcDepthPitch
    E("    mov  [rsp+0x20], r12");            // pSrcData (arg5)
    E("    mov  qword [rsp+0x28], 0");        // SrcRowPitch (arg6)
    E("    mov  qword [rsp+0x30], 0");        // SrcDepthPitch (arg7)
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_phys_bodies]");    // pDstResource
    E("    xor  r8d, r8d");                   // DstSubresource = 0
    E("    xor  r9,  r9");                    // pDstBox = NULL
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_UPDATESUB]");
    E("    mov  [_gpu_phys_bodies_ptr], r12");
    E(".phs_uploaded:");

    // Upload params to cbuffer (Map DISCARD, copy 192B, Unmap) each step.
    E("    lea  r11, [rsp+0x60]");
    E("    mov  [rsp+0x28], r11");
    E("    mov  dword [rsp+0x20], 0");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_phys_cbuf]");
    E("    xor  r8d, r8d");
    E("    mov  r9d, MAP_WR_DISCARD");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x70]");               // Map
    E("    test eax, eax");
    E("    jnz  .phs_ret");
    E("    mov  rdi, [rsp+0x60]");            // mapped.pData
    E("    mov  rsi, r14");                   // params_ptr
    E("    mov  ecx, PHYS_CBSZ / 8");         // 26 qwords
    E("    rep  movsq");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_phys_cbuf]");
    E("    xor  r8d, r8d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x78]");               // Unmap

    // Bind cbuffer at b0: CSSetConstantBuffers(0,1,&cbuf).
    E("    mov  rax, [_gpu_phys_cbuf]");
    E("    mov  [rsp+0x40], rax");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");
    E("    mov  r8d, 1");
    E("    lea  r9,  [rsp+0x40]");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETCB]");

    // groups = (count + 255) / 256 ; ebx = groups (x dimension).
    E("    lea  rax, [r13 + 255]");
    E("    shr  rax, 8");
    E("    mov  ebx, eax");

    // ===== PASS 1: main. Bind Bodies UAV @u0. =====
    E("    mov  rax, [_gpu_phys_uav]");
    E("    mov  [rsp+0x40], rax");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");                   // StartSlot u0
    E("    mov  r8d, 1");
    E("    lea  r9,  [rsp+0x40]");
    E("    mov  qword [rsp+0x20], 0");        // pUAVInitialCounts = NULL
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETUAV]");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_cs_integrate]");
    E("    xor  r8,  r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETSH]");
    E("    mov  rcx, r15");
    E("    mov  edx, ebx");
    E("    mov  r8d, 1");
    E("    mov  r9d, 1");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_DISPATCH]");

    // ===== PASS 2: ClearImpulses. Bind ImpulseAccum UAV @u1. =====
    E("    mov  rax, [_gpu_phys_imp_uav]");
    E("    mov  [rsp+0x40], rax");
    E("    mov  rcx, r15");
    E("    mov  edx, 1");                     // StartSlot u1
    E("    mov  r8d, 1");
    E("    lea  r9,  [rsp+0x40]");
    E("    mov  qword [rsp+0x20], 0");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETUAV]");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_cs_clear]");
    E("    xor  r8,  r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETSH]");
    E("    mov  rcx, r15");
    E("    mov  edx, ebx");
    E("    mov  r8d, 1");
    E("    mov  r9d, 1");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_DISPATCH]");

    // ===== PASS 3: ResolveBodyPairs. Unbind u0, bind SRV @t0. =====
    E("    mov  qword [rsp+0x40], 0");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");
    E("    mov  r8d, 1");
    E("    lea  r9,  [rsp+0x40]");
    E("    mov  qword [rsp+0x20], 0");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETUAV]");       // u0 = NULL
    E("    mov  rax, [_gpu_phys_srv]");
    E("    mov  [rsp+0x40], rax");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");
    E("    mov  r8d, 1");
    E("    lea  r9,  [rsp+0x40]");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETSRV]");        // t0 = srv
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_cs_resolve]");
    E("    xor  r8,  r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETSH]");
    E("    mov  rcx, r15");
    E("    mov  edx, ebx");
    E("    mov  r8d, 1");
    E("    mov  r9d, 1");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_DISPATCH]");

    // ===== PASS 4: ApplyImpulses. Unbind t0, rebind Bodies UAV @u0. =====
    E("    mov  qword [rsp+0x40], 0");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");
    E("    mov  r8d, 1");
    E("    lea  r9,  [rsp+0x40]");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETSRV]");        // t0 = NULL
    E("    mov  rax, [_gpu_phys_uav]");
    E("    mov  [rsp+0x40], rax");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");
    E("    mov  r8d, 1");
    E("    lea  r9,  [rsp+0x40]");
    E("    mov  qword [rsp+0x20], 0");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETUAV]");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_cs_apply]");
    E("    xor  r8,  r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETSH]");
    E("    mov  rcx, r15");
    E("    mov  edx, ebx");
    E("    mov  r8d, 1");
    E("    mov  r9d, 1");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_DISPATCH]");

    // Unbind both UAVs so a later graphics pass can bind Bodies as SRV/vertex.
    E("    mov  qword [rsp+0x40], 0");
    E("    mov  qword [rsp+0x48], 0");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");
    E("    mov  r8d, 2");
    E("    lea  r9,  [rsp+0x40]");
    E("    mov  qword [rsp+0x20], 0");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETUAV]");
    E(".phs_ret:");
    E("    add  rsp, 0x128");
    E("    pop  r15");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    ret");
    E("");

    // ---- _slag_gpu_physics_read ----  r12=dest_ptr r13=count
    // Copy the GPU-resident Bodies buffer into a STAGING buffer, Map it READ, and
    // memcpy count*96 bytes into the caller's CPU buffer (dest_ptr). Lets Slag see
    // the GPU-computed positions each frame (draw them, hit-test the mouse, etc.).
    // The staging buffer is created lazily and recreated when count grows.
    E("; --- _slag_gpu_physics_read ---  r12=dest_ptr r13=count");
    E("_slag_gpu_physics_read:");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    push r15");
    E("    sub  rsp, 0x128");
    E("    cmp  qword [_gpu_phys_ready], 0");
    E("    je   .phr_ret");
    E("    cmp  qword [_gpu_phys_bodies], 0");   // nothing uploaded yet
    E("    je   .phr_ret");
    E("    mov  rbx, [_gpu_device]");
    E("    test rbx, rbx");
    E("    jz   .phr_ret");
    E("    mov  r15, [_gpu_context]");
    E("    test r13, r13");                       // count<=0 -> nothing to do
    E("    jle  .phr_ret");

    // (Re)create the STAGING buffer if first call or count exceeds its capacity.
    E("    mov  rax, [_gpu_phys_stage_cap]");
    E("    cmp  r13, rax");
    E("    jbe  .phr_have");
    // STAGING, no bind, CPU_ACCESS_READ, MISC_STRUCTURED, stride 96, count*96 bytes.
    E("    lea  rdi, [rsp+0x40]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  rax, r13");
    E("    imul rax, RB_STRIDE");
    E("    mov  dword [rsp+0x40+BUFDESC_BYTEWIDTH], eax");
    E("    mov  dword [rsp+0x40+BUFDESC_USAGE], USAGE_STAGING");
    E("    mov  dword [rsp+0x40+BUFDESC_BIND], 0");
    E("    mov  dword [rsp+0x40+BUFDESC_CPUACCESS], D3DCPU_READ");
    E("    mov  dword [rsp+0x40+BUFDESC_MISC], MISC_STRUCTURED");
    E("    mov  dword [rsp+0x40+BUFDESC_STRIDE], RB_STRIDE");
    // Release any prior (smaller) staging buffer before replacing it.
    E("    mov  rax, [_gpu_phys_staging]");
    E("    test rax, rax");
    E("    jz   .phr_nostale");
    E("    mov  rcx, rax");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]");                   // IUnknown::Release (vtbl idx 2)
    E("    mov  qword [_gpu_phys_staging], 0");
    E(".phr_nostale:");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x40]");
    E("    xor  r8,  r8");                         // pInitialData = NULL
    E("    lea  r9,  [_gpu_phys_staging]");
    E("    call [rax + 0x18]");                    // CreateBuffer
    E("    test eax, eax");
    E("    jnz  .phr_ret");
    E("    mov  [_gpu_phys_stage_cap], r13");
    E(".phr_have:");

    // CopyResource(staging, bodies) -- full GPU->GPU copy of the resident bodies.
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_phys_staging]");        // pDstResource
    E("    mov  r8,  [_gpu_phys_bodies]");         // pSrcResource
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_COPYRES]");

    // Map(staging, 0, MAP_READ, 0, &mapped) -- mapped struct at [rsp+0x60].
    E("    lea  r11, [rsp+0x60]");
    E("    mov  [rsp+0x28], r11");                 // pMappedResource (arg5)
    E("    mov  dword [rsp+0x20], 0");             // MapFlags (arg4)
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_phys_staging]");
    E("    xor  r8d, r8d");                        // Subresource = 0
    E("    mov  r9d, MAP_READ");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x70]");                    // Map
    E("    test eax, eax");
    E("    jnz  .phr_ret");

    // memcpy dest_ptr <- mapped.pData, count*96 bytes (count*12 qwords).
    E("    mov  rsi, [rsp+0x60]");                 // mapped.pData
    E("    mov  rdi, r12");                        // dest_ptr
    E("    mov  rax, r13");
    E("    imul rax, RB_STRIDE / 8");              // qwords = count*12
    E("    mov  rcx, rax");
    E("    rep  movsq");

    // Unmap(staging, 0).
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_phys_staging]");
    E("    xor  r8d, r8d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x78]");                    // Unmap
    E(".phr_ret:");
    E("    add  rsp, 0x128");
    E("    pop  r15");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    ret");
    E("");
}

// GPU particle system: dead-pool compute simulation feeding billboard verts to the
// main draw. init creates the 2 compute shaders; step (re)creates the Particles
// (UAV) / render-verts (UAV+VERTEX) / cbuffer on first call or when capacity grows,
// zero-inits the pool once, uploads emitter params, then dispatches Emit+Simulate;
// draw binds the resident render-verts as the vertex buffer and issues one Draw over
// cap*6 verts (dead particles collapse to degenerate verts, skipped by the GPU).
// Reuses the physics equ set (BUFDESC_*, CTX_*, DEV_CREATE_*, USAGE_*, MAP_*).
static void emit_gpu_particles(Codegen *cg) {
    E("; --- particle compute equ (BIND_VERTEX/FMT_R32F already defined above) ---");
    E("PART_STRIDE     equ 64");          // sizeof(Particle)
    E("PART_CBSZ       equ 128");         // sizeof(EmitterConstants) (24 base + buoyancy/turbScale/turbFreq + pad)
    E("");

    // ---- _slag_gpu_particle_init: create Emit + Simulate compute shaders ----
    E("; --- _slag_gpu_particle_init ---");
    E("_slag_gpu_particle_init:");
    E("    push rbx");
    E("    sub  rsp, 0x30");                 // 1 push+0x30 -> 16-align; arg5 at [rsp+0x20]
    E("    cmp  qword [_gpu_part_ready], 0");
    E("    jne  .pti_ret");
    E("    mov  rbx, [_gpu_device]");
    E("    test rbx, rbx");
    E("    jz   .pti_ret");
    E("    lea  rax, [_gpu_cs_pemit]");
    E("    mov  [rsp+0x20], rax");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [_gpu_cs_pemit_blob]");
    E("    mov  r8,  _gpu_cs_pemit_blob_len");
    E("    xor  r9,  r9");
    E("    call [rax + DEV_CREATE_CS]");
    E("    test eax, eax");
    E("    jnz  .pti_ret");
    E("    lea  rax, [_gpu_cs_psim]");
    E("    mov  [rsp+0x20], rax");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [_gpu_cs_psim_blob]");
    E("    mov  r8,  _gpu_cs_psim_blob_len");
    E("    xor  r9,  r9");
    E("    call [rax + DEV_CREATE_CS]");
    E("    test eax, eax");
    E("    jnz  .pti_ret");
    E("    mov  qword [_gpu_part_ready], 1");
    E(".pti_ret:");
    E("    add  rsp, 0x30");
    E("    pop  rbx");
    E("    ret");
    E("");

    // ---- _slag_gpu_particle_step ----  r12=maxParticles  r14=params_ptr
    // (codegen loads r12/r14 before the call; see codegen.c particle_step)
    E("; --- _slag_gpu_particle_step ---  r12=maxParticles r14=params_ptr");
    E("_slag_gpu_particle_step:");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    push r15");
    E("    sub  rsp, 0x128");
    E("    cmp  qword [_gpu_part_ready], 0");
    E("    je   .pts_ret");
    E("    mov  rbx, [_gpu_device]");
    E("    test rbx, rbx");
    E("    jz   .pts_ret");
    E("    mov  r15, [_gpu_context]");
    E("    test r12, r12");                  // maxParticles<=0 -> nothing
    E("    jle  .pts_ret");
    E("    mov  [_gpu_part_cnt], r12");

    // (Re)create resources if first call or capacity exceeded.
    E("    mov  rax, [_gpu_part_cap]");
    E("    cmp  r12, rax");
    E("    jbe  .pts_have");

    // --- Particles buffer: DEFAULT, BIND_UAV, MISC_STRUCTURED, stride 64 ---
    E("    lea  rdi, [rsp+0x40]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  rax, r12");
    E("    imul rax, PART_STRIDE");
    E("    mov  dword [rsp+0x40+BUFDESC_BYTEWIDTH], eax");
    E("    mov  dword [rsp+0x40+BUFDESC_USAGE], USAGE_DEFAULT");
    E("    mov  dword [rsp+0x40+BUFDESC_BIND], BIND_UAV");
    E("    mov  dword [rsp+0x40+BUFDESC_CPUACCESS], 0");
    E("    mov  dword [rsp+0x40+BUFDESC_MISC], MISC_STRUCTURED");
    E("    mov  dword [rsp+0x40+BUFDESC_STRIDE], PART_STRIDE");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x40]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_part_buf]");
    E("    call [rax + 0x18]");              // CreateBuffer
    E("    test eax, eax");
    E("    jnz  .pts_ret");

    // Particles UAV: FORMAT_UNKNOWN, DIM_BUFFER, NumElements = maxParticles.
    E("    lea  rdi, [rsp+0x40]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x40+0], 0");
    E("    mov  dword [rsp+0x40+4], UAV_DIM_BUFFER");
    E("    mov  dword [rsp+0x40+8], 0");
    E("    mov  dword [rsp+0x40+12], r12d");
    E("    mov  dword [rsp+0x40+16], 0");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    mov  rdx, [_gpu_part_buf]");
    E("    lea  r8,  [rsp+0x40]");
    E("    lea  r9,  [_gpu_part_uav]");
    E("    call [rax + DEV_CREATE_UAV]");
    E("    test eax, eax");
    E("    jnz  .pts_ret");

    // --- Render verts: DEFAULT, BIND_UAV|BIND_VERTEX, ByteWidth = maxParticles*6*64.
    //     NOT structured: a D3D11 STRUCTURED buffer may not also bind as a vertex
    //     buffer. It is a plain byte buffer viewed two ways -- a typed R32_FLOAT UAV
    //     for the compute write (16 floats/vertex) and a raw vertex buffer for the
    //     draw (64B stride, the standard fill_triangle_gpu layout). ---
    E("    lea  rdi, [rsp+0x40]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  rax, r12");
    E("    imul rax, 6 * PART_STRIDE");       // 6 verts * 64B
    E("    mov  dword [rsp+0x40+BUFDESC_BYTEWIDTH], eax");
    E("    mov  dword [rsp+0x40+BUFDESC_USAGE], USAGE_DEFAULT");
    E("    mov  dword [rsp+0x40+BUFDESC_BIND], BIND_UAV | BIND_VERTEX");
    E("    mov  dword [rsp+0x40+BUFDESC_CPUACCESS], 0");
    E("    mov  dword [rsp+0x40+BUFDESC_MISC], 0");
    E("    mov  dword [rsp+0x40+BUFDESC_STRIDE], 0");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x40]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_part_verts]");
    E("    call [rax + 0x18]");
    E("    test eax, eax");
    E("    jnz  .pts_ret");

    // Render-verts UAV: typed R32_FLOAT, DIM_BUFFER, NumElements = maxParticles*6*16
    //     (16 float32 per vertex). The Simulate shader writes it as RWBuffer<float>.
    E("    lea  rdi, [rsp+0x40]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x40+0], FMT_R32F"); // DXGI_FORMAT_R32_FLOAT
    E("    mov  dword [rsp+0x40+4], UAV_DIM_BUFFER");
    E("    mov  dword [rsp+0x40+8], 0");
    E("    mov  rax, r12");
    E("    imul rax, 6 * 16");                 // verts * 16 floats
    E("    mov  dword [rsp+0x40+12], eax");
    E("    mov  dword [rsp+0x40+16], 0");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    mov  rdx, [_gpu_part_verts]");
    E("    lea  r8,  [rsp+0x40]");
    E("    lea  r9,  [_gpu_part_verts_uav]");
    E("    call [rax + DEV_CREATE_UAV]");
    E("    test eax, eax");
    E("    jnz  .pts_ret");

    // --- cbuffer: DYNAMIC, BIND_CONSTANT, 96B (once) ---
    E("    cmp  qword [_gpu_part_cbuf], 0");
    E("    jne  .pts_cap_set");
    E("    lea  rdi, [rsp+0x40]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  dword [rsp+0x40+BUFDESC_BYTEWIDTH], PART_CBSZ");
    E("    mov  dword [rsp+0x40+BUFDESC_USAGE], USAGE_DYNAMIC");
    E("    mov  dword [rsp+0x40+BUFDESC_BIND], BIND_CONSTANT");
    E("    mov  dword [rsp+0x40+BUFDESC_CPUACCESS], D3DCPU_WRITE");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x40]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_part_cbuf]");
    E("    call [rax + 0x18]");
    E("    test eax, eax");
    E("    jnz  .pts_ret");
    E(".pts_cap_set:");
    E("    mov  [_gpu_part_cap], r12");
    // Zero the new Particles pool once so every slot reads life=0 (dead) on the first
    // Emit -> the pool fills from empty instead of garbage. UpdateSubresource from a
    // zeroed scratch would need a maxParticles*64 buffer; instead the Emit shader
    // treats life<=0 as dead and DEFAULT buffers are driver-zeroed at creation, so
    // no explicit clear is required. (Marker: capacity just (re)allocated.)
    E(".pts_have:");

    // Upload emitter params to cbuffer (Map DISCARD, copy 96B, Unmap).
    E("    lea  r11, [rsp+0x60]");
    E("    mov  [rsp+0x28], r11");
    E("    mov  dword [rsp+0x20], 0");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_part_cbuf]");
    E("    xor  r8d, r8d");
    E("    mov  r9d, MAP_WR_DISCARD");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x70]");               // Map
    E("    test eax, eax");
    E("    jnz  .pts_ret");
    E("    mov  rdi, [rsp+0x60]");            // mapped.pData
    E("    mov  rsi, r14");                   // params_ptr
    E("    mov  ecx, PART_CBSZ / 8");         // 12 qwords
    E("    rep  movsq");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_part_cbuf]");
    E("    xor  r8d, r8d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x78]");               // Unmap

    // Bind cbuffer at b0: CSSetConstantBuffers(0,1,&cbuf).
    E("    mov  rax, [_gpu_part_cbuf]");
    E("    mov  [rsp+0x40], rax");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");
    E("    mov  r8d, 1");
    E("    lea  r9,  [rsp+0x40]");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETCB]");

    // groups = (maxParticles + 255) / 256 -> ebx.
    E("    lea  rax, [r12 + 255]");
    E("    shr  rax, 8");
    E("    mov  ebx, eax");

    // ===== PASS 1: Emit. Bind Particles UAV @u0. =====
    E("    mov  rax, [_gpu_part_uav]");
    E("    mov  [rsp+0x40], rax");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");                   // u0
    E("    mov  r8d, 1");
    E("    lea  r9,  [rsp+0x40]");
    E("    mov  qword [rsp+0x20], 0");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETUAV]");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_cs_pemit]");
    E("    xor  r8,  r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETSH]");
    E("    mov  rcx, r15");
    E("    mov  edx, ebx");
    E("    mov  r8d, 1");
    E("    mov  r9d, 1");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_DISPATCH]");

    // ===== PASS 2: Simulate. Bind render-verts UAV @u1 (Particles UAV still @u0). =====
    E("    mov  rax, [_gpu_part_verts_uav]");
    E("    mov  [rsp+0x40], rax");
    E("    mov  rcx, r15");
    E("    mov  edx, 1");                     // u1
    E("    mov  r8d, 1");
    E("    lea  r9,  [rsp+0x40]");
    E("    mov  qword [rsp+0x20], 0");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETUAV]");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_cs_psim]");
    E("    xor  r8,  r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETSH]");
    E("    mov  rcx, r15");
    E("    mov  edx, ebx");
    E("    mov  r8d, 1");
    E("    mov  r9d, 1");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_DISPATCH]");

    // Unbind both compute UAVs (u0,u1) so the render-verts buffer can bind as a
    // vertex buffer for the draw (a resource can't be UAV + VB simultaneously).
    E("    mov  qword [rsp+0x40], 0");
    E("    mov  qword [rsp+0x48], 0");
    E("    mov  rcx, r15");
    E("    xor  edx, edx");                   // start u0
    E("    mov  r8d, 2");                     // two slots
    E("    lea  r9,  [rsp+0x40]");
    E("    mov  qword [rsp+0x20], 0");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETUAV]");
    // Unbind the compute shader.
    E("    mov  rcx, r15");
    E("    xor  rdx, rdx");
    E("    xor  r8,  r8");
    E("    xor  r9d, r9d");
    E("    mov  rax, [r15]");
    E("    call [rax + CTX_CSSETSH]");
    E(".pts_ret:");
    E("    add  rsp, 0x128");
    E("    pop  r15");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    ret");
    E("");

    // ---- _slag_gpu_particle_draw ----  no args
    // Bind the resident render-verts buffer as the vertex buffer and draw cap*6
    // verts through the main VS/PS (billboard path). Assumes the main pipeline state
    // (input layout, topology, VS/PS, viewproj cbuffer, blend) is already set by the
    // present frame -- the draw only swaps the VB and issues one Draw. Dead particles
    // are degenerate (zero-size) verts, culled for free by the rasterizer.
    E("; --- _slag_gpu_particle_draw ---");
    E("_slag_gpu_particle_draw:");
    E("    push rbx");
    E("    push r15");
    E("    sub  rsp, 0x58");                      // 2 push + 0x58 -> 16-aligned
    E("    cmp  qword [_gpu_part_ready], 0");
    E("    je   .ptd_ret");
    E("    mov  rax, [_gpu_part_verts]");
    E("    test rax, rax");
    E("    jz   .ptd_ret");
    E("    mov  r15, [_gpu_context]");
    E("    test r15, r15");
    E("    jz   .ptd_ret");
    // Particles are transparent billboards: force straight-alpha blend ON and depth
    // WRITE OFF (test still on, so solid geometry occludes them, but overlapping
    // puffs accumulate instead of z-fighting). OMSetBlendState(_gpu_blend,NULL,~0).
    E("    mov  rdx, [_gpu_blend]");
    E("    test rdx, rdx");
    E("    jz   .ptd_noblend");
    E("    mov  rcx, r15");
    E("    xor  r8,  r8");
    E("    mov  r9d, 0xFFFFFFFF");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x118]");                  // OMSetBlendState
    E(".ptd_noblend:");
    // OMSetDepthStencilState(_gpu_dsstate_noZwrite, 0) if available; else leave as-is.
    E("    mov  rdx, [_gpu_dsstate_read]");
    E("    test rdx, rdx");
    E("    jz   .ptd_nods");
    E("    mov  rcx, r15");
    E("    xor  r8d, r8d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x120]");                  // OMSetDepthStencilState (read-only depth)
    E(".ptd_nods:");
    // CULL_NONE so the billboard quads' fixed winding is never back-face culled
    // (the whole reason the draw issued valid geometry yet nothing appeared).
    E("    mov  rdx, [_gpu_raster_none]");
    E("    test rdx, rdx");
    E("    jz   .ptd_noraster");
    E("    mov  rcx, r15");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x158]");                  // RSSetState(CULL_NONE)
    E(".ptd_noraster:");
    // IASetVertexBuffers(0, 1, ppVertexBuffers, pStrides, pOffsets). Args 5/6 (the
    // strides/offsets pointers) go in the stack shadow at [rsp+0x20]/[rsp+0x28];
    // the pointed-to values live at [rsp+0x38] (stride) / [rsp+0x3C] (offset), and
    // the vertex-buffer pointer at [rsp+0x30].
    E("    mov  dword [rsp+0x38], PART_STRIDE"); // *pStrides
    E("    mov  dword [rsp+0x3C], 0");           // *pOffsets
    E("    mov  rax, [_gpu_part_verts]");
    E("    mov  [rsp+0x30], rax");               // *ppVertexBuffers
    E("    lea  rax, [rsp+0x38]");
    E("    mov  [rsp+0x20], rax");               // pStrides (arg5)
    E("    lea  rax, [rsp+0x3C]");
    E("    mov  [rsp+0x28], rax");               // pOffsets (arg6)
    E("    mov  rcx, r15");
    E("    xor  edx, edx");                      // StartSlot 0
    E("    mov  r8d, 1");                        // NumBuffers
    E("    lea  r9,  [rsp+0x30]");               // ppVertexBuffers
    E("    mov  rax, [r15]");
    E("    call [rax + 0x90]");                  // IASetVertexBuffers
    // Draw(cap*6, 0).
    E("    mov  rax, [_gpu_part_cnt]");
    E("    imul rax, 6");
    E("    mov  rcx, r15");
    E("    mov  edx, eax");                      // VertexCount
    E("    xor  r8d, r8d");                      // StartVertexLocation
    E("    mov  rax, [r15]");
    E("    call [rax + 0x68]");                  // Draw
    E(".ptd_ret:");
    E("    add  rsp, 0x58");
    E("    pop  r15");
    E("    pop  rbx");
    E("    ret");
    E("");

    // ---- _slag_gpu_particle_probe ----  r12=dest_ptr
    // Write 8 runtime state qwords into [dest] so Slag can print the exact values the
    // DRAW path sees. No D3D calls -> cannot fault. Layout (8 x int64):
    //   [0] _gpu_part_ready   [1] _gpu_part_verts  [2] _gpu_part_cnt   [3] _gpu_part_cap
    //   [4] _gpu_blend        [5] _gpu_dsstate_read [6] _gpu_part_buf   [7] _gpu_part_uav
    E("; --- _slag_gpu_particle_probe ---  r12=dest_ptr");
    E("_slag_gpu_particle_probe:");
    E("    mov  rax, [_gpu_part_ready]");
    E("    mov  [r12 + 0],  rax");
    E("    mov  rax, [_gpu_part_verts]");
    E("    mov  [r12 + 8],  rax");
    E("    mov  rax, [_gpu_part_cnt]");
    E("    mov  [r12 + 16], rax");
    E("    mov  rax, [_gpu_part_cap]");
    E("    mov  [r12 + 24], rax");
    E("    mov  rax, [_gpu_blend]");
    E("    mov  [r12 + 32], rax");
    E("    mov  rax, [_gpu_dsstate_read]");
    E("    mov  [r12 + 40], rax");
    E("    mov  rax, [_gpu_part_buf]");
    E("    mov  [r12 + 48], rax");
    E("    mov  rax, [_gpu_part_uav]");
    E("    mov  [r12 + 56], rax");
    E("    ret");
    E("");

    // ---- _slag_gpu_particle_read ----  r12=dest_ptr r13=count(vertices)
    // Copy the GPU-resident render-verts buffer into a STAGING buffer, Map READ, and
    // memcpy count*64 bytes into the caller's CPU buffer (dest_ptr). Lets Slag read
    // the billboard vertices the compute Simulate pass wrote (each 64B = 16 f32:
    // pos3 uv2 col4 slice flag nrm3 pad2). count is a VERTEX count (particle i's 6
    // verts start at i*6). The staging buffer is created lazily and grown on demand.
    // Cloned from the proven _slag_gpu_physics_read; the render-verts buffer is NOT
    // structured, so the staging copy is a plain (non-structured) buffer.
    E("; --- _slag_gpu_particle_read ---  r12=dest_ptr r13=count(verts)");
    E("_slag_gpu_particle_read:");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    push r15");
    E("    sub  rsp, 0x128");
    E("    cmp  qword [_gpu_part_ready], 0");
    E("    je   .prr_ret");
    E("    cmp  qword [_gpu_part_verts], 0");       // nothing created yet
    E("    je   .prr_ret");
    E("    mov  rbx, [_gpu_device]");
    E("    test rbx, rbx");
    E("    jz   .prr_ret");
    E("    mov  r15, [_gpu_context]");
    E("    test r13, r13");                          // count<=0 -> nothing to do
    E("    jle  .prr_ret");

    // (Re)create the STAGING buffer if first call or count exceeds its capacity.
    E("    mov  rax, [_gpu_part_stage_cap]");
    E("    cmp  r13, rax");
    E("    jbe  .prr_have");
    // STAGING, no bind, CPU_ACCESS_READ, plain (NOT structured), count*64 bytes.
    E("    lea  rdi, [rsp+0x40]");
    E("    xor  eax, eax");
    E("    mov  ecx, 8");
    E("    rep  stosd");
    E("    mov  rax, r13");
    E("    imul rax, PART_STRIDE");                  // 64 B / vertex
    E("    mov  dword [rsp+0x40+BUFDESC_BYTEWIDTH], eax");
    E("    mov  dword [rsp+0x40+BUFDESC_USAGE], USAGE_STAGING");
    E("    mov  dword [rsp+0x40+BUFDESC_BIND], 0");
    E("    mov  dword [rsp+0x40+BUFDESC_CPUACCESS], D3DCPU_READ");
    E("    mov  dword [rsp+0x40+BUFDESC_MISC], 0");
    E("    mov  dword [rsp+0x40+BUFDESC_STRIDE], 0");
    // Release any prior (smaller) staging buffer before replacing it.
    E("    mov  rax, [_gpu_part_staging]");
    E("    test rax, rax");
    E("    jz   .prr_nostale");
    E("    mov  rcx, rax");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]");                      // IUnknown::Release
    E("    mov  qword [_gpu_part_staging], 0");
    E(".prr_nostale:");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x40]");
    E("    xor  r8,  r8");                            // pInitialData = NULL
    E("    lea  r9,  [_gpu_part_staging]");
    E("    call [rax + 0x18]");                       // CreateBuffer
    E("    test eax, eax");
    E("    jnz  .prr_ret");
    E("    mov  [_gpu_part_stage_cap], r13");
    E(".prr_have:");

    // CopySubresourceRegion(staging, 0, 0,0,0, render-verts, 0, &box) -- copy the
    // first count*64 bytes (a partial copy; the source buffer is larger, so a whole-
    // resource CopyResource would size-mismatch). Box: left=0, right=count*64.
    E("    mov  rax, r13");
    E("    imul rax, PART_STRIDE");                  // byte length = count*64
    E("    mov  dword [rsp+0x50], 0");               // box.left
    E("    mov  dword [rsp+0x54], 0");               // box.top
    E("    mov  dword [rsp+0x58], 0");               // box.front
    E("    mov  dword [rsp+0x5C], eax");             // box.right = count*64
    E("    mov  dword [rsp+0x60], 1");               // box.bottom
    E("    mov  dword [rsp+0x64], 1");               // box.back
    E("    mov  qword [rsp+0x20], 0");               // DstY
    E("    mov  qword [rsp+0x28], 0");               // DstZ
    E("    mov  rax, [_gpu_part_verts]");
    E("    mov  [rsp+0x30], rax");                   // pSrcResource
    E("    mov  qword [rsp+0x38], 0");               // SrcSubresource
    E("    lea  rax, [rsp+0x50]");
    E("    mov  [rsp+0x40], rax");                   // pSrcBox
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_part_staging]");          // pDstResource
    E("    xor  r8d, r8d");                          // DstSubresource
    E("    xor  r9d, r9d");                          // DstX
    E("    mov  rax, [r15]");
    E("    call [rax + 0x170]");                     // CopySubresourceRegion (vtbl idx 46)

    // Map(staging, 0, MAP_READ, 0, &mapped@[rsp+0x60]).
    E("    lea  r11, [rsp+0x60]");
    E("    mov  [rsp+0x28], r11");                   // pMappedResource (arg5)
    E("    mov  dword [rsp+0x20], 0");               // MapFlags (arg4)
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_part_staging]");
    E("    xor  r8d, r8d");                          // Subresource = 0
    E("    mov  r9d, MAP_READ");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x70]");                      // Map
    E("    test eax, eax");
    E("    jnz  .prr_ret");

    // memcpy dest_ptr <- mapped.pData, count*64 bytes (count*8 qwords).
    E("    mov  rsi, [rsp+0x60]");                   // mapped.pData
    E("    mov  rdi, r12");                          // dest_ptr
    E("    mov  rax, r13");
    E("    imul rax, PART_STRIDE / 8");              // qwords = count*8
    E("    mov  rcx, rax");
    E("    rep  movsq");

    // Unmap(staging, 0).
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_part_staging]");
    E("    xor  r8d, r8d");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x78]");                      // Unmap
    E(".prr_ret:");
    E("    add  rsp, 0x128");
    E("    pop  r15");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    ret");
    E("");
}

// _slag_gpu_set_dispmap(r12=ptr, r13=w, r14=h): upload a w*h R32F height map and
// bind it (as _gpu_dispsrv) at the DS's t3. Creates the texture + SRV on the first
// call or whenever w*h changes; re-uploads every call (cheap; the map may animate).
// ptr is w*h float32 values (RowPitch = w*4). No-op when no device is live.
static void emit_gpu_dispmap(Codegen *cg) {
    E("; --- _slag_gpu_set_dispmap(r12=ptr, r13=w, r14=h) ---");
    E("_slag_gpu_set_dispmap:");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    push r15");
    E("    sub  rsp, 0x68");                    // 4 push + 0x68 -> 16-aligned; desc/args scratch
    E("    mov  rbx, [_gpu_device]");
    E("    test rbx, rbx");
    E("    jz   .sdm_ret");
    E("    mov  r15, [_gpu_context]");
    E("    test r15, r15");
    E("    jz   .sdm_ret");
    E("    test r13, r13");                     // w==0 -> nothing
    E("    jz   .sdm_ret");
    E("    test r14, r14");
    E("    jz   .sdm_ret");

    // packed dim = (w<<32)|h; recreate the texture only when it changes.
    E("    mov  rax, r13");
    E("    shl  rax, 32");
    E("    or   rax, r14");
    E("    cmp  rax, [_gpu_disp_dim]");
    E("    je   .sdm_have");
    E("    mov  [_gpu_disp_dim], rax");
    // Release any prior texture + SRV before recreating at the new size.
    E("    mov  rcx, [_gpu_dispsrv]");
    E("    test rcx, rcx");
    E("    jz   .sdm_no_srv");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]");                 // SRV Release
    E("    mov  qword [_gpu_dispsrv], 0");
    E(".sdm_no_srv:");
    E("    mov  rcx, [_gpu_dispmap]");
    E("    test rcx, rcx");
    E("    jz   .sdm_no_tex");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]");                 // texture Release
    E("    mov  qword [_gpu_dispmap], 0");
    E(".sdm_no_tex:");

    // TEXTURE2D_DESC (44B) at rsp+0x20: w,h,Mip1,Array1,R32F,Sample1/0,DEFAULT,BIND_SRV.
    E("    lea  rdi, [rsp+0x20]");
    E("    xor  eax, eax");
    E("    mov  ecx, 11");
    E("    rep  stosd");
    E("    mov  [rsp+0x20+0], r13d");           // Width
    E("    mov  [rsp+0x20+4], r14d");           // Height
    E("    mov  dword [rsp+0x20+8], 1");        // MipLevels
    E("    mov  dword [rsp+0x20+12], 1");       // ArraySize
    E("    mov  dword [rsp+0x20+16], FMT_R32F");
    E("    mov  dword [rsp+0x20+20], 1");       // SampleDesc.Count
    E("    mov  dword [rsp+0x20+28], USAGE_DEFAULT");
    E("    mov  dword [rsp+0x20+32], BIND_SRV");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x20]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_dispmap]");
    E("    call [rax + 0x28]");                 // CreateTexture2D
    E("    test eax, eax");
    E("    jnz  .sdm_ret");
    // CreateShaderResourceView(tex, NULL, &_gpu_dispsrv) -- default desc, slot 0x38.
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    mov  rdx, [_gpu_dispmap]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_dispsrv]");
    E("    call [rax + 0x38]");                 // CreateShaderResourceView
    E("    test eax, eax");
    E("    jnz  .sdm_ret");
    E(".sdm_have:");

    // Upload: UpdateSubresource(tex,0,NULL,ptr,RowPitch=w*4,0) -- 7 args.
    E("    mov  rax, r13");
    E("    shl  rax, 2");                       // RowPitch = w*4
    E("    mov  [rsp+0x28], eax");              // SrcRowPitch (arg6)
    E("    mov  qword [rsp+0x30], 0");          // SrcDepthPitch (arg7)
    E("    mov  [rsp+0x20], r12");              // pSrcData (arg5)
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_dispmap]");          // pDstResource
    E("    xor  r8d, r8d");                     // DstSubresource 0
    E("    xor  r9,  r9");                      // pDstBox NULL
    E("    mov  rax, [r15]");
    E("    call [rax + 0x180]");                // UpdateSubresource
    E("    mov  [_gpu_disp_ptr], r12");
    E(".sdm_ret:");
    E("    add  rsp, 0x68");
    E("    pop  r15");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    ret");
    E("");
}

// _slag_gpu_set_normmap(r12=ptr, r13=w, r14=h): upload a w*h R8G8B8A8_UNORM normal
// map and bind it (_gpu_normsrv) at the DS's t4. Encoded normals: rgb = N*0.5+0.5
// (the DS decodes rgb*2-1). ptr is w*h*4 bytes (RowPitch = w*4). Creates on first
// call / dim change, re-uploads each call. No-op when no device is live. Mirrors
// _slag_gpu_set_dispmap but FMT_RGBA8 (DXGI_FORMAT_R8G8B8A8_UNORM = 28).
static void emit_gpu_normmap(Codegen *cg) {
    E("; --- _slag_gpu_set_normmap(r12=ptr, r13=w, r14=h) ---");
    E("FMT_RGBA8_UNORM  equ 28");
    E("_slag_gpu_set_normmap:");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    push r15");
    E("    sub  rsp, 0x68");
    E("    mov  rbx, [_gpu_device]");
    E("    test rbx, rbx");
    E("    jz   .snm_ret");
    E("    mov  r15, [_gpu_context]");
    E("    test r15, r15");
    E("    jz   .snm_ret");
    E("    test r13, r13");
    E("    jz   .snm_ret");
    E("    test r14, r14");
    E("    jz   .snm_ret");
    E("    mov  rax, r13");
    E("    shl  rax, 32");
    E("    or   rax, r14");
    E("    cmp  rax, [_gpu_norm_dim]");
    E("    je   .snm_have");
    E("    mov  [_gpu_norm_dim], rax");
    E("    mov  rcx, [_gpu_normsrv]");
    E("    test rcx, rcx");
    E("    jz   .snm_no_srv");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]");
    E("    mov  qword [_gpu_normsrv], 0");
    E(".snm_no_srv:");
    E("    mov  rcx, [_gpu_normmap]");
    E("    test rcx, rcx");
    E("    jz   .snm_no_tex");
    E("    mov  rax, [rcx]");
    E("    call [rax + 0x10]");
    E("    mov  qword [_gpu_normmap], 0");
    E(".snm_no_tex:");
    E("    lea  rdi, [rsp+0x20]");
    E("    xor  eax, eax");
    E("    mov  ecx, 11");
    E("    rep  stosd");
    E("    mov  [rsp+0x20+0], r13d");
    E("    mov  [rsp+0x20+4], r14d");
    E("    mov  dword [rsp+0x20+8], 1");
    E("    mov  dword [rsp+0x20+12], 1");
    E("    mov  dword [rsp+0x20+16], FMT_RGBA8_UNORM");
    E("    mov  dword [rsp+0x20+20], 1");
    E("    mov  dword [rsp+0x20+28], USAGE_DEFAULT");
    E("    mov  dword [rsp+0x20+32], BIND_SRV");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    lea  rdx, [rsp+0x20]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_normmap]");
    E("    call [rax + 0x28]");
    E("    test eax, eax");
    E("    jnz  .snm_ret");
    E("    mov  rcx, rbx");
    E("    mov  rax, [rbx]");
    E("    mov  rdx, [_gpu_normmap]");
    E("    xor  r8,  r8");
    E("    lea  r9,  [_gpu_normsrv]");
    E("    call [rax + 0x38]");
    E("    test eax, eax");
    E("    jnz  .snm_ret");
    E(".snm_have:");
    E("    mov  rax, r13");
    E("    shl  rax, 2");                       // RowPitch = w*4
    E("    mov  [rsp+0x28], eax");
    E("    mov  qword [rsp+0x30], 0");
    E("    mov  [rsp+0x20], r12");
    E("    mov  rcx, r15");
    E("    mov  rdx, [_gpu_normmap]");
    E("    xor  r8d, r8d");
    E("    xor  r9,  r9");
    E("    mov  rax, [r15]");
    E("    call [rax + 0x180]");                // UpdateSubresource
    E("    mov  [_gpu_norm_ptr], r12");
    E(".snm_ret:");
    E("    add  rsp, 0x68");
    E("    pop  r15");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    ret");
    E("");
}
