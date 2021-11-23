#include "common.hlsl"

struct vertex_out
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD;
};

#ifdef VERTEX_SHADER
vertex_out vs_main(uint vertexID : SV_VertexID)
{
    vertex_out vout;
    // Generate a screen-filling triangle.
    if (vertexID == 0)
    {
        // Top right.
        vout.texcoord = float2(1.0, -1.0);
        vout.position = float4(1.0, 3.0, 0.0, 1.0);
    }
    else if (vertexID == 1)
    {
        // Bottom left.
        vout.texcoord = float2(-1.0, 1.0);
        vout.position = float4(-3.0, -1.0, 0.0, 1.0);
    }
    else
    {
        // Bottom right.
        vout.texcoord = float2(1.0, 1.0);
        vout.position = float4(1.0, -1.0, 0.0, 1.0);
    }
    return vout;
}
#endif

#ifdef PIXEL_SHADER

ConstantBuffer<pass_data> cb_pass : register(b0);
ConstantBuffer<counter> cb_particle_lights_counter : register(b1);
SamplerState linear_wrap_sampler : register(s0);
SamplerComparisonState shadow_comp_sampler : register(s1);
SamplerState linear_clamp_sampler : register(s2);
Texture2D<float4> gbuffer0 : register(t0);
Texture2D<float4> gbuffer1 : register(t1);
Texture2D<float4> gbuffer2 : register(t2);
Texture2D<float> Depth : register(t3);
StructuredBuffer<point_light> sb_particle_lights : register(t4);
StructuredBuffer<spot_light> sb_spot_lights : register(t5);
StructuredBuffer<attractor_point_light> sb_attractor_lights : register(t6);
TextureCube diffuse_irradiance_map : register(t7);
TextureCube specular_irradiance_map : register(t8);
Texture2D<float2> specular_brdf_lut : register(t9);
Texture2DArray spotlight_shadow_maps : register(t10);
TextureCubeArray pointlight_shadow_maps : register(t11);

// Materials.
static const float3 Fdielectric = 0.04;
struct material
{
    float4 diffuse_albedo;
    float3 fresnel_F0;
    float roughness;
    float metalness;
    uint diffuse_brdf_id;
    uint specular_brdf_id;
};

float attenuation(float falloff_start, float falloff_end, float dist_to_light)
{
    float a = falloff_end - dist_to_light;
    float b = falloff_end - falloff_start;
    return clamp(a / b, 0.f, 1.f);
}

// Shlick's approximation of the Fresnel factor.
float3 fresnel_schlick1(float3 F0, float cosL)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosL, 5.0);
}

float3 fresnel_schlick2(float3 F0, float NdotL)
{
    float f0 = 1.0f - NdotL;
    float3 reflectPercent = F0 + (1.0f - F0) * (f0 * f0 * f0 * f0 * f0);

    return reflectPercent;
}

float3 blinn_phong(float3 strenght, float NdotL, float NdotH, material mat)
{
    float shininess = 1.f - mat.roughness;
    shininess *= 256.f;

    float roughness_factor = (shininess + 8.f) * pow(NdotH, shininess) / 8.f;
    float3 fresnel_factor = fresnel_schlick2(mat.fresnel_F0, NdotL);
    float3 specular_albedo = roughness_factor * fresnel_factor;

    // Scale back down to [0, 1] for LDR rendering.
    specular_albedo = specular_albedo / (specular_albedo + 1.f);
    
    return float3(mat.diffuse_albedo.rgb + specular_albedo) * strenght;
}

float3 blinn_phong2(float NdotL, float NdotH, material mat)
{
    float shininess = 1.f - mat.roughness;
    shininess *= 256.f;

    float roughness_factor = (shininess + 8.f) * pow(NdotH, shininess) / 8.f;
    float3 fresnel_factor = fresnel_schlick2(mat.fresnel_F0, NdotL);
    float3 specular_albedo = roughness_factor * fresnel_factor;

    // Scale back down to [0, 1] for LDR rendering.
    specular_albedo = specular_albedo / (specular_albedo + 1.f);
    
    return specular_albedo;
}

float3 cook_torrance(float NdotL, float NdotV, float NdotH, float HdotL, material mat)
{
    float roughness = mat.roughness;
    float normalization_factor = max(epsilon, 4.f * NdotL * NdotV);

    // Distribution of normals.
    float D = ggx(NdotH, roughness);
    if (cb_pass.specular_shading == 1)
    {
        return D / normalization_factor;
    }

    // Microfacets Fresnel.
    float3 F = fresnel_schlick1(mat.fresnel_F0, HdotL);
    if (cb_pass.specular_shading == 2)
    {
        return F / normalization_factor;
    }

    // Geometric attenuation.
    // Epic's parameterization to reduce specular hotness.
    float r = roughness + 1.f;
    float k = (r * r) / 8.f;
    float shadowing = schlick_g1(NdotL, k);
    float masking = schlick_g1(NdotV, k);
    float G2 = masking * shadowing;
    if (cb_pass.specular_shading == 3)
    {
        return G2 / normalization_factor;
    }

    // Distribution of visible normals.
    if (cb_pass.specular_shading == 4)
    {
        return (G2 * D) / normalization_factor;
    }

    // Cook-Torrance.
    float3 numerator = D * F * G2;
    return numerator / normalization_factor;
}

float3 calc_direct_brdf(material mat, float NdotL, float NdotV, float NdotH, float HdotL)
{
    // Ratio of refracted (diffuse) light. 
    // Use the inverse of the amount of reflected energy as the ratio of refracted energy to ensure energy consevation.
    float3 F = fresnel_schlick1(mat.fresnel_F0, HdotL);
    float3 kd = float3(1.f, 1.f, 1.f) - F;

    // Metals either absorb or reflect light energy, therefore the ratio of refracted light should always be zero for pure metals.
    kd = lerp(kd, float3(0.f, 0.f, 0.f), mat.metalness);

    // Diffuse term.
    float3 diffuse = float3(0.f, 0.f, 0.f);
    switch (mat.diffuse_brdf_id)
    {
        // Lambertian.
        case 0:
            diffuse = mat.diffuse_albedo.rgb * kd;
            break;

        default:
            return float3(1.f, 0.f, 0.f); // Used to make it obvious when a proper BRDF is not selected.
    }

    // Specular term.
    float3 specular = float3(0.f, 0.f, 0.f);
    switch (mat.specular_brdf_id)
    {
        // Cook-Torrance.
        case 0:
            specular = cook_torrance(NdotL, NdotV, NdotH, HdotL, mat);
            break;

        // Blinn-Phong.
        case 1:
            specular = blinn_phong2(NdotL, NdotH, mat);
            break;

        default:
            return float3(1.f, 1.f, 1.f); // Used to make it obvious when a proper BRDF is not selected.
    }

    return diffuse + specular;
}

float3 compute_point_light(point_light light, material mat, float3 surface_point, float3 surface_normal, float3 to_eye)
{
    float3 to_light = light.position_ws - surface_point;

    // Distance to the light source.
    float light_dist = length(to_light);

    // Don't calculate pixel colors if they're out of the light's range.
    // This is not nearly as good as proper light culling, but better than nothing for now.
    if (light_dist > light.falloff_end)
    {
        return 0.f;
    }
    to_light /= light_dist;

    // Attenuate light by distance.
    float att = attenuation(light.falloff_start, light.falloff_end, light_dist);

    // Useful directions.
    float3 ideal_reflection_normal = normalize(to_eye + to_light); // The normal of facets that are oriented in such a way that they 
                                                        // reflect the incident light ray in the direction of the ideal specular relfection.
    float NdotL = max(dot(to_light, surface_normal), 0.f);
    float NdotV = max(dot(to_eye, surface_normal), 0.f);
    float NdotH = max(dot(ideal_reflection_normal, surface_normal), 0.f);
    float HdotL = max(dot(to_light, ideal_reflection_normal), 0.f);

    // Incoming radiance from this light.
    float3 Li = light.color * light.strenght;

    return (calc_direct_brdf(mat, NdotL, NdotV, NdotH, HdotL) * // BRDF.
           att) * // Point light specific distance attenuation. 
                  // Note that we don't apply it to incoming radiance.
                  // This is because light doesn't lose intensity will travelling.
           Li * // Incoming radiance.
           NdotL; // Geometric factor.
}

float3 compute_spot_light(spot_light light, material mat, float3 surface_point, float3 surface_normal, float3 to_eye)
{
    float3 to_light = light.position_ws - surface_point;

    float dist_to_light = length(to_light);

    if (dist_to_light > light.falloff_end)
    {
        return 0.f;
    }

    // Normalize.
    to_light /= dist_to_light;

    // Scale down by Lambert's cosine law.
    float NdotL = max(0.f, dot(to_light, surface_normal));
    float3 light_strenght = NdotL * light.strenght;

    // Calculate light attenuation.
    float att = attenuation(light.falloff_start, light.falloff_end, dist_to_light);
    light_strenght *= att;

    // Scale by spotlight.
    float spot_factor = pow(max(dot(normalize(light.direction), -to_light), 0.f), light.spot_power);
    light_strenght *= spot_factor;

    float3 ideal_reflection_normal = normalize(to_eye + to_light);
    float NdotH = max(dot(ideal_reflection_normal, surface_normal), 0.f);
    
    return blinn_phong(light_strenght, NdotL, NdotH, mat);
}

float get_spot_shadow_factor(float3 surface_point, float4x4 light_viewproj, uint shadowmap_index)
{
    float4 light_pos = mul(float4(surface_point, 1.f), light_viewproj);

    // Complete projection by doing division by w.
    light_pos.xyz /= light_pos.w;

	// Transform clip space coords to texture space coords (-1:1 to 0:1)
    light_pos.x = light_pos.x / 2.f + 0.5f;
    light_pos.y = -light_pos.y / 2.f + 0.5f;

    // Depth in NDC space.
    float depth = light_pos.z;

    // PCF.
    float dx = 1.f / (float) shadowmap_width;
    const float2 offsets[9] =
    {
        float2(-dx, -dx), float2(0.0f, -dx), float2(dx, -dx),
        float2(-dx, 0.0f), float2(0.0f, 0.0f), float2(dx, 0.0f),
        float2(-dx, +dx), float2(0.0f, +dx), float2(dx, +dx)
    };

    float percent_lit = 0.f;
    [unroll]
    for (int i = 0; i < 9; ++i)
    {
        percent_lit += spotlight_shadow_maps.SampleCmpLevelZero(shadow_comp_sampler, float3(light_pos.xy + offsets[i], shadowmap_index), depth).r;
    }
    return percent_lit / 9.f;
}

float3 get_point_shadow_factor(float3 surface_point, float3 light_pos_ws, uint shadowmap_index, float far_plane, bool use_pcf, bool use_pcf_max_quality)
{
    float3 light_to_pixel = surface_point - light_pos_ws;
    float current_depth = length(light_to_pixel);
    float shadow = 0.0;
    float bias = 0.15;

    if (use_pcf)
    {

        if (use_pcf_max_quality)
        {
            // Unoptimized version.
            float samples = 4.0;
            float offset = 0.1;
            for (float x = -offset; x < offset; x += offset / (samples * 0.5))
            {
                for (float y = -offset; y < offset; y += offset / (samples * 0.5))
                {
                    for (float z = -offset; z < offset; z += offset / (samples * 0.5))
                    {
                        float closest_depth = pointlight_shadow_maps.Sample(linear_wrap_sampler, float4(light_to_pixel + float3(x, y, z), shadowmap_index)).r;
                        closest_depth *= far_plane;
                        if (current_depth - bias < closest_depth)
                        {
                            shadow += 1.0;
                        }
                    }
                }
            }
            shadow /= (samples * samples * samples);
        }
        else
        {
            // Optimized version.
            float3 sampleOffsetDirections[20] =
            {
                float3(1, 1, 1), float3(1, -1, 1), float3(-1, -1, 1), float3(-1, 1, 1),
               float3(1, 1, -1), float3(1, -1, -1), float3(-1, -1, -1), float3(-1, 1, -1),
               float3(1, 1, 0), float3(1, -1, 0), float3(-1, -1, 0), float3(-1, 1, 0),
               float3(1, 0, 1), float3(-1, 0, 1), float3(1, 0, -1), float3(-1, 0, -1),
               float3(0, 1, 1), float3(0, -1, 1), float3(0, -1, -1), float3(0, 1, -1)
            };

            int samples = 20;
            float diskRadius = 0.05;
            for (int i = 0; i < samples; ++i)
            {
                float closest_depth = pointlight_shadow_maps.Sample(linear_wrap_sampler, float4(light_to_pixel + sampleOffsetDirections[i] * diskRadius, shadowmap_index)).r;
                closest_depth *= far_plane;
                if (current_depth - bias < closest_depth)
                {
                    shadow += 1.0;
                }
            }
            shadow /= float(samples);
        }
    }
    else
    {
        float closest_depth = pointlight_shadow_maps.Sample(linear_wrap_sampler, float4(light_to_pixel, shadowmap_index)).r;
        closest_depth *= far_plane;
        shadow = current_depth - bias < closest_depth ? 1.0f : 0.f;
    }
    return shadow;
}

float4 ps_main(vertex_out pin) : SV_Target
{
    float2 pixel_coord = pin.texcoord.xy;
    float depth = Depth.SampleLevel(linear_wrap_sampler, pixel_coord, 0.f);
    float3 surface_point = position_from_depth(depth, pixel_coord, cb_pass.screen_to_world);

    // Diffuse color albedo at pixel location.
    float4 diffuse_albedo = gamma_to_linear(gbuffer0.Sample(linear_wrap_sampler, pixel_coord));

    float2 roughness_metalness = gbuffer2.Sample(linear_wrap_sampler, pixel_coord).gb;
    float roughness = roughness_metalness.x;
    float metalness = roughness_metalness.y;

	// Fresnel reflectance at normal incidence (for metals use albedo color).
    float3 F0 = lerp(Fdielectric.xxx, diffuse_albedo.xyz, metalness);

    material mat;
    mat.diffuse_albedo = diffuse_albedo;
    mat.fresnel_F0 = F0;
    mat.roughness = roughness;
    mat.metalness = metalness;
    mat.diffuse_brdf_id = cb_pass.direct_diffuse_brdf;
    mat.specular_brdf_id = cb_pass.direct_specular_brdf;

    // Surface normal at pixel location.
    float3 surface_normal = normalize(2.f * gbuffer1.Sample(linear_wrap_sampler, pixel_coord).xyz - 1.0f);
    float3 to_eye = normalize(cb_pass.eye_pos - surface_point);
    float NdotV = max(dot(to_eye, surface_normal), 0.f);

    // Indirect lights contribution.
    float3 indirect_light = (float3) 0.f;
    float3 indirect_diffuse = (float3) 0.f;
    float3 indirect_specular = (float3) 0.f;

    // Indirect diffuse.
    switch (cb_pass.indirect_diffuse_brdf)
    {
        case 0: // Lambertian PDF IBL.
            float3 irradiance = diffuse_irradiance_map.Sample(linear_wrap_sampler, surface_normal).rgb;
            float3 F = fresnel_schlick1(F0, NdotV);

            // Ratio of refracted (diffuse) light. 
            // Use the inverse of the amount of reflected energy as the ratio of refracted energy to ensure energy consevation.
            float3 kd = float3(1.f, 1.f, 1.f) - F;
            // Metals either absorb or reflect light energy, therefore the ratio of refracted light should always be zero for pure metals.
            kd = lerp(kd, float3(0.f, 0.f, 0.f), metalness);

            float3 diffuse_ibl = kd * diffuse_albedo.rgb * irradiance;
            indirect_diffuse = diffuse_ibl;
            break;

        case 1: // Lambertian constant.
            indirect_diffuse = cb_pass.ambient_light * diffuse_albedo.rgb;
            break;
    }

    // Indirect specular.
    switch (cb_pass.indirect_specular_brdf)
    {
        case 0: // GGX PDF IBL.
            float3 to_ideal_specular = -reflect(to_eye, surface_normal);
            uint width, height, mip_levels;
            specular_irradiance_map.GetDimensions(0, width, height, mip_levels);
            float3 specular_irradiance = specular_irradiance_map.SampleLevel(linear_wrap_sampler,
                                                                    to_ideal_specular,
                                                                    mat.roughness * mip_levels).rgb; // Each mip level represents a level of roughness.

            float2 specular_brdf = specular_brdf_lut.Sample(linear_clamp_sampler, float2(NdotV, mat.roughness)).rg;
            float3 F0_biased_offset = mat.fresnel_F0 * specular_brdf.r  // F0 bias.
                                                     + specular_brdf.g; // F0 offset.
            indirect_specular = specular_irradiance * F0_biased_offset;
            break;

        case 1: // Simple reflection.
            indirect_specular = (float3) 0.f;
            break;
    }

    indirect_light = indirect_diffuse + indirect_specular;

    // Direct lights contribution.
    float3 direct_light = (float3) 0.f;

    // Attractor point lights direct lighting contribution.
    uint num_lights = 0;
    uint sb_light_stride = 0;
    sb_attractor_lights.GetDimensions(num_lights, sb_light_stride);
    for (uint k = 0; k < num_lights; k++)
    {
        attractor_point_light attractor_light = sb_attractor_lights[k];
        point_light light = attractor_light.light;

        // Point shadow contribution.
        float3 shadow_factor = get_point_shadow_factor(surface_point,
                                                       light.position_ws,
                                                       light.id,
                                                       cb_pass.far_plane,
                                                       cb_pass.use_pcf_point_shadows, cb_pass.use_pcf_max_quality);

        // Light contribution.
        direct_light += shadow_factor * compute_point_light(light, mat, surface_point, surface_normal, to_eye);
    }

    // Particle lights direct lighting contribution.
    uint particle_lights_count = cb_particle_lights_counter.count;
    for (uint i = 0; i < particle_lights_count; i++)
    {
        // Light contribution.
        point_light light = sb_particle_lights[i];
        direct_light += compute_point_light(light, mat, surface_point, surface_normal, to_eye);
    }

    // Spot lights direct lighting contribution.
    sb_spot_lights.GetDimensions(num_lights, sb_light_stride);
    for (uint j = 0; j < num_lights; j++)
    {
        // Spot shadow contribution.
        float3 shadow_factor = get_spot_shadow_factor(surface_point, sb_spot_lights[j].view_proj, sb_spot_lights[j].id);

        // Light contribution.
        direct_light += shadow_factor * compute_spot_light(sb_spot_lights[j],
                                                           mat,
                                                           surface_point,
                                                           surface_normal,
                                                           to_eye) * sb_spot_lights[j].color;
    }

    return float4(indirect_light + direct_light, 0.f);
}
#endif