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
ConstantBuffer<object_data_vs> object_cb_vs : register(b1);

vertex_out vs_main(vertex_in vin)
{
    vertex_out vout;

	// Transform to homogeneous clip space.
    vout.posh = mul(float4(vin.position, 1.f), object_cb_vs.mvp);
    vout.pos = mul(float4(vin.position, 1.f), object_cb_vs.world).xyz;
    vout.normal = mul(float4(vin.normal, 0.f), object_cb_vs.world).xyz;

    // Tangent basis for normal mapping.
    float3x3 TBN = float3x3(vin.tangent, vin.bitangent, vin.normal);
    vout.tangent_basis = mul((float3x3) object_cb_vs.world, transpose(TBN));

    // Texture coordinates.
    vout.tex_coord = float2(vin.tex_coord.x, 1.f - vin.tex_coord.y);

    return vout;
}
#endif

#ifdef PIXEL_SHADER
ConstantBuffer<pass_data> cb_pass : register(b0);
struct object_data_ps
{
    float3 color;
    uint object_id;
    float2 roughness_metalness;
    float2 _pad;
};
ConstantBuffer<object_data_ps> object_cb_ps : register(b1);

Texture2D<float4> albedo : register(t0);
Texture2D<float3> normal : register(t1);
Texture2D<float3> roughness_metalness : register(t2);

SamplerState Sampler : register(s0);

struct pixel_out
{
    float4 gbuffer0 : SV_Target0; //DXGI_FORMAT_R11G11B10_FLOAT
    float4 gbuffer1 : SV_Target1; //DXGI_FORMAT_R10G10B10A2_UNORM
    float4 gbuffer2 : SV_Target2; //DXGI_FORMAT_R16G16B16A16_FLOAT
};

float3 compress_normal(float3 normal)
{
    const float max_xyz = max(abs(normal.x), max(abs(normal.y), abs(normal.z)));
    return ((normal / max_xyz) * 0.5f) + 0.5f;
}

pixel_out ps_main(vertex_out pin)
{
    pixel_out pout;

    // Albedo.
    if (any(object_cb_ps.color))
    {
        pout.gbuffer0 = float4(object_cb_ps.color, 1.f);
    }
    else
    {
        float4 tex_albedo = albedo.Sample(Sampler, pin.tex_coord);
        clip(tex_albedo.w - cb_pass.clip_delta);
        pout.gbuffer0 = tex_albedo;
    }

    // Normals.
    float3 n = normal.Sample(Sampler, pin.tex_coord).rgb;
    if (any(n))
    {
        n = normalize(2.f * n - 1.f);
        n = normalize(mul(pin.tangent_basis, n));
        pout.gbuffer1 = float4(compress_normal(n), 0.f);
    }
    else
    {
        pout.gbuffer1 = float4(compress_normal(pin.normal), 0.f);
    }

    // Roughness / Metalness.
    if (any(object_cb_ps.roughness_metalness))
    {
        pout.gbuffer2 = float4(0.f, object_cb_ps.roughness_metalness, object_cb_ps.object_id);
    }
    else
    {
        pout.gbuffer2 = float4(roughness_metalness.Sample(Sampler, pin.tex_coord), object_cb_ps.object_id);
    }
    return pout;
}
#endif
