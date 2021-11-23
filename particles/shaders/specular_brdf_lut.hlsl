#include "common.hlsl"

RWTexture2D<float2> specular_brdf_LUT : register(u0, space20);
SamplerState linear_clamp_sampler : register(s1, space20);

static const int num_samples = 1024;
static const float inv_num_samples = 1.f / float(num_samples);

// Schlick-GGX approximation of geometric attenuation function using Smith's method (IBL version).
float schlick_ggx_ibl(float NdotL, float NdotV, float roughness)
{
    float r = (roughness * roughness) / 2.f; // IBL specific remapping.
    return schlick_g1(NdotL, r) * schlick_g1(NdotV, r);
}

[numthreads(32, 32, 1)]
void cs_main(uint2 tid : SV_DispatchThreadID)
{
    float lut_width, lut_height;
    specular_brdf_LUT.GetDimensions(lut_width, lut_height);

	// Get integration parameters.
    float NdotV = tid.x / (float)lut_width;
    NdotV = max(NdotV, epsilon2);
    float roughness = tid.y / lut_height;

	// Tangent-space view vector from angle to normal.
    float3 to_view = float3(sqrt(1.f - NdotV * NdotV), 0.f, NdotV);

    float DFG1 = 0.f;
    float DFG2 = 0.f;

    for (int sample = 0; sample < num_samples; sample++)
    {
        // Get a random number pair representing a random direction, in normalized spherical coordinates (phi, theta).
        float2 rand_dir = sample_hammersley(sample, num_samples);

        // Sample directly in tangent (shading) space. No need to transform to world space.
        float3 ideal_reflection_normal = importance_sample_ggx(rand_dir.x, rand_dir.y, roughness);
        float3 to_light = -reflect(to_view, ideal_reflection_normal);

        float NdotH = ideal_reflection_normal.z;
        float NdotL = to_light.z;
        float VdotH = max(dot(to_view, ideal_reflection_normal), 0.f);

        if (NdotL > 0.f)
        {
           // Geometric attenuation. 
            float G = schlick_ggx_ibl(NdotL, NdotV, roughness);
            float Gv = G * VdotH / (NdotH * NdotV);
            float Fc = pow(1.f - VdotH, 5);

            DFG1 += (1.f - Fc) * Gv;
            DFG2 += Fc * Gv;
        }
    }

    specular_brdf_LUT[tid] = float2(DFG1, DFG2) * inv_num_samples;
}
