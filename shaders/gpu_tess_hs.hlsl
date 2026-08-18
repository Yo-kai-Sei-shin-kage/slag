// Slag GPU tessellation HULL shader (tri patches, distance-adaptive, crack-free).
// Constant fn computes per-EDGE factors from camera distance so the shared edge
// between two patches gets the SAME factor from both -> no cracks. Frustum-cull:
// a patch fully outside clip space gets factor 0 -> discarded (free perf win).
// partitioning fractional_odd = continuous LOD (no popping). Winding cw to match
// the direct path's CULL_BACK (FrontCounterClockwise=FALSE) rasterizer state.
// Compile: fxc /T hs_5_0 /Zpr /Fo gpu_tess_hs.cso gpu_tess_hs.hlsl
cbuffer C : register(b0) {
  row_major float4x4 viewproj;
  float3 fogColor; float fogStart;
  float fogInvRange; float2 invTexDims; float sunAng;
  row_major float4x4 lightVP;
  float3 camPos; float shadowPass;
  int lightCount; float3 _lpad;
  // Tessellation controls (appended; HS/DS read, direct path ignores). Layout
  // MUST match gpu_tess_ds.hlsl byte-for-byte (shared cbuffer, 224B total):
  //   tessScale @192, tessMax @196, dispScale @200, useNormMap @204,
  //   dispTexel @208 (float2), pad @216.
  float tessScale; float tessMax; float dispScale; float useNormMap;
  float2 dispTexel; float2 _tpad2;
};
struct HSIn {
  float3 pos:POSITION; float2 uv:TEXCOORD; float4 col:COLOR;
  float slice:TEXCOORD1; float flag:TEXCOORD2; float3 nrm:NORMAL;
};
struct HSOut {
  float3 pos:POSITION; float2 uv:TEXCOORD; float4 col:COLOR;
  float slice:TEXCOORD1; float flag:TEXCOORD2; float3 nrm:NORMAL;
};
struct PatchConst { float edges[3]:SV_TessFactor; float inside:SV_InsideTessFactor; };

// Perspective-depth factor for a patch edge (midpoint of its two endpoints).
// Uses clip-space w (view-space depth), NOT raw 3D distance-to-camPos: w is
// positive and increases with distance for anything in front of the camera (the
// SAME w the working fog uses via o.vdepth = pos.w), so tessScale/w is
// unambiguously near->dense, far->sparse in SCREEN terms -- which is what LOD
// wants (detail where the surface is large on screen). Raw distance(m,camPos)
// inverted on a high, close camera because foreshortening != 3D distance.
float edge_factor(float3 a, float3 b){
  float3 m = (a + b) * 0.5;
  float4 c = mul(float4(m,1), viewproj);
  float w = max(c.w, 0.0001);
  float f = tessScale / w;                 // near (small w) -> large factor
  return clamp(f, 1.0, tessMax);
}
// Clip-space position of a base control point.
float4 clip_of(float3 wp){ return mul(float4(wp,1), viewproj); }
PatchConst ConstHS(InputPatch<HSIn,3> ip){
  PatchConst o;
  // SKY PATCH (flag < 1.5, i.e. the unlit flag==1 backdrop built into the same
  // buffer): emit tessfactor 1 -> NO subdivision, and no frustum/LOD work. The two
  // giant sky triangles stay flat and cheap while riding the single terrain draw.
  // The DS also skips displacement for these, so they render as a flat backdrop.
  if (ip[0].flag < 1.5) {
    o.edges[0] = 1.0; o.edges[1] = 1.0; o.edges[2] = 1.0; o.inside = 1.0;
    return o;
  }
  // PER-PLANE UNANIMOUS frustum cull: drop the patch only when ALL THREE control
  // points lie beyond the SAME frustum plane (e.g. all off the right). A patch
  // that merely straddles -- one CP off-left, another off-right -- is ON screen
  // and must be kept. The previous code OR'd all planes per point then AND'd the
  // points, which culled straddling corner patches (residual near-corner clips).
  //
  // The frustum test runs on UNDISPLACED (y=0) control points; the domain shader
  // later displaces each vertex by up to dispScale along its normal, so a patch
  // whose base CPs sit just outside can still show displaced geometry. Hence:
  //   - a generous guard band on the lateral planes, and
  //   - NO near-plane (c.z<0) cull at all -- the GPU hardware near clipper owns
  //     the near plane correctly per-pixel; culling here punched holes in the
  //     near ground under the camera.
  float4 c0 = clip_of(ip[0].pos);
  float4 c1 = clip_of(ip[1].pos);
  float4 c2 = clip_of(ip[2].pos);
  // If ANY control point is at/behind the near plane (w <= 0), the patch straddles
  // the near plane. The guard band g = w*2 goes negative there, which INVERTS every
  // x/y comparison and wrongly culls a patch directly under the camera (the tiny
  // notch at the bottom edge). Such a patch must ALWAYS be kept -- the hardware near
  // clipper resolves it per-pixel. Only run the frustum cull when all three CPs are
  // safely in front of the camera.
  // Keep any patch with a control point near OR behind the camera plane. At large
  // (e.g. 32m) patch scale a patch under the camera has base CPs whose w dips toward
  // 0; the frustum test's guard g=w*2 collapses there and wrongly culls it (ground
  // vanishes near the camera). Use a generous near margin, not just w<=0, so these
  // straddling near patches are ALWAYS kept -- the hardware near clipper handles them.
  float wmin = min(c0.w, min(c1.w, c2.w));
  float nearMargin = 64.0;   // world units; > one patch's worth of near straddle
  if (wmin > nearMargin) {
    float g0 = c0.w * 2.0 + 0.0001;
    float g1 = c1.w * 2.0 + 0.0001;
    float g2 = c2.w * 2.0 + 0.0001;
    bool cullR = (c0.x >  g0) && (c1.x >  g1) && (c2.x >  g2);
    bool cullL = (c0.x < -g0) && (c1.x < -g1) && (c2.x < -g2);
    bool cullT = (c0.y >  g0) && (c1.y >  g1) && (c2.y >  g2);
    bool cullB = (c0.y < -g0) && (c1.y < -g1) && (c2.y < -g2);
    bool cullF = (c0.z > c0.w) && (c1.z > c1.w) && (c2.z > c2.w);   // far plane
    if (cullR || cullL || cullT || cullB || cullF) {
      o.edges[0] = 0.0; o.edges[1] = 0.0; o.edges[2] = 0.0; o.inside = 0.0;
      return o;
    }
  }
  // SV_TessFactor[i] is the edge OPPOSITE control point i:
  //   edge0 = cp1..cp2, edge1 = cp2..cp0, edge2 = cp0..cp1.
  o.edges[0] = edge_factor(ip[1].pos, ip[2].pos);
  o.edges[1] = edge_factor(ip[2].pos, ip[0].pos);
  o.edges[2] = edge_factor(ip[0].pos, ip[1].pos);
  o.inside = (o.edges[0] + o.edges[1] + o.edges[2]) / 3.0;
  return o;
}
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("ConstHS")]
HSOut main(InputPatch<HSIn,3> ip, uint id:SV_OutputControlPointID){
  HSOut o;
  o.pos = ip[id].pos;
  o.uv = ip[id].uv;
  o.col = ip[id].col;
  o.slice = ip[id].slice;
  o.flag = ip[id].flag;
  o.nrm = ip[id].nrm;
  return o;
}
