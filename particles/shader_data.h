#pragma once
#include "directx12_include.h"
#include <DirectXMath.h>

// Render pass constant data.
struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) pass_data
{
    DirectX::XMFLOAT4X4 view;
    DirectX::XMFLOAT4X4 inv_view;
    DirectX::XMFLOAT4X4 proj;
    DirectX::XMFLOAT4X4 screen_to_world;
    DirectX::XMFLOAT4 frustum_planes[6];
    DirectX::XMFLOAT3 eye_pos;
    float near_plane;
    DirectX::XMFLOAT3 eye_forward;
    float far_plane;
    DirectX::XMFLOAT3 ambient_light;
    float time;
    float delta_time;
    DirectX::XMFLOAT2 screen_size;
    float aspect_ratio;
    INT specular_shading;
    INT brdf_id;
    INT do_tonemapping;
    float exposure;
    INT use_pcf_point_shadows;
    INT use_pcf_max_quality;
    INT indirect_diffuse_brdf;
    INT indirect_specular_brdf;
    INT direct_diffuse_brdf;
    INT direct_specular_brdf;
    float clip_delta;
};

// Per object constant data.
struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) object_data_vs
{
    DirectX::XMFLOAT4X4 world;
    DirectX::XMFLOAT4X4 world_view_proj;
};

struct render_object_data_ps
{
    DirectX::XMFLOAT3 color;
    UINT object_id;
    DirectX::XMFLOAT2 roughness_metalness;
    DirectX::XMFLOAT2 _pad;
};

// Light data.
struct point_light
{
    DirectX::XMFLOAT3 position_ws;
    float falloff_start;
    DirectX::XMFLOAT3 color;
    float falloff_end;
    DirectX::XMFLOAT3 strenght;
    UINT id;
};

struct attractor_point_light
{
    DirectX::XMFLOAT4X4 world;
    point_light light;
    DirectX::XMFLOAT4X4 view_proj[6];
};

struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) volume_light_data_ps
{
    DirectX::XMFLOAT4X4 world_to_object;
    DirectX::XMFLOAT4 color;
    DirectX::XMFLOAT3 object_space_cam_pos;
    float radius;
    DirectX::XMFLOAT3 object_space_cam_forward;
    float _pad1;
};

struct spot_light
{
    DirectX::XMFLOAT3 position_ws;
    float falloff_start;
    DirectX::XMFLOAT3 color;
    float falloff_end;
    DirectX::XMFLOAT3 strenght;
    float spot_power;
    DirectX::XMFLOAT3 direction;
    UINT id;
    DirectX::XMFLOAT4X4 view_proj;
};

// Particle system data.
struct alignas(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) particle_system_info
{
    DirectX::XMFLOAT4X4 world;
    int particle_system_index;
    float amplitude;
    float frequency;
    float speed;
    point_light light;
    INT particle_lights_enabled;
    float depth_fade;
};
