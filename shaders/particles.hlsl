// =================================================================================
// Slag GPU particle system compute shader. Shader Model 5.0. GPU-resident dead-pool
// particle simulation for volumetric-looking smoke, fire, sparks, dust and debris.
// Each thread owns one particle slot; the system integrates motion, ages/fades, and
// writes camera-facing billboard vertices straight into a render buffer that
// fill_triangle_gpu draws with no CPU round-trip.
//
// Two entry points, each compiled to its own .cso and embedded in the runtime:
//   fxc /T cs_5_0 /E Emit     /Fo particles_emit.cso particles.hlsl
//   fxc /T cs_5_0 /E Simulate /Fo particles_sim.cso  particles.hlsl
//
// --- RUNTIME BUFFER / VIEW LAYOUT (authoritative) --------------------------------
//   Particles buffer : ONE D3D11 buffer, StructureByteStride = 64 (see Particle),
//                      BIND_UNORDERED_ACCESS | MISC_BUFFER_STRUCTURED, bound u0.
//                      GPU-resident: state persists between frames.
//   RenderVerts      : ONE D3D11 buffer, the 64-byte GPU vertex layout (16 f32),
//                      capacity = maxParticles * 6 verts. BIND_UNORDERED_ACCESS |
//                      BIND_SHADER_RESOURCE (also drawn as a vertex buffer),
//                      MISC_BUFFER_STRUCTURED stride 64, bound u1 in Simulate.
//   EmitterConstants cbuffer : 96 bytes, bound b0.
//
// --- PER-FRAME CPU DISPATCH ORDER (maxParticles threads; groups = ceil/256) -------
//   1. Emit     : bind u0=Particles. Recycle dead slots -> spawn from emitter params
//                 (respawnBudget claimed via an atomic counter in the cbuffer copy).
//   2. Simulate : bind u0=Particles, u1=RenderVerts. Integrate + age + fade; write
//                 6 billboard verts per LIVE particle, degenerate (zero) verts for
//                 dead slots so the single Draw skips them.
// =================================================================================

#define THREAD_GROUP_SIZE 256
#define TWO_PI 6.28318530718

// -----------------------------------------------------------------------------
// Per-particle state. StructureByteStride = 64 (four 16-byte rows, fully packed).
//   position    @0   world-space center
//   age         @12  seconds alive (>= life -> dead / recyclable)
//   velocity    @16  world linear velocity
//   life        @28  total lifetime in seconds (0 = slot never spawned / permanently dead)
//   color       @32  rgba base color 0..1 (a is the peak alpha at mid-life)
//   size0       @48  billboard half-size at birth (world units)
//   sizeRate    @52  size growth per second (smoke expands as it rises)
//   rot0        @56  initial billboard roll (radians)
//   rotRate     @60  roll angular velocity (rad/s) -> stride 64
// -----------------------------------------------------------------------------
struct Particle
{
    float3 position;  float age;      // 0
    float3 velocity;  float life;     // 16
    float4 color;                     // 32
    float  size0;  float sizeRate;  float rot0;  float rotRate;  // 48 -> 64
};
RWStructuredBuffer<Particle> Particles : register(u0);

// Output vertices are written into a TYPED R32_FLOAT UAV (a plain vertex buffer that
// also binds BIND_UNORDERED_ACCESS; a D3D11 structured buffer may not bind as a
// vertex buffer, so it is addressed here as a flat float stream). Each vertex is the
// 64-byte fill_triangle_gpu layout (16 f32): pos3 @0, uv2, rgba, slice, flag, nrm3,
// pad2. Vertex v's float f lives at linear index v*16 + f.
//   Billboard contract: flag = -size (negative -> billboard mode in the VS, magnitude
//   = world half-size), uv = rotated corner in [-1,1] (VS expands it camera-facing,
//   PS remaps to [0,1] for the smoke texture), slice = texture layer.
RWBuffer<float> RenderVerts : register(u1);

// Write one 16-float vertex at vertex index vi.
void WriteVertex(uint vi, float3 pos, float2 uv, float4 col, float slice, float flag)
{
    uint o = vi * 16u;
    RenderVerts[o + 0u]  = pos.x;
    RenderVerts[o + 1u]  = pos.y;
    RenderVerts[o + 2u]  = pos.z;
    RenderVerts[o + 3u]  = uv.x;
    RenderVerts[o + 4u]  = uv.y;
    RenderVerts[o + 5u]  = col.r;
    RenderVerts[o + 6u]  = col.g;
    RenderVerts[o + 7u]  = col.b;
    RenderVerts[o + 8u]  = col.a;
    RenderVerts[o + 9u]  = slice;
    RenderVerts[o + 10u] = flag;
    RenderVerts[o + 11u] = 0.0;   // nx
    RenderVerts[o + 12u] = 0.0;   // ny
    RenderVerts[o + 13u] = 0.0;   // nz
    RenderVerts[o + 14u] = 0.0;   // pad
    RenderVerts[o + 15u] = 0.0;   // pad
}

cbuffer EmitterConstants : register(b0)
{
    float3 emitOrigin;   float  deltaTime;    // 0   : spawn point / frame dt
    float3 emitVelocity; float  emitSpread;   // 16  : base spawn velocity / random cone spread
    float3 gravity;      float  drag;         // 32  : accel / linear damping per second
    float4 baseColor;                         // 48  : spawn rgba (a = peak alpha)
    float  lifeMin;    float lifeMax;         // 64  : lifetime range (s)
    float  sizeStart;  float sizeGrow;        // 72  : birth half-size / growth rate (world/s)
    float  groundY;    float bounce;          // 80  : ground plane / restitution (0 = no bounce)
    float  texSlice;   float randSeed;        // 88  : smoke texture layer / per-frame RNG salt
    // Behavior knobs (Slag-settable): 0 for clean ballistic effects (sparks/debris),
    // high for smoke/fire. buoyancy = upward accel that fades over life; turbScale =
    // curl-turbulence strength (0 = straight arcs); turbFreq = swirl tightness.
    float  buoyancy;   float turbScale;       // 96  : upward accel / churn strength
    float  turbFreq;   float prewarm;         // 104 : swirl frequency / cold-start pre-age (0..1)
    float3 wind;       float emitRate;        // 112 : wind accel / per-frame spawn fraction (Slag ramps 0->target)
};                                            // 128 total

// --- hash / RNG (deterministic per slot per frame) ---------------------------
// PCG-style integer hash -> [0,1). Cheap, well-distributed; no groupshared needed.
float hash11(uint n)
{
    n = n * 747796405u + 2891336453u;
    uint w = ((n >> ((n >> 28) + 4u)) ^ n) * 277803737u;
    w = (w >> 22) ^ w;
    return (float)w * (1.0 / 4294967296.0);
}

// --- Pass 1: recycle dead slots and (re)spawn from the emitter -------------------
// A slot is dead when age >= life (or life == 0 for a never-spawned slot). Every
// dead slot respawns each frame at the emitter with randomized velocity/lifetime,
// giving a continuous stream. To throttle the spawn RATE, the CPU shrinks the live
// pool by pre-seeding lifetimes; here we simply refill whatever has died. Random
// draws are salted by randSeed so the stream differs each frame.
[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void Emit(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint i = dispatchThreadID.x;
    uint count = 0, stride = 0;
    Particles.GetDimensions(count, stride);
    if (i >= count) return;

    Particle p = Particles[i];

    bool dead = (p.life <= 0.0) || (p.age >= p.life);
    if (!dead) return;

    // random draws for this slot this frame
    uint  base = i * 5u + (uint)(randSeed * 65536.0);
    float r0 = hash11(base + 0u);
    float r1 = hash11(base + 1u);
    float r2 = hash11(base + 2u);
    float r3 = hash11(base + 3u);
    float r4 = hash11(base + 4u);            // independent draw for the prewarm age

    // SPAWN THROTTLE: a saturated pool respawns every dead slot instantly, so the
    // cloud is always full -> a steady ball. Instead only a small fraction of dead
    // slots respawn each frame, keeping a continuous birth->rise->death gradient
    // (a real plume). emitSpread doubles as the per-frame spawn probability's inverse
    // is not used; use a fixed low gate salted per slot so the stream is smooth.
    if (r3 > emitRate) { return; }           // Slag-driven spawn fraction (ramp from ~0 at start)

    // POSITION: spawn in a small disc at the origin (not a single point), so puffs
    // do not all radiate from one spot. Radius scales with emitSpread.
    float ang  = r0 * TWO_PI;
    float rad  = sqrt(r1) * emitSpread;      // sqrt -> uniform disc
    p.position = emitOrigin + float3(cos(ang) * rad, 0.0, sin(ang) * rad);

    // VELOCITY: strongly biased UP (emitVelocity, ~+Y), only a SMALL lateral jitter
    // -> a rising column, not a symmetric sphere. Lateral is a fraction of spread.
    float2 lat = (float2(r1, r2) - 0.5) * emitSpread * 0.6;
    p.velocity = emitVelocity + float3(lat.x, 0.0, lat.y);

    p.life     = lerp(lifeMin, lifeMax, r2);
    // PREWARM: cold start has all slots dead at once -> a coincident newborn cluster
    // (a static pod) until they age enough for turbulence/buoyancy to ramp in. Give
    // each spawn a random initial age spread across prewarm*life, and pre-advance its
    // position by that age along its spawn velocity, so it is born partway up the
    // plume already moving -> an established column from the first frame. prewarm=0
    // spawns fresh at the origin (ignition builds from nothing); 1 = fully populated.
    // prewarm spreads spawns across prewarm*life (established column). Even at
    // prewarm=0 add a tiny age jitter (~0..0.2s) so the batch that spawns on a given
    // frame is not a coincident age-0 stack at one point (which reads as a dense pod
    // before turbulence ramps in) -- it is smeared along the first cm of the rise.
    float a0 = (r4 * prewarm * p.life) + (r2 * 0.2);
    p.age    = a0;
    p.position += p.velocity * a0;
    p.color    = baseColor;
    p.size0    = sizeStart;
    p.sizeRate = sizeGrow;
    p.rot0     = r0 * TWO_PI;                 // random initial roll
    p.rotRate  = (r2 - 0.5) * 1.2;            // slow random spin

    Particles[i] = p;
}

// --- Pass 2: integrate, age/fade, and emit billboard vertices --------------------
// Six vertices per live particle (two triangles). Dead slots emit six zeroed
// (degenerate) vertices so the one Draw over maxParticles*6 verts skips them for
// free. Corner offsets are rotated by the particle's current roll in the compute
// pass; the VS expands them camera-facing, so the roll survives the billboard.
[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void Simulate(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint i = dispatchThreadID.x;
    uint count = 0, stride = 0;
    Particles.GetDimensions(count, stride);
    if (i >= count) return;

    uint vbase = i * 6u;

    Particle p = Particles[i];
    bool dead = (p.life <= 0.0) || (p.age >= p.life);

    if (!dead)
    {
        // --- integrate (semi-implicit Euler) ---
        // gravity (usually vertical) and wind (horizontal) are independent constant
        // accelerations, so an effect can have real downward gravity AND a side wind
        // at once (e.g. windblown falling debris). Both applied every frame; wind bends
        // the plume over more the longer a particle lives / the higher it climbs.
        p.velocity += gravity * deltaTime;
        p.velocity += wind * deltaTime;
        p.velocity *= saturate(1.0 - drag * deltaTime);

        // --- CURL TURBULENCE (turbScale): a divergence-free-ish swirl that makes the
        // column churn and billow instead of moving in straight lines. Sine bands of
        // world position + time give neighboring particles a rotating push (lateral
        // only, so rise is preserved). Strength ramps in with age. turbScale = 0 gives
        // clean ballistic motion (sparks/debris); high gives rolling smoke. turbFreq
        // sets the swirl tightness (spatial frequency of the bands).
        if (turbScale > 0.0)
        {
            float  f    = max(turbFreq, 0.0001);
            float3 wp   = p.position * f + float3(0.0, p.age * 1.5, 0.0);
            float  n1   = sin(wp.x + 1.7 * sin(wp.z)) * cos(wp.z * 0.9 + 1.3);
            float  n2   = sin(wp.z + 1.7 * sin(wp.x)) * cos(wp.x * 0.9 + 2.1);
            float  turb = saturate(p.age * 0.8) * turbScale;
            p.velocity.x += n1 * turb * deltaTime;
            p.velocity.z += n2 * turb * deltaTime;
        }

        // --- BUOYANCY (buoyancy): upward accel that fades over life. 0 = no lift
        // (gravity-driven debris/sparks); high = hot rising smoke/fire.
        float buoy = (1.0 - saturate(p.age / max(p.life, 0.001))) * buoyancy;
        p.velocity.y += buoy * deltaTime;

        p.position += p.velocity * deltaTime;
        p.age      += deltaTime;

        // --- ground collision (optional bounce) ---
        if (p.position.y < groundY)
        {
            p.position.y = groundY;
            if (p.velocity.y < 0.0) { p.velocity.y = -p.velocity.y * bounce; }
            p.velocity.xz *= 0.90;           // tangential scrub on contact
        }

        Particles[i] = p;
    }

    // life fraction 0..1; dead slots collapse to zero-size (degenerate) verts.
    float t = (p.life > 0.0) ? saturate(p.age / p.life) : 1.0;

    // alpha profile: ramp in over the first 15%, ease out to 0 by end of life.
    // fadeIn rises 0->1 quickly, fadeOut falls 1->0 over the tail; product peaks
    // near t~0.15 then decays -> a soft puff that appears, billows, and dissipates.
    float fadeIn  = saturate(t / 0.15);
    float fadeOut = saturate((1.0 - t) / 0.85);
    float alpha   = p.color.a * fadeIn * fadeOut;

    float halfSize = (p.size0 + p.sizeRate * p.age);
    if (dead) { halfSize = 0.0; alpha = 0.0; }

    float roll = p.rot0 + p.rotRate * p.age;
    float cs = cos(roll), sn = sin(roll);

    // four corners in [-1,1], rotated by roll. (VS multiplies uv by size*w and adds
    // to the projected center, so these stay unit-ish here; magnitude via flag.)
    float2 c0 = float2(-1.0, -1.0);
    float2 c1 = float2( 1.0, -1.0);
    float2 c2 = float2( 1.0,  1.0);
    float2 c3 = float2(-1.0,  1.0);
    float2x2 rot = float2x2(cs, -sn, sn, cs);
    c0 = mul(rot, c0); c1 = mul(rot, c1); c2 = mul(rot, c2); c3 = mul(rot, c3);

    float4 col = float4(p.color.rgb, alpha);
    float  flag = -halfSize;                 // negative -> billboard; |flag| = size
    float3 P = p.position;

    // assemble two triangles: (c0,c1,c2) and (c0,c2,c3)
    WriteVertex(vbase + 0u, P, c0, col, texSlice, flag);
    WriteVertex(vbase + 1u, P, c1, col, texSlice, flag);
    WriteVertex(vbase + 2u, P, c2, col, texSlice, flag);
    WriteVertex(vbase + 3u, P, c0, col, texSlice, flag);
    WriteVertex(vbase + 4u, P, c2, col, texSlice, flag);
    WriteVertex(vbase + 5u, P, c3, col, texSlice, flag);
}
