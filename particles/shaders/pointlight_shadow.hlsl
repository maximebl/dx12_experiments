#include "common.hlsl"

StructuredBuffer<attractor_point_light> sb_point_light : register(t0, space1);

struct shadow_caster_info
{
    uint id;
    uint light_id;
};
ConstantBuffer<shadow_caster_info> c_shadow_caster_info : register(b0, space2);

struct vertex_out
{
    float4 wpos : SV_Position;
};

struct geo_out
{
    float4 hpos : SV_Position;
    float4 wpos : POSITION;
    uint rt_slice_index : SV_RenderTargetArrayIndex;
};

#ifdef VERTEX_SHADER
ConstantBuffer<object_data_vs> cb_shadow_caster_transforms[num_shadow_casters] : register(b1, space2);

vertex_out vs_main(float3 pos : POSITION)
{
    vertex_out vout;
    vout.wpos = mul(float4(pos, 1.f), cb_shadow_caster_transforms[c_shadow_caster_info.id].world);
    return vout;
}
#endif

#ifdef GEOMETRY_SHADER
[maxvertexcount(18)]
void gs_main(triangle vertex_out gs_in[3], inout TriangleStream<geo_out> gs_out)
{
    for (int face = 0; face < 6; ++face)
    {
        geo_out output;
        output.rt_slice_index = face + (sb_point_light[c_shadow_caster_info.light_id].light.id * 6);
        float4x4 light_view_proj = sb_point_light[c_shadow_caster_info.light_id].view_proj[face];
        for (int vertex = 0; vertex < 3; vertex++)
        {
            output.wpos = gs_in[vertex].wpos;
            output.hpos = mul(gs_in[vertex].wpos, light_view_proj);
            gs_out.Append(output);
        }
        gs_out.RestartStrip();
    }
}
#endif

#ifdef PIXEL_SHADER
ConstantBuffer<pass_data> cb_pass : register(b0);
float ps_main(geo_out pin) : SV_Depth
{
    float light_to_pixel_dist = length(pin.wpos.xyz - sb_point_light[c_shadow_caster_info.light_id].light.position_ws);
    light_to_pixel_dist /= cb_pass.far_plane;
    return light_to_pixel_dist;
}
#endif