// Slag GPU tessellation VERTEX shader (patch control-point pass-through).
// Tessellated path only: IA(patchlist) -> VS -> HS -> [tess] -> DS -> PS.
// The direct fill_triangle_gpu path keeps its own projecting VS (unchanged).
// This VS does NO projection: it forwards the 64B control-point vertex in
// world space so the HS can measure edges in world space and the DS can
// displace THEN project. Projection at this stage would break tessellation.
// Compile: fxc /T vs_5_0 /Zpr /Fo gpu_tess_vs.cso gpu_tess_vs.hlsl
//
// 64B input vertex (16 f32), IDENTICAL layout to the direct path:
//   pos3, uv2 (RAW texel), col4 (0-255), slice, flag, nrm3, pad2.
struct VIn {
  float3 pos:POSITION; float2 uv:TEXCOORD; float4 col:COLOR;
  float slice:TEXCOORD1; float flag:TEXCOORD2; float3 nrm:NORMAL;
};
// Control point handed to the HS. World-space pos + all per-vertex payload the
// DS needs to reconstruct the final VOut the PS consumes.
struct HSIn {
  float3 pos:POSITION; float2 uv:TEXCOORD; float4 col:COLOR;
  float slice:TEXCOORD1; float flag:TEXCOORD2; float3 nrm:NORMAL;
};
HSIn main(VIn i){
  HSIn o;
  o.pos = i.pos;      // world-space, untransformed
  o.uv = i.uv;
  o.col = i.col;
  o.slice = i.slice;
  o.flag = i.flag;
  o.nrm = i.nrm;
  return o;
}
