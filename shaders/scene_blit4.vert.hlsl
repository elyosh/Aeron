/*
 * Free-corner quad vertex shader — single quad with arbitrary corner
 * positions and UVs.
 *
 * Briefing-room map warp blits a source RT onto a 2D-tilted polygon
 * (neither edge axis-aligned). The shared rect-blit's trap_top_dx/_w
 * mechanism (top-edge horizontal inset only) doesn't apply, so this
 * VS takes 4 separate corners in TRIANGLESTRIP zig-zag order:
 *   vid 0 = TL, vid 1 = TR, vid 2 = BL (strip order), vid 3 = BR.
 * The C-side caller reorders from public CW (TL,TR,BR,BL) to strip
 * order before pushing — see sdl_remaster.c.
 *
 * Per-corner `q[]` carries projective coefficients (Wolberg
 * "homogeneous q") so the rasterizer produces hyperbolic UV across
 * the quad — straight lines in source space stay straight even though
 * the dst quad is non-rectangular.
 */

struct Blit4Uniforms
{
    float4 c0;     /* TL  — (pos.x, pos.y, uv.u, uv.v) */
    float4 c1;     /* TR */
    float4 c2;     /* BL (strip order — corner index 3 in CW) */
    float4 c3;     /* BR (strip order — corner index 2 in CW) */
    float4 q;      /* per-corner projective w; xyzw = strip order */
    float4 tint;
    float4 bias;
};

cbuffer Blit4 : register(b0, space1)
{
    Blit4Uniforms u;
};

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float4 tint     : TEXCOORD1;
    float4 bias     : TEXCOORD2;
};

VSOut main(uint vid : SV_VertexID)
{
    float4 cv;
    float  qv;
    if      (vid == 0u) { cv = u.c0; qv = u.q.x; }
    else if (vid == 1u) { cv = u.c1; qv = u.q.y; }
    else if (vid == 2u) { cv = u.c2; qv = u.q.z; }
    else                { cv = u.c3; qv = u.q.w; }

    /* Multiply pos by q and set w = q. Metal's perspective-correct
     * varying interpolation then produces hyperbolic UV across the
     * quad — no diagonal seam between the two strip triangles. */
    VSOut o;
    o.position = float4(cv.x * qv, cv.y * qv, 0.0f, qv);
    o.uv       = float2(cv.z, cv.w);
    o.tint     = u.tint;
    o.bias     = u.bias;
    return o;
}
