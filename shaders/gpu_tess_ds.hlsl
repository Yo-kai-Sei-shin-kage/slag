// Slag GPU tessellation DOMAIN shader. Runs once per tessellator-generated vertex.
// Barycentric-interpolate the tri control points, displace along the normal by a
// height map, recompute the normal (normal map, else finite-difference the
// height), THEN project. Emits the EXACT VOut the existing PS consumes, so fog /
// shadow / point-lights / SDF all work unchanged on tessellated geometry.
// Projects with lightVP during the shadow pass, viewproj otherwise, mirroring the
// direct VS's two-pass behavior so the shadow silhouette matches the displaced
// surface.
// Compile: fxc /T ds_5_0 /Zpr /Fo gpu_tess_ds.cso gpu_tess_ds.hlsl
Texture2D dispMap : register(t3);    // R32F height (0..1), separate from the t0 array
Texture2D normMap : register(t4);    // RGB tangent-space/world normal (0..1 encoded)
SamplerState smpDisp : register(s1); // linear+clamp (reuse the SDF linear sampler)
cbuffer C : register(b0) {
  row_major float4x4 viewproj;
  float3 fogColor; float fogStart;
  float fogInvRange; float2 invTexDims; float sunAng;
  row_major float4x4 lightVP;
  float3 camPos; float shadowPass;
  int lightCount; float3 _lpad;
  // Shared 224B cbuffer tail, byte-identical to gpu_tess_hs.hlsl:
  //   tessScale @192, tessMax @196, dispScale @200, useNormMap @204,
  //   dispTexel @208 (float2 = 1/w,1/h of dispMap), pad @216.
  float tessScale; float tessMax; float dispScale; float useNormMap;
  float2 dispTexel; float2 _tpad2;
};
struct HSOut {
  float3 pos:POSITION; float2 uv:TEXCOORD; float4 col:COLOR;
  float slice:TEXCOORD1; float flag:TEXCOORD2; float3 nrm:NORMAL;
};
struct PatchConst { float edges[3]:SV_TessFactor; float inside:SV_InsideTessFactor; };
struct DSOut {
  float4 pos:SV_POSITION; float2 uv:TEXCOORD; float4 col:COLOR; float slice:TEXCOORD1;
  float flag:TEXCOORD2; float3 wpos:TEXCOORD3; float4 lclip:TEXCOORD4; float vdepth:TEXCOORD5;
  float3 nrm:TEXCOORD6;
};
[domain("tri")]
DSOut main(PatchConst pc, float3 bary:SV_DomainLocation, const OutputPatch<HSOut,3> patch){
  DSOut o;
  // Barycentric interpolate the control-point payload.
  float3 pos = patch[0].pos*bary.x + patch[1].pos*bary.y + patch[2].pos*bary.z;
  float2 uv  = patch[0].uv *bary.x + patch[1].uv *bary.y + patch[2].uv *bary.z;
  float4 col = patch[0].col*bary.x + patch[1].col*bary.y + patch[2].col*bary.z;
  float3 nrm = normalize(patch[0].nrm*bary.x + patch[1].nrm*bary.y + patch[2].nrm*bary.z);
  float  slice = patch[0].slice;   // per-patch constant
  float  flag  = patch[0].flag;

  // dispMap uv normalized by invTexDims (raw texel coords, same convention as the
  // color texture). Displace along the interpolated normal. SKY patches (flag<1.5,
  // the unlit flag==1 backdrop) are NOT displaced -- they render flat.
  float2 duv = uv * invTexDims;
  bool isSky = (flag < 1.5);
  float h = dispMap.SampleLevel(smpDisp, duv, 0).r;
  if (!isSky) { pos += nrm * (h * dispScale); }

  // Recompute the normal on the displaced surface.
  float3 N = nrm;
  if (isSky) {
    N = nrm;
  } else if (useNormMap > 0.5) {
    float3 nm = normMap.SampleLevel(smpDisp, duv, 0).rgb * 2.0 - 1.0;
    N = normalize(nm);
  } else {
    // Finite-difference the height map: two tangents from neighboring texels.
    float hx = dispMap.SampleLevel(smpDisp, duv + float2(dispTexel.x,0), 0).r;
    float hy = dispMap.SampleLevel(smpDisp, duv + float2(0,dispTexel.y), 0).r;
    float3 tx = float3(dispTexel.x, (hx-h)*dispScale, 0);
    float3 ty = float3(0, (hy-h)*dispScale, dispTexel.y);
    N = normalize(cross(ty, tx));
  }

  o.wpos = pos;
  o.uv = uv * invTexDims;
  o.col = col;
  o.slice = slice;
  o.flag = flag;
  o.nrm = N;
  // Project: light POV during the shadow pass, camera otherwise.
  if (shadowPass > 0.5) {
    o.pos = mul(float4(pos,1), lightVP);
    o.lclip = float4(0,0,0,1); o.vdepth = 0.0;
  } else if (isSky) {
    // HORIZON-LOCKED SKY BACKDROP. pos.x,pos.y are NDC coords; pos.y is measured
    // FROM the horizon (0 = grey at the horizon, +1 = black at screen top). sunAng
    // carries the horizon's NDC-y for the current pitch (computed in build_camera),
    // so adding it slides the whole gradient with the camera -> the grey bottom sits
    // on the real terrain horizon whether the camera looks up or down. z is pinned
    // just inside the far plane so terrain always draws in front. No buffer rebuild.
    o.pos = float4(pos.x, pos.y + sunAng, 0.9999, 1.0);
    o.lclip = float4(0,0,0,1);
    o.vdepth = fogStart;   // <= fogStart so the flat PS branch applies no fog
  } else {
    o.pos = mul(float4(pos,1), viewproj);
    o.lclip = mul(float4(pos,1), lightVP);
    o.vdepth = o.pos.w;
  }
  return o;
}
