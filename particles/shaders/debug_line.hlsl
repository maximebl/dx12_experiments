#include "common.hlsl"

struct vertex_in
{
    float3 position : POSITION;
    float4 color : COLOR;
};

struct vertex_out
{
    float4 posh : SV_Position;
    float4 color : COLOR;
};

#ifdef VERTEX_SHADER
ConstantBuffer<pass_data> cb_pass : register(b0);
ConstantBuffer<object_data_vs> object_cb : register(b1);

vertex_out vs_main(vertex_in vin)
{
    vertex_out vout;

	// Transform to homogeneous clip space.
    float4x4 view_proj = mul(cb_pass.inv_view, cb_pass.proj);
    vout.posh = mul(float4(vin.position, 1.f), view_proj);
    vout.color = vin.color;

    return vout;
}
#endif

#ifdef PIXEL_SHADER
struct pixel_out
{
    float4 color : SV_Target;
};

pixel_out ps_main(vertex_out pin)
{
    pixel_out pout;
    pout.color = pin.color;
    return pout;
}
#endif
