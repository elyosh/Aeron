/* Reduce each 32x32 velocity tile to its maximum-magnitude vector. This path
 * handles non-temporal high-quality blur and retained temporal velocities that
 * do not have a tile result from the fused reconstruction dispatch. */

#define MB_TILE_SIZE 32u
#define MB_GROUP_SIZE 8u
#define MB_GROUP_THREADS (MB_GROUP_SIZE * MB_GROUP_SIZE)

Texture2D<float2> g_velocity   : register(t0, space0);
SamplerState      g_velocity_s : register(s0, space0);

RWTexture2D<float2> g_tile_out : register(u0, space1);

cbuffer MbTileMaxUniforms : register(b0, space2)
{
    uint2 output_size;
    uint2 _pad;
};

groupshared float2 s_velocity[MB_GROUP_THREADS];
groupshared float  s_length_sq[MB_GROUP_THREADS];
groupshared uint   s_pixel_index[MB_GROUP_THREADS];

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

    [unroll] for (uint yi = 0u; yi < 4u; yi++) {
        uint y = group_thread_id.y + yi * MB_GROUP_SIZE;
        [unroll] for (uint xi = 0u; xi < 4u; xi++) {
            uint x = group_thread_id.x + xi * MB_GROUP_SIZE;
            uint2 pixel = tile_base + uint2(x, y);
            if (all(pixel < output_size)) {
                float2 uv = (float2(pixel) + 0.5f) / float2(output_size);
                float2 velocity = g_velocity.SampleLevel(g_velocity_s, uv, 0.0f).rg;
                float length_sq = dot(velocity, velocity);
                uint pixel_index = y * MB_TILE_SIZE + x;
                if (velocity_is_better(length_sq, pixel_index, best_length_sq, best_pixel_index)) {
                    best_velocity = velocity;
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
