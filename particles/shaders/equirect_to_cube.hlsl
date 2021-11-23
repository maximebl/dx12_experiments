#include "common.hlsl"

Texture2D equirect_tex : register(t0, space20);
RWTexture2DArray<float4> unfiltered_envmap : register(u0, space20);
SamplerState linear_wrap_sampler : register(s0, space20);

[numthreads(32, 32, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID)
{
    // Get normalized cube face texture coordinates.
    uint cube_tex_width;
    uint cube_tex_height;
    uint cube_tex_depth;
    unfiltered_envmap.GetDimensions(cube_tex_width, cube_tex_height, cube_tex_depth);
    float2 cube_face_uv = tid.xy / float2(cube_tex_width, cube_tex_height);

	// Convert cube map texel to a 3D sampling vector.
    float3 to_sample = inverse_sample(cube_face_uv, tid.z);

    // Convert cartesian coords to spherical.
    float2 sph_coord = cartesian_to_spherical(to_sample);

    // [0..1].
    sph_coord /= float2(twoPI, PI);

    float4 color = equirect_tex.SampleLevel(linear_wrap_sampler, sph_coord, 0.f);
    unfiltered_envmap[tid] = color;
}
