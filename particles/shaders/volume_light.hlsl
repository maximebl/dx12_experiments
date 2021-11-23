#include "common.hlsl"

// Most of this code comes from Eric Lengyel's book "Foundations of Game Engine Development Volume 2: Rendering".
// Section 10.4.1.

struct vertex_in
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float3 bitangent : BITANGENT;
    float2 tex_coord : TEXCOORD;
};

struct vertex_out
{
    float4 posh : SV_Position;
    float3 posw : TEXCOORD1;
};
#ifdef VERTEX_SHADER
struct volume_light_data_vs
{
    matrix world;
    matrix mvp;
};
ConstantBuffer<volume_light_data_vs> object_cb_vs : register(b1);
ConstantBuffer<pass_data> cb_pass : register(b0);

vertex_out vs_main(vertex_in vin)
{
    vertex_out vout;

    // Pixel world space position.
    vout.posw = mul(float4(vin.position, 1.f), object_cb_vs.world).xyz;

	// Transform to homogeneous clip space.
    vout.posh = mul(float4(vin.position, 1.f), object_cb_vs.mvp);

    return vout;
}
#endif

#ifdef PIXEL_SHADER
ConstantBuffer<pass_data> cb_pass : register(b0);
struct volume_light_data_ps
{
    matrix world_to_object;
    float4 color;
    float3 object_space_cam_pos;
    float radius;
    float3 object_space_cam_forward;
    float _pad1;
};
ConstantBuffer<volume_light_data_ps> volume_light : register(b2);
Texture2D<float> Depth : register(t3);
SamplerState linear_wrap_sampler : register(s0);

float4 ps_main(vertex_out pin) : SV_Target
{
    float2 uv = pin.posh.xy / cb_pass.sceen_size;
    float depth = Depth.SampleLevel(linear_wrap_sampler, uv, 0.f);
    depth = linearize_depth2(1.f - depth, cb_pass.near_plane, cb_pass.far_plane);

    // Volumetric halo variables.
    float halo = 0.f;
    float halo_radius = volume_light.radius;
    float halo_radius2 = halo_radius * halo_radius;
    float rcp_halo_radius2 = 1.f / halo_radius2;
    float rcp_3halo_radius2 = 1.f / (3.f * halo_radius2);
    float density_integral_normalizer = 3.f / (4.f * halo_radius);

    // Render volumetric halo.
    float4 world_space_pixel = float4(pin.posw, 1.f);
    float3 pobject = float3(mul(world_space_pixel, volume_light.world_to_object).xyz);
    float3 cam_pos = volume_light.object_space_cam_pos;

    // Find the limits of integration t1 and t2.
    float3 vdir = cam_pos - pobject;
    float v2 = dot(vdir, vdir);
    float p2 = dot(pobject, pobject);
    float pv = -dot(pobject, vdir);
    float m = sqrt(max(pv * pv - v2 * (p2 - halo_radius2), 0.f));

    float3 cam_view = volume_light.object_space_cam_forward;
    float t0 = 1.f + depth / dot(cam_view, vdir);

    float t1 = clamp((pv - m) / v2, t0, 1.f);
    float t2 = clamp((pv + m) / v2, t0, 1.f);
    float u1 = t1 * t1;
    float u2 = t2 * t2;

    // Integrate and normalize.
    float B = ((1.f - p2 * rcp_halo_radius2) * (t2 - t1) + pv * rcp_halo_radius2 * (u2 - u1) - v2 * rcp_3halo_radius2 * (t2 * u2 - t1 * u1)) * density_integral_normalizer;
    halo = (B * B * v2);

    return float4(halo * volume_light.color);
}
#endif
