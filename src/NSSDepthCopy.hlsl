// Copies scene depth (any depth-stencil format) into an R32F texture for Arm NSS,
// which reads depth through a plain colour view.

Texture2D<float>   t_Depth  : register(t0);
RWTexture2D<float> u_Output : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    uint width, height;
    u_Output.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
        return;
    u_Output[id] = t_Depth[id];
}
