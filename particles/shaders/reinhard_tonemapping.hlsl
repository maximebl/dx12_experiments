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
    if (vertexID == 0)
    {
        vout.texcoord = float2(1.0, -1.0);
        vout.position = float4(1.0, 3.0, 0.0, 1.0);
    }
    else if (vertexID == 1)
    {
        vout.texcoord = float2(-1.0, 1.0);
        vout.position = float4(-3.0, -1.0, 0.0, 1.0);
    }
    else
    {
        vout.texcoord = float2(1.0, 1.0);
        vout.position = float4(1.0, -1.0, 0.0, 1.0);
    }
    return vout;
}
#endif

#ifdef PIXEL_SHADER
ConstantBuffer<pass_data> cb_pass : register(b0);
Texture2D frame_buffer : register(t0);
SamplerState linear_clamp_sampler : register(s2);

float4 ps_main(vertex_out pin) : SV_Target
{
    float3 color = frame_buffer.Sample(linear_clamp_sampler, pin.texcoord).rgb;
    if (!cb_pass.do_tonemapping)
    {
        return float4(linear_to_gamma(color), 1.0);
    }

    // Apply exposure.
    color *= cb_pass.exposure;

	// Reinhard tonemapping operator.
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    float mapped_luminance = (luminance * (1.0 + luminance / (pure_white * pure_white))) / (1.0 + luminance);

	// Scale color by ratio of average luminances.
    float3 mapped_color = (mapped_luminance / luminance) * color;
    
	// Gamma correction.
    return float4(linear_to_gamma(mapped_color), 1.0);
}
#endif
