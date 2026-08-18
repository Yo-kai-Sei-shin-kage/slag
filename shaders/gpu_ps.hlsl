// Slag GPU pixel shader (fill_triangle_gpu / pcolor pipeline). Ubershader.
// Adds shadow-mapped diffuse lighting for LIT geometry (flag == 2): per-pixel
// geometric normal from ddx/ddy(worldPos), N.L diffuse, and a shadow-map term
// sampled from the light-space depth (t2). UI/text/textures (other flags) are
// untouched.
// Compile: fxc /T ps_5_0 /Fo gpu_ps.cso gpu_ps.hlsl
Texture2DArray tex : register(t0);
Texture2DArray shadowMap : register(t2);   // light-space depth array (one slice per light)
SamplerState smp : register(s0);      // point+clamp: world textures
SamplerState smpLin : register(s1);   // linear+clamp: SDF text distance
SamplerComparisonState smpCmp : register(s2);  // comparison LEQUAL: hardware PCF shadow

// Dynamic point-light set. Sized by the app (gpu.set_lights ptr+count) and bound
// as an SRV at t1, so the count is not capped by the 176-byte cbuffer. Each light
// is a point source: pos (world), color (0..1 rgb), range (radial falloff radius),
// castShadows (nonzero -> modulated by the single shadow map on lightVP).
struct Light { float3 pos; float3 color; float range; int castShadows; };
StructuredBuffer<Light> lights : register(t1);

// Per-light view-projection matrices for multi-light shadow mapping (gpu.set_lightproj_array).
// lightVPs[i] projects world -> light i's shadow-map clip space; light i's depth map is
// slice i of the shadowMap array. Bound at t5. Indexed by the light loop below.
struct LightVP { row_major float4x4 m; };
StructuredBuffer<LightVP> lightVPs : register(t5);

// Hardware 3x3 PCF. shadowPosH = light-clip position AFTER perspective divide,
// remapped to [0,1] shadow-map UV with the Y flip. bias = slope-scaled depth bias
// (computed by the caller from ddx/ddy in uniform flow, since derivatives are
// undefined inside this loop). Returns 0 (shadowed) .. 1 (lit).
float ShadowPCF3x3(float3 shadowPosH, float bias, float slice) {
  if (shadowPosH.z > 1.0) { return 1.0; }   // beyond the light far plane -> lit
  // Outside the shadow map's [0,1] UV bounds there is no depth data -- the CLAMP
  // sampler would read the border texels and smear a false shadow stripe across the
  // scene along the light axis. Treat anything off the map as fully lit.
  if (shadowPosH.x < 0.0 || shadowPosH.x > 1.0 ||
      shadowPosH.y < 0.0 || shadowPosH.y > 1.0) { return 1.0; }
  const float dx = 1.0 / 1024.0;            // shadow map slice is 1024x1024
  float z = shadowPosH.z - bias;
  float lit = 0.0;
  [unroll] for (int y = -1; y <= 1; y = y + 1) {
    [unroll] for (int x = -1; x <= 1; x = x + 1) {
      float2 o = float2(x * dx, y * dx);
      lit += shadowMap.SampleCmpLevelZero(smpCmp, float3(shadowPosH.xy + o, slice), z).r;
    }
  }
  return lit / 9.0;
}

cbuffer C : register(b0) {
  row_major float4x4 viewproj;
  float3 fogColor; float fogStart;
  float fogInvRange; float2 invTexDims; float sunAng;
  row_major float4x4 lightVP;
  float3 camPos; float shadowPass;   // camPos (was lightDir): world camera position for radial fog
  int lightCount; float3 _lpad;      // number of active lights in the StructuredBuffer (t1)
};

struct PIn { float4 pos:SV_POSITION; float2 uv:TEXCOORD; float4 col:COLOR; float slice:TEXCOORD1;
             float flag:TEXCOORD2; float3 wpos:TEXCOORD3; float4 lclip:TEXCOORD4; float vdepth:TEXCOORD5;
             float3 nrm:TEXCOORD6; };

float4 main(PIn i):SV_TARGET {
  // SDF text: abs(slice) is the atlas slice, distance in .r.
  if (i.slice < 0.0) {
    float d = tex.Sample(smpLin, float3(i.uv, -i.slice)).r;
    float a = smoothstep(0.46, 0.54, d);
    return float4(i.col.rgb, i.col.a * a);
  }

  // BILLBOARD PARTICLE (flag < 0): soft smoke/fire/dust puff. The smoke texture's
  // SHAPE lives in its alpha channel (soft wisp), so the puff is shaped by the
  // sampled texture alpha, not a hard quad. A radial edge fade (1 - r^2 from the
  // billboard center) feathers the border so overlapping puffs read volumetric
  // without a hard rim -- the cheap, depth-free soft-particle approximation. Final
  // alpha = texture.a * edge * per-particle fade (i.col.a from the compute pass).
  if (i.flag < 0.0) {
    // Puff shape comes ONLY from the smoke texture's soft alpha (no extra radial
    // feather -- stacking two radial falloffs quantized to 8-bit alpha produced the
    // concentric banding rings). Per-particle fade in i.col.a.
    float4 tx = tex.Sample(smpLin, float3(i.uv, i.slice));
    return float4(i.col.rgb * tx.rgb, tx.a * i.col.a);
  }

  // SKY backdrop (flag==1, unlit, screen-locked): use the vertex color DIRECTLY,
  // skipping the ground texture so the grey->black sky gradient is never tinted by
  // the grass. (flag==2 is lit terrain; flag>=0 non-sky uses the textured base.)
  if (i.flag > 0.5 && i.flag < 1.5) {
    return float4(i.col.rgb, i.col.a);
  }

  // Ground texture TILED by world position (not the shared 0..1 terrain UV, which
  // stretches one texture across the whole 16km world). TWO-SCALE DOMAIN WARP to kill
  // the visible tile repeat: sample the grass at two MISMATCHED tile scales (8m and
  // 37m -- non-integer ratio so their grids never coincide) and blend. The regular
  // 8m lattice dissolves into non-repeating variation. frac() wraps each; the second
  // scale is offset so the two lookups don't correlate. Point+clamp is fine at the
  // frac seam (no bilinear bleed). Sky (flag==1) already returned above.
  float2 guv0 = frac(i.wpos.xz * (1.0 / 8.0));
  float2 guv1 = frac(i.wpos.xz * (1.0 / 37.0) + float2(0.37, 0.61));
  float3 t0 = tex.Sample(smp, float3(guv0, i.slice)).rgb;
  float3 t1 = tex.Sample(smp, float3(guv1, i.slice)).rgb;
  // Average the two mismatched scales (slightly brightened to offset the averaging
  // toward the mean), so neither grid dominates and the repeat dissolves.
  float3 t = (t0 + t1) * 0.5 * 1.12;
  float3 base = t * i.col.rgb;

  // LIT geometry (flag == 2): the EXACT CPU lighting model ported to the GPU.
  // CPU (asset_creator draw_mesh): bright = 60 + max(0, dot(N,L)*195), capped 256,
  // color *= bright/256. L is a FIXED pre-normalized constant (no cbuffer light),
  // ambient floor 60/256. No shadow map, no sunAng, no fog on lit geometry --
  // this mirrors what the CPU actually does, nothing more.
  if (i.flag > 1.5 && i.flag < 2.5) {
    float3 N = normalize(i.nrm);
    // Ambient floor: 5% (was 60/256 ~= 23%).
    float amb = 13.0 / 256.0;
    // Accumulate every point light's diffuse contribution. Per light:
    //   ndl  = max(0, N.L),  L = normalize(pos - wpos)
    //   diff = ndl * 195/256  (same slope as the single-light CPU model)
    //   fall = physically-correct inverse-square attenuation, scaled by range:
    //          att = 1 / (1 + (dist/range)^2). ~1 at the light, ~1/dist^2 far out.
    //   Combined with N.L, a LOW light makes a small intense hotspot (nearby
    //   ground is close -> att high, far ground drops fast); a HIGH light is a
    //   broad dim wash (all ground roughly equidistant). Real inverse-square.
    //   shadow term applied only for castShadows lights (per-light shadow map).
    // Light-space shadow coord. Grazing-angle acne is handled by the HARDWARE
    // slope-scaled depth bias applied in the shadow-pass rasterizer (write side),
    // so the PS only needs a tiny constant read-side epsilon here.
    // Two shadow modes, selected by the runtime via shadowPass @172:
    //   0.0 = single-light: sh from the cbuffer lightVP, sampled at slice 0.
    //   2.0 = multi-light array: per light, project into lightVPs[li] (t5) and
    //         sample slice li. Signalled by shadowPass > 1.5.
    float shBias = 0.0005;
    bool useArray = (shadowPass > 1.5);
    float3 accum = float3(0.0, 0.0, 0.0);
    for (int li = 0; li < lightCount; li = li + 1) {
      Light lt = lights[li];
      float3 d = lt.pos - i.wpos;
      float dist = length(d);
      float3 L = d / max(dist, 0.0001);
      float ndl = max(0.0, dot(N, L));
      float fall = 1.0;
      if (lt.range > 0.0) {
        float dr = dist / lt.range;
        fall = 1.0 / (1.0 + dr * dr);
      }
      float lit = 1.0;
      if (lt.castShadows != 0) {
        if (useArray) {
          // per-light shadow: project into light li's clip space, sample slice li.
          float4 lc = mul(float4(i.wpos, 1.0), lightVPs[li].m);
          float3 shi;
          shi.x = lc.x / lc.w * 0.5 + 0.5;
          shi.y = 0.5 - lc.y / lc.w * 0.5;
          shi.z = lc.z / lc.w;
          lit = ShadowPCF3x3(shi, shBias, (float)li);
        } else {
          // single map = slice 0; sh from the VS-interpolated lightVP clip.
          float3 sh;
          sh.x = i.lclip.x / i.lclip.w * 0.5 + 0.5;
          sh.y = 0.5 - i.lclip.y / i.lclip.w * 0.5;
          sh.z = i.lclip.z / i.lclip.w;
          lit = ShadowPCF3x3(sh, shBias, 0.0);
        }
      }
      // Intensity gain > 1 so a near-ground light (att ~ 1) OVERDRIVES past the
      // flat surface color -> a bright, near-white hotspot; inverse-square then
      // falls to a dim broad wash farther out. accum is intentionally unclamped
      // (final base*shade can exceed 1 -> blows out bright near the source).
      float diff = ndl * 3.0 * fall * lit;
      accum += diff * lt.color;
    }
    float3 shade = amb.xxx + accum;
    float3 litRgb = base * shade;
    // Distance fog on LIT geometry. The VS skips fog for flag==2 (PS owns shading),
    // so apply the SAME linear distance term here. CRITICAL: the lit color is
    // unclamped HDR (accum has a 3x gain, so litRgb can be >> 1). Lerping fog into an
    // over-bright color leaves the lit term swamping fogColor -> fog invisible ("lit
    // branch draws over the fog"). SATURATE the lit color to 0..1 FIRST so fog lerps
    // between a real displayed color and fogColor, exactly like the VS does at 0..1.
    float fogL = saturate((i.vdepth - fogStart) * fogInvRange);
    litRgb = lerp(saturate(litRgb), fogColor, fogL);
    return float4(litRgb, i.col.a);
  }

  return float4(base, i.col.a);
}
