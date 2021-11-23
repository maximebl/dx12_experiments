#include "common.hlsl"

struct vertex_in
{
    float3 position : POSITION;
    float4 color : COLOR;
};

struct vertex_out
{
    float4 hpos : SV_Position;
    float4 color : COLOR;
};

#ifdef VERTEX_SHADER
ConstantBuffer<particle_system_info> particle_system_cb : register(b0, space1);
ConstantBuffer<pass_data> cb_pass : register(b0);

vertex_out vs_main(vertex_in vin)
{
    vertex_out vout;
    float4x4 view_proj = mul(cb_pass.inv_view, cb_pass.proj);
    float4x4 mvp = mul(particle_system_cb.world, view_proj);

    vout.hpos = mul(float4(vin.position, 1.f), mvp);
    vout.color = float4(1.f, 1.f, 1.f, 1.f);
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
    pixel_out ps_out;
    ps_out.color = pin.color;
    return ps_out;
}
#endif
