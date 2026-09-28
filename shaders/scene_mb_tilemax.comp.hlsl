/* Reduce each resolution-scaled velocity tile to its maximum-magnitude vector. This path
 * handles non-temporal high-quality blur and retained temporal velocities that
 * do not have a tile result from the fused reconstruction dispatch. */

#define MB_GROUP_SIZE 8u
#define MB_GROUP_THREADS (MB_GROUP_SIZE * MB_GROUP_SIZE)

Texture2D<float2> g_velocity   : register(t0, space0);
SamplerState      g_velocity_s : register(s0, space0);

RWTexture2D<float2> g_tile_out : register(u0, space1);

cbuffer MbTileMaxUniforms : register(b0, space2)
{
    uint2 output_size;
    uint tile_size;
    uint _pad;
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
    uint2 tile_base = group_id.xy * tile_size;
    float2 best_velocity = 0.0f.xx;
    float best_length_sq = -1.0f;
    uint best_pixel_index = 0xffffffffu;

    [loop] for (uint y = group_thread_id.y; y < tile_size; y += MB_GROUP_SIZE) {
        [loop] for (uint x = group_thread_id.x; x < tile_size; x += MB_GROUP_SIZE) {
            uint2 pixel = tile_base + uint2(x, y);
            if (all(pixel < output_size)) {
                float2 uv = (float2(pixel) + 0.5f) / float2(output_size);
                float2 velocity = g_velocity.SampleLevel(g_velocity_s, uv, 0.0f).rg;
                float2 pixel_velocity = velocity * float2(output_size);
                float length_sq = dot(pixel_velocity, pixel_velocity);
                uint pixel_index = y * tile_size + x;
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
