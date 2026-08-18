// =================================================================================
// Slag GPU physics compute shader (rigid-body integration + collision response).
// Shader Model 5.0. Drives per-object 3D rigid bodies entirely on the GPU: each
// thread owns one body, integrates linear + angular motion (semi-implicit Euler),
// resolves collision against a ground plane, a fixed set of static world spheres,
// AND against every other dynamic body (sphere-sphere), then writes the updated
// state back in place. Object transforms stay GPU-resident so a later pass can feed
// them straight to fill_triangle_gpu.
//
// Four entry points, each compiled to its own .cso and embedded in the runtime:
//   fxc /T cs_5_0 /E main             /Fo physics_integrate.cso physics.hlsl
//   fxc /T cs_5_0 /E ClearImpulses    /Fo physics_clear.cso     physics.hlsl
//   fxc /T cs_5_0 /E ResolveBodyPairs /Fo physics_resolve.cso   physics.hlsl
//   fxc /T cs_5_0 /E ApplyImpulses    /Fo physics_apply.cso     physics.hlsl
//
// --- RUNTIME BUFFER / VIEW LAYOUT (authoritative) --------------------------------
//   Bodies buffer  : ONE D3D11 buffer, StructureByteStride = 96, created with
//                    D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE and
//                    MISC_BUFFER_STRUCTURED. Two views of it:
//                      - UAV  -> bound at u0 as `Bodies`
//                      - SRV  -> bound at t0 as `BodiesRead`
//                    D3D11 forbids binding the same resource as UAV and SRV at the
//                    same time, so exactly ONE view is bound per pass (order below).
//   ImpulseAccum   : a SEPARATE buffer, StructureByteStride = 32 (two float3 rows,
//                    padded to 32), D3D11_BIND_UNORDERED_ACCESS, bound at u1.
//   PhysicsConstants cbuffer: 192 bytes (see layout at the cbuffer decl), bound b0.
//
// --- PER-FRAME CPU DISPATCH ORDER (bodyCount threads each; groups = ceil/256) -----
//   1. main             : bind u0=Bodies.            integrate + static collision
//   2. ClearImpulses    : bind u1=ImpulseAccum.       zero the scratch buffer
//   3. ResolveBodyPairs : unbind u0 (NULL), bind t0=BodiesRead, u1=ImpulseAccum.
//                         body-vs-body detect + impulse accumulation
//   4. ApplyImpulses    : unbind t0 (NULL), bind u0=Bodies, u1=ImpulseAccum.
//                         commit impulses + wake/sleep bookkeeping
//   Steps 2-4 may be looped 2-4x per frame (Jacobi iteration) for stiffer stacks;
//   each loop re-reads the committed Bodies through BodiesRead.
// =================================================================================

#define THREAD_GROUP_SIZE 256
#define MAX_COLLIDERS     8          // static world sphere colliders in the cbuffer

// -----------------------------------------------------------------------------
// Per-body state. StructureByteStride = 96 (six 16-byte rows, fully packed).
//   position     @0   world-space center of mass
//   invMass      @12  1/mass (0 = infinite mass / immovable static body)
//   velocity     @16  linear velocity
//   radius       @28  bounding-sphere radius (collision + inertia)
//   orientation  @32  unit quaternion (x,y,z,w)
//   angularVel   @48  angular velocity (world axis * rad/s)
//   restitution  @60  bounciness 0..1
//   flags        @64  uint: bit0 = active, bit1 = sleeping
//   invInertia   @80  1 / (2/5 * mass * radius^2) for a solid sphere (0 if static)
//   sleepTimer   @84  seconds under the sleep threshold (poke 0 on spawn)
//   _pad2        @88  8 bytes padding -> element is exactly 96 bytes, no slack
// CPU pokes: floats via mem.pokef32 at 0/12/16/28/32..44/48/60/80/84; flags is a
// 32-bit int at offset 64 (mem.poke32-style). Never write past offset 88.
// -----------------------------------------------------------------------------
struct RigidBody
{
    float3 position;     float  invMass;      // 0
    float3 velocity;     float  radius;       // 16
    float4 orientation;  // 32
    float3 angularVel;   float  restitution;  // 48
    uint   flags;        float3 _pad;         // 64
    float  invInertia;   float  sleepTimer;   float2 _pad2;   // 80 -> stride 96
};

// Frame / world parameters. cbuffer total = 208 bytes.
cbuffer PhysicsConstants : register(b0)
{
    float3 gravity;        float  deltaTime;    // 0   : world gravity accel / frame dt
    uint   bodyCount;      float  linearDamp;   // 16  : active body count / linear drag
    float  angularDamp;    float  groundY;      // 24  : angular drag / ground plane height
    float  groundRestitution; uint colliderCount; // 32 : ground bounce / static sphere count
    // Static world sphere colliders: xyz = center, w = radius. Immovable.
    float4 colliders[MAX_COLLIDERS];            // 48  : 8 * 16 = 128 bytes -> 176
    float  friction;          float  sleepLinThreshold;  // 176 : Coulomb mu / lin speed (m/s)
    float  sleepAngThreshold; float  sleepTimeThreshold; // 184 : ang speed (rad/s) / seconds-to-sleep
    // OPT-IN axis-aligned walls. OFF by default: a zero-filled cbuffer leaves
    // objects unbounded on x/ceiling (an open world throws things off-screen and
    // they keep going). x walls activate ONLY when minX < maxX; the ceiling ONLY
    // when maxY > 0. The ground plane (groundY) is separate and unchanged.
    float4 wallBounds;                          // 192 : x=minX, y=maxX, z=maxY, w=restitution
};                                              // 208 total

RWStructuredBuffer<RigidBody> Bodies : register(u0);

// Read-only SRV view of the SAME buffer as Bodies (see RUNTIME layout note above).
// Bound only during ResolveBodyPairs, when the UAV view (u0) is unbound.
StructuredBuffer<RigidBody> BodiesRead : register(t0);

// Per-body collision-response accumulator. Exactly one thread ever writes a given
// slot (thread i only ever touches ImpulseAccum[i]), so no atomics are needed.
// StructureByteStride = 32 (float3 lin @0, float3 ang @16, each row padded to 16).
struct ImpulseAccumEntry
{
    float3 linearImpulse;   float _pad0;    // 0
    float3 angularImpulse;  float _pad1;    // 16 -> stride 32
};
RWStructuredBuffer<ImpulseAccumEntry> ImpulseAccum : register(u1);

// --- quaternion helpers ------------------------------------------------------
float4 quat_mul(float4 a, float4 b)
{
    return float4(
        a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz),
        a.w * b.w - dot(a.xyz, b.xyz));
}

// Integrate an orientation quaternion by an angular velocity over dt.
// dq/dt = 0.5 * omega_quat * q ; first-order update, renormalized.
float4 integrate_orientation(float4 q, float3 omega, float dt)
{
    float4 wq = float4(omega * dt, 0.0);
    q = q + 0.5 * quat_mul(wq, q);
    return normalize(q);
}

// --- Pass 1: integrate + static (ground plane + world sphere) collision ----------
[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint index = dispatchThreadID.x;
    if (index >= bodyCount)
    {
        return;
    }

    RigidBody b = Bodies[index];

    // Hard positional clamp (opt-in, same gating). Placed BEFORE the sleeping/
    // inactive early-return so EVERY write-back path is bounded: a body can never
    // end a step outside the walls, no matter how fast it was thrown or whether it
    // is asleep. Reflect-and-bounce below gives the bounce; this is the backstop.
    // Disabled when unset (minX==maxX, maxY<=0), so open worlds are unaffected.
    if (wallBounds.x < wallBounds.y)
    {
        b.position.x = clamp(b.position.x, wallBounds.x + b.radius, wallBounds.y - b.radius);
    }
    if (wallBounds.z > 0.0)
    {
        b.position.y = min(b.position.y, wallBounds.z - b.radius);
    }

    // Inactive (bit0 clear) or sleeping (bit1 set) bodies are left untouched.
    bool active   = (b.flags & 0x1u) != 0u;
    bool sleeping = (b.flags & 0x2u) != 0u;
    if (!active || sleeping)
    {
        Bodies[index] = b;
        return;
    }

    // --- INTEGRATE (semi-implicit Euler) -------------------------------------
    // Only dynamic bodies (invMass > 0) feel gravity and move; invMass == 0 is a
    // static/kinematic body that still writes back unchanged.
    if (b.invMass > 0.0)
    {
        b.velocity   += gravity * deltaTime;
        b.velocity   *= saturate(1.0 - linearDamp  * deltaTime);
        b.angularVel *= saturate(1.0 - angularDamp * deltaTime);

        b.position    += b.velocity * deltaTime;
        b.orientation  = integrate_orientation(b.orientation, b.angularVel, deltaTime);

        // --- GROUND PLANE COLLISION (y = groundY) ----------------------------
        // Penetration when the sphere bottom dips below the plane. Push out and
        // reflect the normal velocity component scaled by restitution.
        float pen = groundY + b.radius - b.position.y;
        if (pen > 0.0)
        {
            b.position.y += pen;                       // positional correction
            if (b.velocity.y < 0.0)
            {
                float e = min(b.restitution, groundRestitution);
                b.velocity.y = -b.velocity.y * e;      // bounce
                b.velocity.xz *= 0.98;                 // tangential friction
            }
        }

        // --- WORLD SPHERE COLLIDERS ------------------------------------------
        // Sphere-vs-static-sphere: push the body out along the contact normal and
        // reflect the approaching velocity. colliderCount is clamped to MAX_COLLIDERS.
        uint cc = min(colliderCount, (uint)MAX_COLLIDERS);
        for (uint c = 0u; c < cc; c = c + 1u)
        {
            float3 sc = colliders[c].xyz;
            float  sr = colliders[c].w;
            float3 d  = b.position - sc;
            float  dist = length(d);
            float  minDist = b.radius + sr;
            if (dist < minDist && dist > 1e-5)
            {
                float3 n = d / dist;                   // contact normal
                b.position += n * (minDist - dist);    // separate
                float vn = dot(b.velocity, n);
                if (vn < 0.0)
                {
                    b.velocity -= (1.0 + b.restitution) * vn * n;
                }
            }
        }

        // --- OPT-IN AXIS-ALIGNED WALLS (window/box edges) --------------------
        // Same clamp-and-reflect as the ground plane, on x-min/x-max/ceiling.
        // Disabled unless the caller sets wallBounds: x walls need minX < maxX,
        // the ceiling needs maxY > 0. An unset (zero) wallBounds is a no-op, so an
        // open-world body thrown past the screen keeps going. e = min(body, wall).
        if (wallBounds.x < wallBounds.y)
        {
            float e = min(b.restitution, wallBounds.w);
            // left wall (x = minX): body center clamped to minX + radius
            float penL = (wallBounds.x + b.radius) - b.position.x;
            if (penL > 0.0)
            {
                b.position.x += penL;
                if (b.velocity.x < 0.0)
                {
                    b.velocity.x = -b.velocity.x * e;
                    b.velocity.yz *= 0.98;
                }
            }
            // right wall (x = maxX): body center clamped to maxX - radius
            float penR = b.position.x - (wallBounds.y - b.radius);
            if (penR > 0.0)
            {
                b.position.x -= penR;
                if (b.velocity.x > 0.0)
                {
                    b.velocity.x = -b.velocity.x * e;
                    b.velocity.yz *= 0.98;
                }
            }
        }
        if (wallBounds.z > 0.0)
        {
            float e = min(b.restitution, wallBounds.w);
            // ceiling (y = maxY): body center clamped to maxY - radius
            float penC = b.position.y - (wallBounds.z - b.radius);
            if (penC > 0.0)
            {
                b.position.y -= penC;
                if (b.velocity.y > 0.0)
                {
                    b.velocity.y = -b.velocity.y * e;
                    b.velocity.xz *= 0.98;
                }
            }
        }
    }

    Bodies[index] = b;
}

// --- Pass 2: zero the scratch accumulator before each frame's pair pass ----------
[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void ClearImpulses(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint index = dispatchThreadID.x;
    if (index >= bodyCount) return;

    ImpulseAccum[index].linearImpulse  = float3(0.0, 0.0, 0.0);
    ImpulseAccum[index].angularImpulse = float3(0.0, 0.0, 0.0);
}

// Cached per-body fields used while tiling the all-pairs scan through groupshared
// memory. THREAD_GROUP_SIZE * sizeof(CachedBody) = 256 * 64B = 16KB, under the
// 32KB groupshared limit.
struct CachedBody
{
    float3 position;
    float  radius;
    float3 velocity;
    float  invMass;
    float3 angularVel;
    float  invInertia;
    float  restitution;
    uint   flags;
    float2 _pad;
};
groupshared CachedBody sTile[THREAD_GROUP_SIZE];

// --- Pass 3: dynamic body-vs-body broad+narrow phase and impulse resolution -----
// Each thread owns body i and scans every other body j in THREAD_GROUP_SIZE-wide
// tiles cached through groupshared memory (classic tiled N-body pattern: O(N^2)
// total work but coherent, bandwidth-friendly reads instead of N^2 global loads).
//
// Thread i writes ONLY ImpulseAccum[i] (never j's slot), so no atomics are needed.
// The impulse each thread computes is the reaction ON ITS OWN BODY: when thread j
// runs the same code, its contact normal for the (i,j) pair points the opposite way
// (nj = -ni) and it uses its own invMass/invInertia, producing the equal-and-
// opposite reaction on body j. Both halves are therefore computed independently and
// consistently from the identical pre-pass snapshot in BodiesRead. All response
// terms below (normal impulse, friction, Baumgarte bias) are scaled by THIS body's
// invMass/invInertia, so mass ratio is respected on both sides.
[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void ResolveBodyPairs(uint3 dispatchThreadID : SV_DispatchThreadID,
                       uint3 groupThreadID    : SV_GroupThreadID)
{
    uint i = dispatchThreadID.x;
    bool iValid = i < bodyCount;

    RigidBody bi = (RigidBody)0;
    bool iActive = false;
    if (iValid)
    {
        bi = BodiesRead[i];
        iActive = (bi.flags & 0x1u) != 0u;   // sleeping bodies still participate (a
                                              // contact can wake them); only fully
                                              // inactive bodies are skipped
    }

    float3 linImp = float3(0.0, 0.0, 0.0);
    float3 angImp = float3(0.0, 0.0, 0.0);

    uint numTiles = (bodyCount + THREAD_GROUP_SIZE - 1) / THREAD_GROUP_SIZE;
    for (uint t = 0u; t < numTiles; t = t + 1u)
    {
        uint j = t * THREAD_GROUP_SIZE + groupThreadID.x;

        CachedBody cb = (CachedBody)0;
        if (j < bodyCount)
        {
            RigidBody bj = BodiesRead[j];
            cb.position    = bj.position;
            cb.radius      = bj.radius;
            cb.velocity    = bj.velocity;
            cb.invMass     = bj.invMass;
            cb.angularVel  = bj.angularVel;
            cb.invInertia  = bj.invInertia;
            cb.restitution = bj.restitution;
            cb.flags       = bj.flags;
        }
        sTile[groupThreadID.x] = cb;
        GroupMemoryBarrierWithGroupSync();

        if (iActive)
        {
            uint tileBase  = t * THREAD_GROUP_SIZE;
            uint tileCount = min((uint)THREAD_GROUP_SIZE, bodyCount - tileBase);
            for (uint k = 0u; k < tileCount; k = k + 1u)
            {
                uint j2 = tileBase + k;
                if (j2 == i) continue;

                CachedBody oj = sTile[k];
                if ((oj.flags & 0x1u) == 0u) continue;         // other body inactive

                float3 d       = bi.position - oj.position;
                float  dist    = length(d);
                float  minDist = bi.radius + oj.radius;
                if (dist >= minDist || dist <= 1e-5) continue; // no contact

                float3 n  = d / dist;                          // points from j toward i
                float3 ri = -n * bi.radius;                    // contact offset from i's COM
                float3 rj =  n * oj.radius;                    // contact offset from j's COM

                float3 velAtI = bi.velocity + cross(bi.angularVel, ri);
                float3 velAtJ = oj.velocity + cross(oj.angularVel, rj);
                float3 relVel = velAtI - velAtJ;
                float  vn     = dot(relVel, n);
                if (vn >= 0.0) continue;                       // separating already

                float angTermI = dot(cross(bi.invInertia * cross(ri, n), ri), n);
                float angTermJ = dot(cross(oj.invInertia * cross(rj, n), rj), n);
                float denom    = bi.invMass + oj.invMass + angTermI + angTermJ;
                if (denom <= 1e-8) continue;                   // both effectively static

                float  e    = min(bi.restitution, oj.restitution);
                float  jImp = -(1.0 + e) * vn / denom;         // scalar normal impulse
                float3 impulse = jImp * n;

                // Reaction on THIS body only (weighted by i's inverse mass/inertia).
                linImp += impulse * bi.invMass;
                angImp += bi.invInertia * cross(ri, impulse);

                // Coulomb friction: tangential impulse clamped by mu * normal impulse.
                float3 vt    = relVel - vn * n;
                float  vtLen = length(vt);
                if (vtLen > 1e-5)
                {
                    float3 tangent   = vt / vtLen;
                    float angTermTI  = dot(cross(bi.invInertia * cross(ri, tangent), ri), tangent);
                    float angTermTJ  = dot(cross(oj.invInertia * cross(rj, tangent), rj), tangent);
                    float denomT     = bi.invMass + oj.invMass + angTermTI + angTermTJ;
                    if (denomT > 1e-8)
                    {
                        float maxFriction = friction * abs(jImp);
                        float jt = clamp(-dot(relVel, tangent) / denomT, -maxFriction, maxFriction);
                        float3 fImpulse = jt * tangent;
                        linImp += fImpulse * bi.invMass;
                        angImp += bi.invInertia * cross(ri, fImpulse);
                    }
                }

                // Baumgarte positional bias: a small velocity nudge proportional to
                // penetration/dt, mass-weighted like the impulse so a light body is
                // pushed out more than a heavy one and a static body (invMass 0) not
                // at all -- the same split thread j applies with the opposite normal.
                float pen = minDist - dist;
                linImp += n * (0.2 * pen / max(deltaTime, 1e-5)) * bi.invMass * denom;
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (iValid)
    {
        ImpulseAccum[i].linearImpulse  = linImp;
        ImpulseAccum[i].angularImpulse = angImp;
    }
}

// --- Pass 4: commit accumulated impulses, handle wake/sleep transitions ---------
[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void ApplyImpulses(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint index = dispatchThreadID.x;
    if (index >= bodyCount) return;

    RigidBody b = Bodies[index];
    bool active = (b.flags & 0x1u) != 0u;
    if (!active)
    {
        Bodies[index] = b;
        return;
    }

    ImpulseAccumEntry acc = ImpulseAccum[index];
    float impMag = length(acc.linearImpulse) + length(acc.angularImpulse);

    bool wasSleeping = (b.flags & 0x2u) != 0u;
    if (wasSleeping)
    {
        if (impMag > 1e-4)
        {
            b.flags      = b.flags & ~0x2u;   // woken by a collision impulse
            b.sleepTimer = 0.0;
        }
        else
        {
            Bodies[index] = b;                // stays asleep, nothing to do
            return;
        }
    }

    if (b.invMass > 0.0)
    {
        b.velocity   += acc.linearImpulse;
        b.angularVel += acc.angularImpulse;

        // Independent linear + angular rest tests (each named threshold checked
        // against its own quantity in its own units).
        float linSpeed = length(b.velocity);
        float angSpeed = length(b.angularVel);
        if (linSpeed < sleepLinThreshold && angSpeed < sleepAngThreshold)
        {
            b.sleepTimer += deltaTime;
            if (b.sleepTimer > sleepTimeThreshold)
            {
                b.flags      = b.flags | 0x2u;
                b.velocity   = float3(0.0, 0.0, 0.0);
                b.angularVel = float3(0.0, 0.0, 0.0);
            }
        }
        else
        {
            b.sleepTimer = 0.0;
        }
    }

    Bodies[index] = b;
}

// =================================================================================
// Out of scope for this shader (flag for a future revision if needed):
//   - CCD / swept collision for fast-moving small bodies (tunneling still possible)
//   - Warm-started / multi-iteration solver beyond the CPU-driven Jacobi loop noted
//     at the top (fine for light contact graphs; deep stacks may still jitter)
//   - Non-sphere collision geometry (static world is ground plane + spheres only;
//     dynamic bodies collide as bounding spheres)
// =================================================================================
