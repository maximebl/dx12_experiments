#if defined(__INTELLISENSE__) // HLSL Tools specific
#define VERTEX_SHADER
#define PIXEL_SHADER
#define GEOMETRY_SHADER
#endif

#pragma warning( disable : 4000 ) // Buggy warning that goes away in future versions of the compiler.

#include "../shader_shared_constants.h"

// Constants.
#define GPU_VIRTUAL_ADDRESS double
static const float PI = 3.141592f;
static const float twoPI = 2 * PI;
static const float epsilon = 0.0001f;
static const float epsilon2 = 0.001f;
static const float gamma = 2.2f;
static const float pure_white = 1.0f;

float4 gamma_to_linear(float4 c)
{
    return float4(pow(abs(c.rgb), 2.2f), c.a);
}

float3 linear_to_gamma(float3 color)
{
    return pow(abs(color.rgb), 1.f / gamma);
}

float linearize_depth(float depth, float n, float f)
{
    return (2.0f * n) / (f + n - depth * (f - n));
}

float linearize_depth2(float depth, float n, float f)
{
    float z_b = depth;
    float z_n = 2.0 * z_b - 1.0;
    float lin = 2.0 * f * n / (n + f - z_n * (n - f));
    return lin;
}

float3 inverse_sample(float2 cube_face_uv, float cube_face_index)
{
    float2 uv = 2.0 * float2(cube_face_uv.x, 1.0 - cube_face_uv.y) - float2(1.0, 1.0);

	// Select vector based on cubemap face index.
    float3 ret = 0;
    switch (cube_face_index)
    {
        case 0:
            ret = float3(1.0, uv.y, -uv.x);
            break;
        case 1:
            ret = float3(-1.0, uv.y, uv.x);
            break;
        case 2:
            ret = float3(uv.x, 1.0, -uv.y);
            break;
        case 3:
            ret = float3(uv.x, -1.0, uv.y);
            break;
        case 4:
            ret = float3(uv.x, uv.y, 1.0);
            break;
        case 5:
            ret = float3(-uv.x, uv.y, -1.0);
            break;
    }
    return normalize(ret);
}

// Compute orthonormal basis from N.
void compute_basis(float3 N, out float3 S, out float3 T)
{
    float3 right = float3(1.f, 0.f, 0.f);
    float3 up = float3(0.f, 1.f, 0.f);

    // Branchless technique to compute a non-zero vector T.
    T = cross(N, up);
    T = lerp(cross(N, right), up, step(epsilon, dot(up, up)));
    T = normalize(T);

    // S is trivial once you have N and T.
    S = cross(N, T);
}

// Convert point from tangent/shading space to world space.
float3 tangent_to_world(const float3 ts_normal, const float3 N, const float3 S, const float3 T)
{
    // Dot product between the tangent space normal and the tangent space transform.
    float3 tangent = S * ts_normal.x;
    float3 binormal = T * ts_normal.y;
    float3 normal = N * ts_normal.z;
    float3 ws_normal = tangent + binormal + normal;
    return ws_normal;
}

// Van der Corput radical inverse.
float radical_inverse(uint bits)
{
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}

// Sample i-th point from Hammersley point set of num_samples points total.
// http://holger.dammertz.org/stuff/notes_HammersleyOnHemisphere.html
float2 sample_hammersley(uint i, uint num_samples)
{
	return float2(i * rcp((float)num_samples), radical_inverse(i));
}

// Uniformly sample points on a hemisphere.
float3 sample_hemisphere(float u1, float u2)
{
    float3 to_sample = (float3)0.f;
    float u1_point = sqrt(max(0.f, 1.f - u1 * u1));
    float x = u1_point * cos(twoPI * u2);
    float y = u1_point * sin(twoPI * u2);
    float z = u1;
    to_sample = float3(x,y,z);
    return to_sample;
}

float2 cartesian_to_spherical(float3 v)
{
    float phi = atan2(v.z, v.x); // Hemisphere azimuth angle.
    float theta = acos(v.y); // Hemisphere polar angle.
    return float2(phi, theta);
}

float3 position_from_depth(float depth, float2 pixel_coord, matrix screen_to_world)
{
    float2 clipspace_pos = pixel_coord;

    // Put (0,0) at the center. [-1, 1]
    clipspace_pos *= 2.0f;
    clipspace_pos -= 1.0f;

    // Flip the coordinates from texture-space Y-down to Y-up.
    clipspace_pos.y *= -1.0f;

    // Reconstruct the position's Z component using depth.
    float4 screen_pos = float4(clipspace_pos, depth, 1.f);

    // Transform from screen space to world space.
    float4 position_ws = mul(screen_pos, screen_to_world);

    // Apply perspective divide to put in NDC space.
    position_ws /= position_ws.w;

    return position_ws.xyz;
}

bool is_sky(float depth)
{
    return depth >= 1.0f;
}

// Trowbridge-Reitz normal distribution function.
// Uses Disney's reparametrization of alpha = roughness^2.
float ggx(float NdotH, float roughness)
{
    // Disney's parameterization. 
    // Makes the effect of changing the roughness more linear from the user's point of view.
    float alpha = roughness * roughness;
    float alphaSq = alpha * alpha;

    float denom = (NdotH * NdotH) * (alphaSq - 1.f) + 1.f;
    return alphaSq / (PI * denom * denom);
}

// This returns normalized half-vector between to_light and to_view.
float3 importance_sample_ggx(float u1, float u2, float roughness)
{
    float alpha = roughness * roughness;

    float cosTheta = sqrt((1.0 - u2) / (1.0 + (alpha * alpha - 1.0) * u2));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta); // Trig. identity
    float phi = twoPI * u1;

	// Convert to Cartesian upon return.
    return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

// Schlick's GGX approximation of geometric attenuation function using Smith's method.
// cosTheta is either: dot(to_view, ideal_reflection_normal) or dot(to_light, ideal_reflection_normal).
float schlick_g1(float NdotX, float roughness)
{
    return NdotX / (NdotX * (1.f - roughness) + roughness);
}

// Mirror of CPU-side D3D12 structs.
struct bounding_box
{
    float3 extents;
    float4 center;
    float3 position[8];
};

struct object_data_vs
{
    float4x4 world;
    float4x4 mvp;
};

struct counter
{
    uint count;
};

struct pass_data
{
    matrix view;
    matrix inv_view;
    matrix proj;
    matrix screen_to_world;
    float4 frustum_planes[6];
    float3 eye_pos;
    float near_plane;
    float3 eye_forward;
    float far_plane;
    float3 ambient_light;
    float time;
    float delta_time;
    float2 sceen_size;
    float aspect_ratio;
    int specular_shading;
    int brdf_id;
    int do_tonemapping;
    float exposure;
    int use_pcf_point_shadows;
    int use_pcf_max_quality;
    int indirect_diffuse_brdf;
    int indirect_specular_brdf;
    int direct_diffuse_brdf;
    int direct_specular_brdf;
    float clip_delta;
};

struct spot_light
{
    float3 position_ws;
    float falloff_start;
    float3 color;
    float falloff_end;
    float3 strenght;
    float spot_power;
    float3 direction;
    uint id;
    float4x4 view_proj;
};

struct point_light
{
    float3 position_ws;
    float falloff_start;
    float3 color;
    float falloff_end;
    float3 strenght;
    uint id;
};

struct attractor_point_light
{
    float4x4 world;
    point_light light;
    float4x4 view_proj[6];
};

struct particle
{
    float3 position;
    float size;
    float3 velocity;
    float age;
};

struct particle_system_info
{
    float4x4 world;
    int particle_system_index;
    float amplitude;
    float frequency;
    float speed;
    point_light light;
    int particle_lights_enabled;
    float depth_fade;
};

struct particle_buffer_info
{
    uint input_buffer_index;
    uint output_buffer_index;
};

// Indirect commands definitions.
struct VERTEX_BUFFER_VIEW
{
    GPU_VIRTUAL_ADDRESS BufferLocation;
    uint SizeInBytes;
    uint StrideInBytes;
};

struct D3D12_DRAW_ARGUMENTS
{
    uint VertexCountPerInstance;
    uint InstanceCount;
    uint StartVertexLocation;
    uint StartInstanceLocation;
};

struct D3D12_DISPATCH_ARGUMENTS
{
    uint ThreadGroupCountX;
    uint ThreadGroupCountY;
    uint ThreadGroupCountZ;
};
