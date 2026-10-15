/* One cell of a probe occupancy mip (probe_occlusion.c): the mean of the
 * 2 x 2 x 2 cells below it in the level above. The level is written to a
 * staging texture, then copied into the occupancy, which a compute pass may
 * not read while writing it. */

cbuffer ProbeOccupancyDownsample : register(b0, space2)
{
    int4 region_lo;   /* the first cell of this level's dispatch */
    int4 region_size; /* cells; w: the level read, one finer */
    int4 source_dims; /* that level's cells */
};

Texture3D<float>         g_source         : register(t0, space0);
SamplerState             g_source_sampler : register(s0, space0);
RWTexture3D<unorm float> g_level          : register(u0, space1);

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= (uint3)region_size.xyz)) return;
    int3  cell  = region_lo.xyz + (int3)id;
    float sum   = 0.0f;
    float count = 0.0f;
    [unroll] for (int k = 0; k < 8; ++k) {
        int3 c = cell * 2 + int3(k & 1, (k >> 1) & 1, k >> 2);
        if (all(c < source_dims.xyz)) {
            sum += g_source.Load(int4(c, region_size.w));
            count += 1.0f;
        }
    }
    g_level[uint3(cell)] = count > 0.0f ? sum / count : 0.0f;
}
