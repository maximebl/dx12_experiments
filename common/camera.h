#pragma once
#include "common.h"
#include "transform.h"
#include "gpu_interface.h"

#pragma warning(push)
#pragma warning(disable : 4251) // Safe to ignore because the users of this DLL will always be compiled together with the DLL
struct COMMON_API camera
{
    struct vertex_debug
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT4 color;
    };
    camera() = default;
    camera(float in_aspect_ratio,
           transform in_transform = transform{},
           float in_near = 1.f,
           float in_far = 1000.f,
           float in_fov_angle = 0.25f * DirectX::XM_PI);

    void update_position();
    void update_view_proj();
    void calc_projection();
    void update_yaw_pitch(DirectX::XMFLOAT2 current_mouse_pos,
                          DirectX::XMFLOAT2 last_mouse_pos);
    std::vector<vertex_debug> get_debug_frustum_vertices();
    std::vector<vertex_debug> calc_plane_vertices(float dist_from_origin);

    std::vector<UINT16> get_frustum_planes_indices();
    std::vector<UINT16> get_frustum_indices();
    void viewspace_frustum_planes();

    float m_vfov_angle = 0.f;
    float m_aspect_ratio = 0.f;
    float m_near = 0.f;
    float m_far = 0.f;
    float m_proj_dist = 0.f;
    float m_x_accum = 0.f;
    float m_y_accum = 0.f;
    transform m_transform = {};
    DirectX::XMFLOAT4X4 m_inv_view = {};
    DirectX::XMFLOAT4X4 m_view = {};
    DirectX::XMFLOAT4X4 m_proj = {};
    DirectX::XMFLOAT4X4 m_view_proj = {};

    DirectX::XMFLOAT4 m_far_plane;
    DirectX::XMFLOAT4 m_near_plane;
    DirectX::XMFLOAT4 m_left_plane;
    DirectX::XMFLOAT4 m_right_plane;
    DirectX::XMFLOAT4 m_top_plane;
    DirectX::XMFLOAT4 m_bottom_plane;
};
#pragma warning(pop)
