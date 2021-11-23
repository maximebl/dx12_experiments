#include "common.hlsl"

struct vertex_out
{
    float3 pos : POSITION;
    float size : SIZE;
};

struct geo_out
{
    float4 hpos : SV_Position;
    float2 tex_coord : TEXCOORD;
};

#ifdef VERTEX_SHADER
struct vertex_in
{
    float3 position : POSITION;
    float size : SIZE;
    float3 velocity : VELOCITY;
    float age : AGE;
};

vertex_out vs_main(vertex_in vin)
{
    vertex_out vout;
    vout.pos = vin.position;
    vout.size = vin.size;
    return vout;
}
#endif

#ifdef GEOMETRY_SHADER
ConstantBuffer<pass_data> cb_pass : register(b0);
ConstantBuffer<particle_system_info> particle_system_cb : register(b0, space1);

[maxvertexcount(4)]
void gs_main(point vertex_out gs_in[1], inout TriangleStream<geo_out> stream)
{
    vertex_out current_point = gs_in[0];

    // Compute the local coordinate system of the sprite relative to world space.
    float3 up = cb_pass.view._12_22_32;
    matrix view_proj = mul(cb_pass.inv_view, cb_pass.proj);
    current_point.pos = mul(float4(current_point.pos, 1.f), particle_system_cb.world).xyz;

    // Get the vector from the billboard's position to the eye.
    float3 forward = normalize(cb_pass.eye_pos - current_point.pos);
    float3 right = cross(up, forward);

    // Compute quad in world space.
    float half_width = current_point.size;
    float half_height = half_width;

    float3 scaled_right = half_width * right;
    float3 scaled_up = half_height * up;

    float2 tex_coords[4] =
    {
        // One texture coordinate for each corner of the billboard
        float2(0.0f, 1.0f), // Bottom left
		float2(0.0f, 0.0f), // Top left
		float2(1.0f, 1.0f), // Bottom right
		float2(1.0f, 0.0f) // Top right
    };

    
    geo_out gs_out;

    // Bottom left vertex
    float4 new_bl_pos = float4((current_point.pos + scaled_right) - scaled_up, 1.f);
    gs_out.hpos = mul(new_bl_pos, view_proj);

    float4 bl_texc = float4(tex_coords[0], 0.f, 1.f);
    gs_out.tex_coord = tex_coords[0];
    stream.Append(gs_out);

    // Top left vertex
    float4 new_tl_pos = float4((current_point.pos + scaled_right) + scaled_up, 1.f);
    gs_out.hpos = mul(new_tl_pos, view_proj);

    float4 tl_texc = float4(tex_coords[1], 0.f, 1.f);
    gs_out.tex_coord = tex_coords[1];
    stream.Append(gs_out);

    // Bottom right vertex
    float4 new_br_pos = float4((current_point.pos - scaled_right) - scaled_up, 1.f);
    gs_out.hpos = mul(new_br_pos, view_proj);

    float4 br_texc = float4(tex_coords[2], 0.f, 1.f);
    gs_out.tex_coord = tex_coords[2];
    stream.Append(gs_out);

    // Top right vertex
    float4 new_tr_pos = float4((current_point.pos - scaled_right) + scaled_up, 1.f);
    gs_out.hpos = mul(new_tr_pos, view_proj);

    float4 tr_texc = float4(tex_coords[3], 0.f, 1.f);
    gs_out.tex_coord = tex_coords[3];
    stream.Append(gs_out);
}
#endif

#ifdef PIXEL_SHADER
ConstantBuffer<pass_data> cb_pass : register(b0);
ConstantBuffer<particle_system_info> particle_system_cb : register(b0, space1);
Texture2D<float4> fire_sprite : register(t0);
SamplerState linear_wrap_sampler : register(s0);
SamplerState linear_clamp_sampler : register(s2);
Texture2D<float> Depth : register(t3);

float depth_fade(float2 pixel_coord, float current_depth, float fade_distance)
{
	// Calculate the difference between the existing scene depth and
	// the current depth of the object being rendered.
    float scene_depth = Depth.Sample(linear_clamp_sampler, pixel_coord);
    float depth_delta = scene_depth - current_depth;
    return saturate(depth_delta / max(fade_distance, epsilon));
}

float4 ps_main(geo_out ps_in, float4 pixel_pos : SV_Position) : SV_Target
{
    float4 color = fire_sprite.Sample(linear_wrap_sampler, ps_in.tex_coord);

    float2 uv = pixel_pos.xy / cb_pass.sceen_size;
    float current_depth = ps_in.hpos.z;
    float faded_depth = depth_fade(uv, current_depth, particle_system_cb.depth_fade);
    color.w *= faded_depth;

    // Premultiplied alpha.
    color.rgb *= color.w;

    return color;
}
#endif
