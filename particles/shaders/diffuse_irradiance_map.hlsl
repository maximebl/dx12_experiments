#include "common.hlsl"

TextureCube unfiltered_envmap : register(t1, space20);
RWTexture2DArray<float4> diffuse_irradiance_map : register(u1, space20);
SamplerState linear_wrap_sampler : register(s0, space20);

//static const int samples_per_pixel = 64;
static const int samples_per_pixel = 64;
static const int num_samples = samples_per_pixel * envmap_res;

[numthreads(32, 32, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID)
{
    // Get normalized cube face texture coordinates.
    uint irrmap_face_width, irrmap_face_height, irrmap_face_depth;
    diffuse_irradiance_map.GetDimensions(irrmap_face_width, irrmap_face_height, irrmap_face_depth);
    float2 irrmap_face_uv = tid.xy / float2(irrmap_face_width, irrmap_face_height);

    // Normal of the surface being shaded.
    float3 N = inverse_sample(irrmap_face_uv, tid.z);

    // Compute a basis around that normal.
    float3 S, T;
    compute_basis(N, S, T);

    // Apply convolution.
    // Monte Carlo sampling with a Hammersley sequence.
    float3 irradiance = 0.f;
    for (int i = 0; i < num_samples; i++)
    {
        // Get a random number pair representing a random direction, in normalized spherical coordinates.
        float2 rand = sample_hammersley(i, num_samples);

        // Turn those random numbers into a 3D vector that samples a hemisphere.
        float3 to_sample = sample_hemisphere(rand.x, rand.y);

        // Transform the sampling vector into the same space as the world space normal of the surface being shaded.
        float3 to_light = tangent_to_world(to_sample, N, S, T);

        // Geometric factor.
        float NdotL = max(0.f, dot(N, to_light));

        irradiance += 2.f * unfiltered_envmap.SampleLevel(linear_wrap_sampler, to_light, 0.f).rgb * NdotL;
        // Where 2.f comes from:
        // The Lambertian BRDF wants to divide by PI to remain energy conserving.
        // So we scale our Lambertian BRDF for a material at full illumination (1) by 1/PI:
        // LambertianBRDF = 1 * (1/PI) = 1/PI.

        // When evaluating the Monte Carlo estimator, we divide by a probability density function (PDF).
        // In our case, the PDF of a unit hemisphere is 1/2PI.

        // PDF = 1/2PI
        // Weight = 1/PDF = 1 / (1 / (2PI)).
        // Weight * LambertianBRDF = 2.
        
    }
    irradiance /= num_samples;

    diffuse_irradiance_map[tid] = float4(irradiance, 1.f);
}
