/* One cell of the probe occupancy (probe_occlusion.c), from the depth views
 * of the occluders around it.
 *
 * Each occluder was drawn into the view atlas along the occupancy's three
 * axes, from both sides, 2 texels per cell: along an axis, a texel's two
 * views give the nearest surface from each side, the first and the last
 * along the ray. A cell is occupied when, for an occluder whose box holds
 * it, it meets the span between them along all three axes (a solid fill
 * that needs no closed mesh: what lies under a hull stays empty, since the
 * view from below meets the underside first), or when a view's surface lies
 * in it (thin parts seen from one side). Depth 0 is no surface. */

cbuffer ProbeOccupancyCompose : register(b0, space2)
{
    float4 origin_cell; /* the occupancy's low corner (model metres); w: cell size */
    int4   region_lo;   /* the first cell of this dispatch */
    int4   region_size; /* cells; w: the occluder count */
    float4 texels;      /* x: view texels per metre */
};

struct ViewRecord
{
    float4 lo;       /* the views' box (model metres), on the occupancy's lattice */
    float4 hi;
    uint4  tiles[3]; /* per axis: the view from its high side (xy) and from its low side (zw) */
};

Texture2D<float>               g_views         : register(t0, space0);
SamplerState                   g_views_sampler : register(s0, space0);
StructuredBuffer<ViewRecord>   g_records       : register(t1, space0);
RWTexture3D<unorm float>       g_occupancy     : register(u0, space1);

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= (uint3)region_size.xyz)) return;
    int3   cell = region_lo.xyz + (int3)id;
    float3 clo  = origin_cell.xyz + (float3)cell * origin_cell.w;
    float3 chi  = clo + origin_cell.w;
    float  tpm  = texels.x;
    bool   occupied = false;
    [loop] for (int r = 0; r < region_size.w && !occupied; ++r) {
        ViewRecord rec = g_records[r];
        if (any(chi <= rec.lo.xyz) || any(clo >= rec.hi.xyz)) continue;
        bool inside  = true;
        bool surface = false;
        [unroll] for (int a = 0; a < 3; ++a) {
            int   u     = (a + 1) % 3;
            int   v     = (a + 2) % 3;
            float range = rec.hi[a] - rec.lo[a];
            int2  t0    = int2((int)round((clo[u] - rec.lo[u]) * tpm), (int)round((rec.hi[v] - chi[v]) * tpm));
            bool  along = false;
            [unroll] for (int k = 0; k < 4; ++k) {
                int2  t  = t0 + int2(k & 1, k >> 1);
                float dh = g_views.Load(int3((int2)rec.tiles[a].xy + t, 0));
                float dl = g_views.Load(int3((int2)rec.tiles[a].zw + t, 0));
                float sh = rec.lo[a] + dh * range; /* the last surface along the axis */
                float sl = rec.hi[a] - dl * range; /* the first */
                surface = surface || (dh > 0.0f && sh >= clo[a] && sh <= chi[a]) ||
                          (dl > 0.0f && sl >= clo[a] && sl <= chi[a]);
                along = along || (dh > 0.0f && dl > 0.0f && sl <= chi[a] && sh >= clo[a]);
            }
            inside = inside && along;
        }
        occupied = inside || surface;
    }
    g_occupancy[uint3(cell)] = occupied ? 1.0f : 0.0f;
}
