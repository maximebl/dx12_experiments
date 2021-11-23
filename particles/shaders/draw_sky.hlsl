#include "common.hlsl"

struct vertex_out
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD;
};

#ifdef VERTEX_SHADER
vertex_out vs_main(uint vertexID : SV_VertexID)
{
    vertex_out vout;
    // Generate a screen-filling triangle.
    if (vertexID == 0)
    {
        // Top right.
        vout.texcoord = float2(1.0, -1.0);
        vout.position = float4(1.0, 3.0, 0.0, 1.0);
    }
    else if (vertexID == 1)
    {
        // Bottom left.
        vout.texcoord = float2(-1.0, 1.0);
        vout.position = float4(-3.0, -1.0, 0.0, 1.0);
    }
    else
    {
        // Bottom right.
        vout.texcoord = float2(1.0, 1.0);
        vout.position = float4(1.0, -1.0, 0.0, 1.0);
    }
    return vout;
}
#endif

#ifdef PIXEL_SHADER
TextureCube environment_map : register(t0);
Texture2D<float> Depth : register(t3);
SamplerState linear_wrap_sampler : register(s0);

ConstantBuffer<pass_data> cb_pass : register(b0);

float4 ps_main(vertex_out pin) : SV_Target
{
    float2 pixel_coord = pin.texcoord.xy;
    float depth = Depth.SampleLevel(linear_wrap_sampler, pixel_coord, 0);

    if (!is_sky(depth))
    {
        discard;
    }

    float3 view_dir = normalize(position_from_depth(1.f, pixel_coord, cb_pass.screen_to_world));
    float3 env_color = environment_map.Sample(linear_wrap_sampler, view_dir).rgb;

    return float4(env_color, 1.f);
}
#endif
