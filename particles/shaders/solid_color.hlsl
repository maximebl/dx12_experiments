#include "common.hlsl"

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
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 tex_coord : TEXCOORD;
    float3x3 tangent_basis : TBASIS;
};

#ifdef VERTEX_SHADER
ConstantBuffer<pass_data> cb_pass : register(b0);
ConstantBuffer<object_data_vs> object_cb : register(b1);

vertex_out vs_main(vertex_in vin)
{
    vertex_out vout;

	// Transform to homogeneous clip space.
    float4x4 view_proj = mul(cb_pass.inv_view, cb_pass.proj);
    float4x4 mvp = mul(object_cb.world, view_proj);
    vout.posh = mul(float4(vin.position, 1.f), mvp);
    vout.pos = mul(float4(vin.position, 1.f), mvp).xyz;
    vout.normal = mul(float4(vin.normal, 0.f), object_cb.world).xyz;

    // Tangent basis for normal mapping.
    float3x3 TBN = float3x3(vin.tangent, vin.bitangent, vin.normal);
    vout.tangent_basis = mul((float3x3) object_cb.world, transpose(TBN));

    // Texture coordinates.
    vout.tex_coord = float2(vin.tex_coord.x, 1.f - vin.tex_coord.y);

    return vout;
}
#endif

#ifdef PIXEL_SHADER

struct object_data_ps
{
    float3 color;
    uint object_id;
};
ConstantBuffer<object_data_ps> object_cb_ps : register(b1);
struct pixel_out
{
    float4 color : SV_Target;
};

pixel_out ps_main(vertex_out pin)
{
    pixel_out pout;
    pout.color = float4(object_cb_ps.color, 1.f);
    return pout;
}
#endif
