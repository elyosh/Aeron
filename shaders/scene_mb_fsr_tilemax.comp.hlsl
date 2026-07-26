/* Reduce FSR's retained render-resolution motion directly into one maximum
 * vector per 32x32 output tile. Output-to-render mapping is monotonic, so each
 * distinct source texel only needs to be visited once within a tile. */

#define MB_TILE_SIZE 32u
#define MB_GROUP_SIZE 8u
#define MB_GROUP_THREADS (MB_GROUP_SIZE * MB_GROUP_SIZE)

Texture2D<float2> g_motion : register(t0, space0);
RWTexture2D<float2> g_tile_out : register(u0, space1);

cbuffer MbFsrTileMaxUniforms : register(b0, space2)
{
    uint2 render_size;
    uint2 output_size;
};

groupshared float2 s_velocity[MB_GROUP_THREADS];
groupshared float  s_length_sq[MB_GROUP_THREADS];
groupshared uint   s_source_index[MB_GROUP_THREADS];

bool velocity_is_better(float length_sq, uint source_index,
                        float best_length_sq, uint best_source_index)
{
    return length_sq > best_length_sq ||
           (length_sq == best_length_sq && source_index < best_source_index);
}

uint2 output_to_render(uint2 output_pixel)
{
    float2 uv = (float2(output_pixel) + 0.5f) / float2(output_size);
    return min(uint2(uv * float2(render_size)), render_size - 1u);
}

[numthreads(MB_GROUP_SIZE, MB_GROUP_SIZE, 1)]
void main(uint3 group_id : SV_GroupID, uint group_index : SV_GroupIndex)
{
    uint2 output_first = group_id.xy * MB_TILE_SIZE;
    uint2 output_last = min(output_first + MB_TILE_SIZE, output_size) - 1u;
    uint2 source_first = output_to_render(output_first);
    uint2 source_last = output_to_render(output_last);
    uint source_width = source_last.x - source_first.x + 1u;
    uint source_count = source_width * (source_last.y - source_first.y + 1u);

    float2 best_velocity = 0.0f.xx;
    float best_length_sq = -1.0f;
    uint best_source_index = 0xffffffffu;

    for (uint index = group_index; index < source_count; index += MB_GROUP_THREADS) {
        uint2 source = source_first + uint2(index % source_width, index / source_width);
        float2 velocity = -g_motion.Load(int3(source, 0));
        float length_sq = dot(velocity, velocity);
        uint source_index = source.y * render_size.x + source.x;
        if (velocity_is_better(length_sq, source_index, best_length_sq, best_source_index)) {
            best_velocity = velocity;
            best_length_sq = length_sq;
            best_source_index = source_index;
        }
    }

    s_velocity[group_index] = best_velocity;
    s_length_sq[group_index] = best_length_sq;
    s_source_index[group_index] = best_source_index;
    GroupMemoryBarrierWithGroupSync();

    [unroll(6)] for (uint stride = MB_GROUP_THREADS / 2u; stride > 0u; stride >>= 1u) {
        if (group_index < stride) {
            uint other = group_index + stride;
            if (velocity_is_better(s_length_sq[other], s_source_index[other],
                                   s_length_sq[group_index], s_source_index[group_index])) {
                s_velocity[group_index] = s_velocity[other];
                s_length_sq[group_index] = s_length_sq[other];
                s_source_index[group_index] = s_source_index[other];
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (group_index == 0u)
        g_tile_out[group_id.xy] = s_velocity[0];
}
