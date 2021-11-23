#include "common.hlsl"

static const int num_samples = 256;

struct filter_setting
{
    float roughness;
};
ConstantBuffer<filter_setting> cb_filter_setting : register(b4);

TextureCube unfiltered_envmap : register(t2, space20);
RWTexture2DArray<float4> specular_map_mip : register(u2, space20);
SamplerState linear_wrap_sampler : register(s0, space20);

[numthreads(32, 32, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID)
{
	// Dont write past the specular map face texture when computing higher mipmap levels.
    uint specmap_face_width, specmap_face_height, specmap_face_depth;
    specular_map_mip.GetDimensions(specmap_face_width, specmap_face_height, specmap_face_depth);
    if (tid.x >= specmap_face_width || tid.y >= specmap_face_height)
    {
        return;
    }

	// Solid angle associated with a single cubemap texel at zero mipmap level.
	// This will come in handy for importance sampling below.
    float envmap_face_with, envmap_face_height, envmap_levels;
    unfiltered_envmap.GetDimensions(0, envmap_face_with, envmap_face_height, envmap_levels);
    float envmap_area = (6 * envmap_face_with * envmap_face_height);
    float total_steradians = 4.f * PI; // There are 4PI steradians on a unit sphere.
    float mip0_texel_solid_angle = total_steradians / envmap_area; // Steradians per texel.

    // Get normalized cube face texture coordinates.
    float2 specmap_face_uv = tid.xy / float2(specmap_face_width, specmap_face_height);

	// Approximation: Assume zero viewing angle (isotropic reflections).
    float3 macro_surface_normal = inverse_sample(specmap_face_uv, tid.z);
    float3 to_view = macro_surface_normal;

    // Compute a basis around that normal.
    float3 S, T;
    compute_basis(macro_surface_normal, S, T);

    // Estimate the rendering equation integral.
	// Convolve the environment map using GGX NDF importance sampling.
    float3 color = 0;
    float weight = 0;
    for (int sample = 0; sample < num_samples; sample++)
    {
        // Get a random number pair representing a random direction, in normalized spherical coordinates (phi, theta).
        float2 rand_dir = sample_hammersley(sample, num_samples);

        // Obtain an important sample according to the roughness, in world space.
        float3 ideal_reflection_normal = tangent_to_world(importance_sample_ggx(rand_dir.x, rand_dir.y, cb_filter_setting.roughness),
                                                          macro_surface_normal, S, T);
        float3 to_light = -reflect(to_view, ideal_reflection_normal);
        float NdotL = dot(macro_surface_normal, to_light);
        if (NdotL > 0.f)
        {
            float NdotH = max(dot(macro_surface_normal, ideal_reflection_normal), 0.f);

			// Use Mipmap Filtered Importance Sampling to improve convergence.

            // D term.
            float pdf = ggx(NdotH, cb_filter_setting.roughness) * 0.25f;

            // Divide the Monte Carlo estimator by 1/PDF to normalize it.

            // Solid angle for this sample.
            float normalized_samples_sphere = num_samples * pdf;
            float sample_texel_solid_angle = 1.f / normalized_samples_sphere;

			// Mip level to sample from.
            float texel_area = sample_texel_solid_angle / mip0_texel_solid_angle;
            float max_mips_from_texel = log2(texel_area);
            float mip_level = max(max_mips_from_texel * 0.5f + 1.f, 0.f);
            
            color = unfiltered_envmap.SampleLevel(linear_wrap_sampler, to_light, mip_level).rgb * NdotL;
            weight += NdotL;
        }
    }
    color /= weight;

    specular_map_mip[tid] = float4(color, 0.f);
}
