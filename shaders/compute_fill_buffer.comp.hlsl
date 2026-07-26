RWStructuredBuffer<uint> output_buffer : register(u0, space1);

cbuffer FillBufferParams : register(b0, space2)
{
    uint fill_value;
    uint element_count;
    uint2 _padding;
};

[numthreads(64, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    if (dispatch_thread_id.x < element_count)
    {
        output_buffer[dispatch_thread_id.x] = fill_value;
    }
}
