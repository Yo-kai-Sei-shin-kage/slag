// Slag GPU vertex shader (fill_triangle_gpu / pcolor pipeline).
// Ubershader: textured/colored geo, fog, SDF text, billboard points, and now a
// two-pass shadow-mapped light for "lit" geometry.
// Compile: fxc /T vs_5_0 /Zpr /Fo gpu.vs.cso gpu.vs.hlsl
// Row-major cbuffer packing (/Zpr) matches the runtime's row-major matrix write.
cbuffer C : register(b0) {
  row_major float4x4 viewproj;         // 0   : camera view-projection
  float3 fogColor; float fogStart;     // 64  : fog color / start
  float fogInvRange; float2 invTexDims;// 80  : fog range recip / (1/texw,1/texh)
  float sunAng;                        // 92  : sun-cycle angle (was _pad0)
  row_major float4x4 lightVP;          // 96  : light view-projection (shadow map)
  float3 camPos; float shadowPass;     // 160 : camera world pos (radial fog) / shadow flag
};
// Direct-F32 vertex (48B, 11 f32): pos3, uv2 (RAW texel coords), col4 (0-255),
// slice, flag. flag: 1 = no fog (default geo), 0 = full fog, <0 = billboard point,
// 2 = LIT (shadow-mapped diffuse; the mesh sets this so UI/text stay unlit).
struct VIn  { float3 pos:POSITION; float2 uv:TEXCOORD; float4 col:COLOR; float slice:TEXCOORD1; float flag:TEXCOORD2; float3 nrm:NORMAL; };
struct VOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD; float4 col:COLOR; float slice:TEXCOORD1;
              float flag:TEXCOORD2; float3 wpos:TEXCOORD3; float4 lclip:TEXCOORD4; float vdepth:TEXCOORD5; float3 nrm:TEXCOORD6; };
VOut main(VIn i){
  VOut o;
  o.flag = i.flag;
  o.wpos = i.pos;
  o.nrm = i.nrm;

  // --- SHADOW DEPTH PASS -------------------------------------------------
  // shadowPass == 1.0: render from the light's POV into the shadow depth map.
  // Only LIT geometry (flag == 2) casts; everything else is pushed off-clip so
  // the UI/text/billboards do not write shadow depth. NOTE: the multi-light MAIN
  // pass sends shadowPass == 2.0 (PS array flag), which must NOT take this path,
  // so the gate is a bounded window around 1.0, not > 0.5.
  if (shadowPass > 0.5 && shadowPass < 1.5) {
    if (i.flag > 1.5 && i.flag < 2.5) {
      o.pos = mul(float4(i.pos,1), lightVP);
    } else {
      o.pos = float4(2,2,2,1);   // outside clip -> discarded
    }
    o.uv = float2(0,0); o.col = float4(0,0,0,0); o.slice = 0.0;
    o.lclip = float4(0,0,0,1); o.vdepth = 0.0; o.nrm = float3(0,1,0);
    return o;
  }

  // --- BILLBOARD MODE (flag < 0) -----------------------------------------
  // Camera-facing quad for the GPU particle system (particles.hlsl writes these).
  // Per-vertex contract: pos = particle center (world), uv = corner offset in
  // [-1,1] (already rotated by the particle's roll in the compute pass), |flag| =
  // world half-size, slice = smoke texture layer, col = rgba (color * fade alpha).
  // Expansion is screen-space (uv * size * w after projection), so the quad always
  // faces the camera; uv is remapped to [0,1] and passed through for the PS texture.
  if (i.flag < 0.0) {
    float4 cp = mul(float4(i.pos,1), viewproj);
    float sz = -i.flag;                 // |flag| carries the world half-size
    cp.x += i.uv.x * sz * cp.w;
    cp.y += i.uv.y * sz * cp.w;
    o.pos = cp;
    o.uv  = i.uv * 0.5 + 0.5;           // [-1,1] corner -> [0,1] texture coord
    o.slice = i.slice;                  // smoke texture layer (>= 0)
    o.col = i.col;                       // pass through color * fade alpha
    o.lclip = float4(0,0,0,1); o.vdepth = cp.w; o.nrm = float3(0,1,0);
    o.flag = -1.0;                       // keep PS on the billboard/textured branch
    return o;
  }

  // --- MAIN PASS ---------------------------------------------------------
  o.pos = mul(float4(i.pos,1), viewproj);
  o.uv  = i.uv * invTexDims;      // raw texel coords -> 0..1
  o.slice = i.slice;
  o.lclip = mul(float4(i.pos,1), lightVP);   // for the PS shadow lookup
  o.vdepth = o.pos.w;                          // view-space depth for PS fog
  // Linear distance fog, gated per-vertex by flag (flag=1 disables fog).
  // flag==2 (lit) also disables the vertex fog; the PS does its own lighting.
  float fogGate = 1.0;
  if (i.flag > 0.5) { fogGate = 0.0; }   // flag 1 or 2 -> no fog
  float fog = saturate((o.pos.w - fogStart) * fogInvRange) * fogGate;
  float3 c = i.col.rgb * (1.0/255.0);
  o.col.rgb = lerp(c, fogColor, fog);
  o.col.a   = i.col.a * (1.0/255.0);
  return o;
}
