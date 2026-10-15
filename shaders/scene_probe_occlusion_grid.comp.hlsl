/* One cell of the probe occlusion grid (probe_occlusion.c).
 *
 * Along 32 directions spread over the sphere, the probe radiance reaching
 * the cell's centre, read from the cube of the probe covering it at a low
 * mip, is kept where a ray of 16 steps (0.45 m to 40 m apart, growing
 * geometrically) meets no occupied cell; it only samples within the boxes
 * holding the occupied cells. The cell stores, for the six faces of an
 * axis-aligned cube, the share of their irradiance that the kept radiance
 * gives: toward +x, -x, +y, -y in the grid's left half, toward +z, -z in
 * the right. Unlike a spherical harmonic summary, a face facing away from
 * the light that still reaches the cell, such as the floor under a hull lit
 * only from near the horizon, takes none of it. A ratio, so the cube's
 * level does not matter. Cells farther than the reach from every occluder
 * skip the rays. When the build
 * follows a change, a cell none of whose rays meets what changed would
 * come out the same: it is left as it is. */

cbuffer ProbeOcclusionGrid : register(b0, space2)
{
    float4 origin_cell;    /* the box's low corner (model metres); w: occupancy cell size */
    uint4  occupancy_dims;
    uint4  grid_dims;      /* w: the grid cell size in metres, as float bits */
    int4   region;         /* the first grid cell of this dispatch */
    float4 occluders_lo;   /* the occluders' bounds (model metres); w: reach */
    float4 occluders_hi;   /* w: the probe cube mip to read */
    float4 boxes[8];       /* lo, hi pairs (model metres); lo.w: 1 when used */
    float4 changes[8];     /* what changed, as boxes; none: every cell is built */
};

Texture3D<float>         g_occupancy           : register(t0, space0);
SamplerState             g_occupancy_sampler   : register(s0, space0);
TextureCubeArray<float4> g_probe_cubes         : register(t1, space0);
SamplerState             g_probe_cubes_sampler : register(s1, space0);
StructuredBuffer<uint4>  g_probe_set           : register(t2, space0);
RWTexture3D<float4>      g_grid                : register(u0, space1);

#define DIRECTIONS 32u
#define STEPS      16u

/* The cube layer of the first probe covering model point `pm`; -1 for none
 * (the layout of scene_pbr_mesh_impl.hlsli's probe_specular). */
float probe_layer(float3 pm)
{
    uint4  dims = g_probe_set[1];
    float4 head = asfloat(g_probe_set[0]);
    int3   cell = (int3)floor((pm - head.xyz) * head.w);
    if (any(cell < 0) || any(cell >= (int3)dims.xyz)) return -1.0f;
    uint index = ((uint)cell.z * dims.y + (uint)cell.y) * dims.x + (uint)cell.x;
    uint word  = g_probe_set[2u + 5u * dims.w + (index >> 2u)][index & 3u];
    uint probe = word & 0xFFu;
    if (probe == 0u) return -1.0f;
    return (float)g_probe_set[2u + 5u * (probe - 1u) + 1u].w;
}

/* Ray `k` of the DIRECTIONS spread over the sphere. */
float3 direction(uint k)
{
    float h   = ((float)k + 0.5f) / (float)DIRECTIONS;
    float z   = 1.0f - 2.0f * h;
    float r   = sqrt(saturate(1.0f - z * z));
    float phi = 3.14159265f * (1.0f + sqrt(5.0f)) * ((float)k + 0.5f);
    return float3(r * cos(phi), r * sin(phi), z);
}

/* Whether the ray from `pm` along `d`, over the length blocked() reads,
 * meets one of `changes`. */
bool meets_change(float3 pm, float3 d)
{
    float3 inv = 1.0f / select(abs(d) > 1.0e-6f, d, select(d >= 0.0f, 1.0e-6f, -1.0e-6f));
    [unroll] for (uint b = 0u; b < 4u; ++b) {
        if (changes[2u * b].w == 0.0f) continue;
        float3 ta = (changes[2u * b].xyz - pm) * inv;
        float3 tb = (changes[2u * b + 1u].xyz - pm) * inv;
        float  e  = max(max(min(ta.x, tb.x), min(ta.y, tb.y)), min(ta.z, tb.z));
        float  l  = min(min(max(ta.x, tb.x), max(ta.y, tb.y)), max(ta.z, tb.z));
        if (l >= max(e, 0.45f) && e <= 40.0f) return true;
    }
    return false;
}

/* Whether the ray from `pm` along `d` meets an occupied cell. */
bool blocked(float3 pm, float3 d)
{
    const float growth = pow(40.0f / 0.45f, 1.0f / (float)(STEPS - 1u));
    float3 inv   = 1.0f / select(abs(d) > 1.0e-6f, d, select(d >= 0.0f, 1.0e-6f, -1.0e-6f));
    float  enter = 1.0e30f;
    float  leave = -1.0e30f;
    [unroll] for (uint b = 0u; b < 4u; ++b) {
        if (boxes[2u * b].w == 0.0f) continue;
        float3 ta = (boxes[2u * b].xyz - pm) * inv;
        float3 tb = (boxes[2u * b + 1u].xyz - pm) * inv;
        float  e  = max(max(min(ta.x, tb.x), min(ta.y, tb.y)), min(ta.z, tb.z));
        float  l  = min(min(max(ta.x, tb.x), max(ta.y, tb.y)), max(ta.z, tb.z));
        if (l >= max(e, 0.45f) && e <= 40.0f) {
            enter = min(enter, e);
            leave = max(leave, l);
        }
    }
    float t = 0.45f;
    [loop] for (uint k = 0u; k < STEPS && t <= leave; ++k) {
        if (t >= enter) {
            float3 p = (pm + d * t - origin_cell.xyz) / origin_cell.w;
            if (any(p < 0.0f) || any(p >= (float3)occupancy_dims.xyz)) return false;
            if (g_occupancy.Load(int4((int3)p, 0)) > 0.5f) return true;
        }
        t *= growth;
    }
    return false;
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    int3 cell = region.xyz + (int3)id;
    if (any(cell >= (int3)grid_dims.xyz)) return;
    float  grid_cell = asfloat(grid_dims.w);
    float3 pm        = origin_cell.xyz + ((float3)cell + 0.5f) * grid_cell;
    float  reach     = occluders_lo.w;
    bool   near      = all(pm >= occluders_lo.xyz - reach) && all(pm <= occluders_hi.xyz + reach);
    if (near && changes[0].w != 0.0f) {
        bool changed = false;
        [loop] for (uint k = 0u; k < DIRECTIONS && !changed; ++k) {
            changed = meets_change(pm, direction(k));
        }
        if (!changed) return;
    }
    float  layer     = probe_layer(pm);

    /* Each face's irradiance, toward + and - along each axis, past the
     * occluders and without them. */
    float3 seen_pos  = float3(0.0f, 0.0f, 0.0f);
    float3 seen_neg  = float3(0.0f, 0.0f, 0.0f);
    float3 whole_pos = float3(0.0f, 0.0f, 0.0f);
    float3 whole_neg = float3(0.0f, 0.0f, 0.0f);
    [loop] for (uint k = 0u; k < DIRECTIONS; ++k) {
        float3 d        = direction(k);
        float  radiance = 1.0f;
        if (layer >= 0.0f) {
            float3 rgb = g_probe_cubes.SampleLevel(g_probe_cubes_sampler, float4(d.x, d.z, d.y, layer),
                                                   occluders_hi.w).rgb;
            radiance = max(dot(rgb, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
        }
        float3 pos = radiance * max(d, 0.0f);
        float3 neg = radiance * max(-d, 0.0f);
        whole_pos += pos;
        whole_neg += neg;
        if (!near || !blocked(pm, d)) {
            seen_pos += pos;
            seen_neg += neg;
        }
    }
    /* A face no light reaches keeps it all. */
    float  least = 1.0e-4f * dot(whole_pos + whole_neg, float3(1.0f, 1.0f, 1.0f));
    float3 pos   = select(whole_pos > least, seen_pos / max(whole_pos, 1.0e-30f), 1.0f);
    float3 neg   = select(whole_neg > least, seen_neg / max(whole_neg, 1.0e-30f), 1.0f);
    g_grid[uint3(cell)]                              = float4(pos.x, neg.x, pos.y, neg.y);
    g_grid[uint3(cell) + uint3(grid_dims.x, 0u, 0u)] = float4(pos.z, neg.z, 0.0f, 0.0f);
}
