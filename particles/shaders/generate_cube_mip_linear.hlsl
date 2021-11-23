Texture2DArray cube_tex : register(t2, space20);
RWTexture2DArray<float4> cube_tex_mip : register(u2, space20);

[numthreads(8, 8, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID)
{
    int4 sample_location = int4(tid.x * 2, tid.y * 2, tid.z, 0); // Skip to next quad in the cube map face. 

    // Load 4 samples and apply linear filtering.
    float4 gather_value = cube_tex.Load(sample_location, uint2(0, 0)) + // Top left.
                          cube_tex.Load(sample_location, uint2(1, 0)) + // Top right.
                          cube_tex.Load(sample_location, uint2(0, 1)) + // Bottom left.
                          cube_tex.Load(sample_location, uint2(1, 1));  // Bottom right.
    cube_tex_mip[tid] = gather_value * 0.25f;
}
