RWTexture2D<float4> output_texture : register(u0, space1);

cbuffer FillParams : register(b0, space2)
{
    float4 fill_color;
    uint2 output_size;
    uint2 _padding;
};

[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    if (all(dispatch_thread_id.xy < output_size))
    {
        output_texture[dispatch_thread_id.xy] = fill_color;
    }
}
