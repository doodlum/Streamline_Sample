// Ground-truth capture: averages many jittered, non-antialiased HDR frames of the same instant.
// Alpha counts the accumulated samples.

Texture2D<float4>   t_Input  : register(t0);
RWTexture2D<float4> u_Output : register(u0);

[numthreads(8, 8, 1)]
void AccumulateFirst(uint2 id : SV_DispatchThreadID)
{
    uint width, height;
    u_Output.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
        return;
    u_Output[id] = float4(t_Input[id].rgb, 1.0);
}

[numthreads(8, 8, 1)]
void Accumulate(uint2 id : SV_DispatchThreadID)
{
    uint width, height;
    u_Output.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
        return;
    u_Output[id] += float4(t_Input[id].rgb, 1.0);
}

[numthreads(8, 8, 1)]
void Resolve(uint2 id : SV_DispatchThreadID)
{
    uint width, height;
    u_Output.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
        return;
    float4 sum   = t_Input[id];
    u_Output[id] = float4(sum.rgb / max(sum.a, 1.0), 1.0);
}
