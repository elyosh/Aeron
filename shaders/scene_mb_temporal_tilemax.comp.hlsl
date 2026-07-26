/* Reconstruct FSR motion at display resolution and reduce each 32x32 output
 * tile to its maximum-magnitude velocity in the same dispatch. The display
 * velocity remains available to the final motion-blur reconstruction, while
 * the group reduction replaces the separate horizontal and vertical TileMax
 * fragment passes. */

#define MB_TILE_SIZE 32u
#define MB_GROUP_SIZE 8u
#define MB_GROUP_THREADS (MB_GROUP_SIZE * MB_GROUP_SIZE)

Texture2D<float>  g_depth    : register(t0, space0);
SamplerState      g_depth_s  : register(s0, space0);
Texture2D<float2> g_motion   : register(t1, space0);
SamplerState      g_motion_s : register(s1, space0);

RWTexture2D<float2> g_velocity_out : register(u0, space1);
RWTexture2D<float2> g_tile_out     : register(u1, space1);

cbuffer MbTemporalTileMaxUniforms : register(b0, space2)
{
    float2 source_texel;
    uint2  output_size;
    uint   native_resolution;
    uint3  _pad;
};

groupshared float2 s_velocity[MB_GROUP_THREADS];
groupshared float  s_length_sq[MB_GROUP_THREADS];
groupshared uint   s_pixel_index[MB_GROUP_THREADS];

float2 reconstruct_velocity(uint2 pixel)
{
    float2 uv = (float2(pixel) + 0.5f) / float2(output_size);
    if (native_resolution != 0u)
        return -g_motion.SampleLevel(g_motion_s, uv, 0.0f).rg;

    float2 source_position = uv / source_texel - 0.5f;
    float2 source_base = floor(source_position);
    float2 source_fraction = frac(source_position);
    float2 best_uv = (source_base + 0.5f) * source_texel;
    float best_depth = -1.0f;

    /* Preserve the fragment implementation's row-major footprint and tie
     * behavior so disocclusion boundaries select the same source motion. */
    float4 source_depths = g_depth.GatherRed(g_depth_s, uv).wzxy;
    [unroll] for (int y = 0; y < 2; y++) {
        [unroll] for (int x = 0; x < 2; x++) {
            float2 axis_weight = lerp(1.0f.xx - source_fraction, source_fraction, float2(x, y));
            if (axis_weight.x * axis_weight.y <= 1e-5f)
                continue;
            float2 sample_uv = (source_base + float2(x, y) + 0.5f) * source_texel;
            float sample_depth = source_depths[y * 2 + x];
            if (sample_depth > best_depth) {
                best_depth = sample_depth;
                best_uv = sample_uv;
            }
        }
    }
    return -g_motion.SampleLevel(g_motion_s, best_uv, 0.0f).rg;
}

float2 quantize_velocity(float2 velocity)
{
    /* TileMax previously read R16G16_FLOAT after the reconstruction pass.
     * Round explicitly before reducing so the fused path makes the same
     * maximum-magnitude decision. */
    return f16tof32(f32tof16(velocity));
}

bool velocity_is_better(float length_sq, uint pixel_index,
                        float best_length_sq, uint best_pixel_index)
{
    return length_sq > best_length_sq ||
           (length_sq == best_length_sq && pixel_index < best_pixel_index);
}

[numthreads(MB_GROUP_SIZE, MB_GROUP_SIZE, 1)]
void main(uint3 group_id : SV_GroupID,
          uint3 group_thread_id : SV_GroupThreadID,
          uint group_index : SV_GroupIndex)
{
    uint2 tile_base = group_id.xy * MB_TILE_SIZE;
    float2 best_velocity = 0.0f.xx;
    float best_length_sq = -1.0f;
    uint best_pixel_index = 0xffffffffu;

    /* Each of the 64 threads handles a 4x4 stratum of the 32x32 tile. */
    [unroll] for (uint yi = 0u; yi < 4u; yi++) {
        uint y = group_thread_id.y + yi * MB_GROUP_SIZE;
        [unroll] for (uint xi = 0u; xi < 4u; xi++) {
            uint x = group_thread_id.x + xi * MB_GROUP_SIZE;
            uint2 pixel = tile_base + uint2(x, y);
            if (all(pixel < output_size)) {
                float2 velocity = reconstruct_velocity(pixel);
                g_velocity_out[pixel] = velocity;

                float2 reduced_velocity = quantize_velocity(velocity);
                float length_sq = dot(reduced_velocity, reduced_velocity);
                uint pixel_index = y * MB_TILE_SIZE + x;
                if (velocity_is_better(length_sq, pixel_index, best_length_sq, best_pixel_index)) {
                    best_velocity = reduced_velocity;
                    best_length_sq = length_sq;
                    best_pixel_index = pixel_index;
                }
            }
        }
    }

    s_velocity[group_index] = best_velocity;
    s_length_sq[group_index] = best_length_sq;
    s_pixel_index[group_index] = best_pixel_index;
    GroupMemoryBarrierWithGroupSync();

    [unroll(6)] for (uint stride = MB_GROUP_THREADS / 2u; stride > 0u; stride >>= 1u) {
        if (group_index < stride) {
            uint other = group_index + stride;
            if (velocity_is_better(s_length_sq[other], s_pixel_index[other],
                                   s_length_sq[group_index], s_pixel_index[group_index])) {
                s_velocity[group_index] = s_velocity[other];
                s_length_sq[group_index] = s_length_sq[other];
                s_pixel_index[group_index] = s_pixel_index[other];
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (group_index == 0u)
        g_tile_out[group_id.xy] = s_velocity[0];
}
