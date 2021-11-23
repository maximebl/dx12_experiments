#include "camera.h"

using namespace DirectX;

camera::camera(float in_aspect_ratio,
               transform in_transform,
               float in_near,
               float in_far,
               float in_fov_angle)
    : m_transform(in_transform),
      m_vfov_angle(in_fov_angle),
      m_aspect_ratio(in_aspect_ratio),
      m_near(in_near),
      m_far(in_far)
{
    XMVECTOR forward = XMLoadFloat3(&m_transform.m_forward);
    XMVECTOR position = XMLoadFloat3(&m_transform.m_translation);

    XMVECTOR scaled_cam_dir = XMVectorScale(forward, 15.f);

    calc_projection();
    viewspace_frustum_planes();
}

void camera::update_position()
{
    XMVECTOR right = XMLoadFloat3(&m_transform.m_right);
    XMVECTOR up = XMLoadFloat3(&m_transform.m_up);
    XMVECTOR forward = XMLoadFloat3(&m_transform.m_forward);
    XMVECTOR position = XMLoadFloat3(&m_transform.m_translation);
    XMVECTOR step = XMVectorReplicate(0.030f);

    if (GetAsyncKeyState('E'))
    {
        position = XMVectorMultiplyAdd(-step, up, position);
    }
    if (GetAsyncKeyState('Q'))
    {
        position = XMVectorMultiplyAdd(step, up, position);
    }

    if (GetAsyncKeyState('W'))
    {
        position = XMVectorMultiplyAdd(step, forward, position);
    }
    if (GetAsyncKeyState('S'))
    {
        position = XMVectorMultiplyAdd(-step, forward, position);
    }
    if (GetAsyncKeyState('A'))
    {
        position = XMVectorMultiplyAdd(-step, right, position);
    }
    if (GetAsyncKeyState('D'))
    {
        position = XMVectorMultiplyAdd(step, right, position);
    }

    m_transform.set_translation(XMVectorGetX(position), XMVectorGetY(position), XMVectorGetZ(position));
}

void camera::update_view_proj()
{
    XMVECTOR position = XMLoadFloat3(&m_transform.m_translation);
    XMVECTOR right = XMLoadFloat3(&m_transform.m_right);
    XMVECTOR up = XMLoadFloat3(&m_transform.m_up);
    XMVECTOR forward = XMLoadFloat3(&m_transform.m_forward);
    XMMATRIX proj = XMLoadFloat4x4(&m_proj);

    // Renormalize camera axis
    forward = XMVector3Normalize(forward);
    right = XMVector3Cross(up, forward);
    up = XMVector3Normalize(XMVector3Cross(forward, right));

    // Camera rotation
    XMMATRIX R;
    R.r[0] = right;
    R.r[1] = up;
    R.r[2] = forward;
    R.r[3] = XMVectorSet(0.f, 0.f, 0.f, 1.f);

    // Camera translation
    XMMATRIX P;
    P = XMMatrixTranslationFromVector(position);

    // The view matrix is the inverted camera transform
    XMMATRIX V;
    XMMATRIX inv_V;
    V = R * P;                                       // Order of multiplication is reversed
    inv_V = XMMatrixInverse(&XMMatrixDeterminant(V), V); // because we invert the result

    // Calculate the view-projection matrix.
    XMMATRIX view_proj = inv_V * proj;

    XMStoreFloat4x4(&m_view_proj, view_proj);
    XMStoreFloat4x4(&m_inv_view, inv_V);
    XMStoreFloat4x4(&m_view, V);
    XMStoreFloat3(&m_transform.m_right, right);
    XMStoreFloat3(&m_transform.m_up, up);
    XMStoreFloat3(&m_transform.m_forward, forward);
}

void camera::calc_projection()
{
    XMStoreFloat4x4(&m_proj, XMMatrixPerspectiveFovLH(m_vfov_angle, m_aspect_ratio, m_near, m_far));
    m_proj_dist = m_proj(1, 1);
}

void camera::update_yaw_pitch(XMFLOAT2 current_mouse_pos, XMFLOAT2 last_mouse_pos)
{
    XMVECTOR position = XMLoadFloat3(&m_transform.m_translation);
    XMVECTOR right = XMLoadFloat3(&m_transform.m_right);
    XMVECTOR up = XMLoadFloat3(&m_transform.m_up);
    XMVECTOR forward = XMLoadFloat3(&m_transform.m_forward);

    float dx = XMConvertToRadians(0.5f * (current_mouse_pos.x - last_mouse_pos.x));
    float dy = XMConvertToRadians(0.5f * (current_mouse_pos.y - last_mouse_pos.y));

    if (GetAsyncKeyState(VK_MENU))
    {
        XMVECTOR start_cam_pos = position;

        XMVECTOR scaled_cam_dir = XMVectorScale(forward, 15.f);
        XMVECTOR orbit_target_pos = start_cam_pos + scaled_cam_dir;

        // Get the direction vector from the camera position to the target position
        XMVECTOR cam_to_target_dir = orbit_target_pos - position;

        // Translate to target position (direction + point = point)
        position = cam_to_target_dir + position;

        // Rotate camera axis
        XMMATRIX to_yaw = XMMatrixRotationY(dx);
        XMMATRIX to_pitch = XMMatrixRotationAxis(right, dy);
        XMMATRIX to_rot = to_pitch * to_yaw; // apply global yaw to local pitch

        XMVECTOR rot_cam_forward;
        rot_cam_forward = XMVector3Transform(forward, to_rot);
        right = XMVector3Transform(right, to_rot);
        up = XMVector3Transform(up, to_rot);

        rot_cam_forward = XMVector3Normalize(rot_cam_forward);

        // get the distance from the camera position to the object it orbits
        XMVECTOR cam_to_target = XMVectorSubtract(orbit_target_pos, start_cam_pos);
        float dist_to_target = XMVectorGetX(XMVector3Length(cam_to_target));

        // Place the camera back
        rot_cam_forward = XMVectorScale(rot_cam_forward, -dist_to_target);
        position = XMVectorAdd(position, rot_cam_forward);

        //look at the thing we're orbiting around
        forward = XMVector4Normalize(XMVectorSubtract(orbit_target_pos, position));
    }
    else
    {
        XMMATRIX yaw = XMMatrixRotationY(dx);
        right = XMVector3TransformNormal(right, yaw);
        up = XMVector3TransformNormal(up, yaw);
        forward = XMVector3TransformNormal(forward, yaw);

        XMMATRIX pitch = XMMatrixRotationAxis(right, dy);
        forward = XMVector3TransformNormal(forward, pitch);
        up = XMVector3TransformNormal(up, pitch);
    }

    m_x_accum = m_transform.m_rotation.y;
    m_y_accum = m_transform.m_rotation.x;
    m_x_accum += dx;
    m_y_accum += dy;
    m_transform.m_rotation.x = m_y_accum;
    m_transform.m_rotation.y = m_x_accum;

    m_transform.set_translation(XMVectorGetX(position), XMVectorGetY(position), XMVectorGetZ(position));

    XMStoreFloat3(&m_transform.m_right, right);
    XMStoreFloat3(&m_transform.m_up, up);
    XMStoreFloat3(&m_transform.m_forward, forward);
}

std::vector<camera::vertex_debug> camera::get_debug_frustum_vertices()
{
    std::vector<vertex_debug> vertices(0);
    vertices.reserve(12);

    // Far plane
    auto far_plane_corners = calc_plane_vertices(m_far);
    vertices.insert(vertices.end(),
                    far_plane_corners.begin(),
                    far_plane_corners.end());

    // Projection plane
    auto projection_plane_corners = calc_plane_vertices(m_proj(1, 1));
    vertices.insert(vertices.end(),
                    projection_plane_corners.begin(),
                    projection_plane_corners.end());

    // Near plane
    auto near_plane_corners = calc_plane_vertices(m_near);
    vertices.insert(vertices.end(),
                    near_plane_corners.begin(),
                    near_plane_corners.end());

    return vertices;
}

std::vector<camera::vertex_debug> camera::calc_plane_vertices(float dist_from_origin)
{
    XMVECTOR translation = XMLoadFloat3(&m_transform.m_translation); // C
    XMVECTOR right = XMLoadFloat3(&m_transform.m_right);             // x
    XMVECTOR up = XMLoadFloat3(&m_transform.m_up);                   // y
    XMVECTOR forward = XMLoadFloat3(&m_transform.m_forward);         // z

    float z_scale = dist_from_origin;
    float y_scale = z_scale / m_proj_dist;
    float x_scale = y_scale * m_aspect_ratio;

    XMVECTOR scaled_right = right * x_scale;
    XMVECTOR scaled_up = up * y_scale;
    XMVECTOR scaled_forward = forward * z_scale;

    vertex_debug bottom_right = {}; // q0
    vertex_debug top_right = {};    // q1
    vertex_debug top_left = {};     // q2
    vertex_debug bottom_left = {};  // q3

    bottom_right.color = {1.f, 1.f, 1.f, 0.2f};
    top_right.color = {1.f, 1.f, 1.f, 0.2f};
    top_left.color = {1.f, 1.f, 1.f, 0.2f};
    bottom_left.color = {1.f, 1.f, 1.f, 0.2f};

    XMStoreFloat3(&bottom_right.position, scaled_right - scaled_up + scaled_forward + translation);
    XMStoreFloat3(&top_right.position, scaled_right + scaled_up + scaled_forward + translation);
    XMStoreFloat3(&top_left.position, -scaled_right + scaled_up + scaled_forward + translation);
    XMStoreFloat3(&bottom_left.position, -scaled_right - scaled_up + scaled_forward + translation);

    return {bottom_right, top_right, top_left, bottom_left};
}

void camera::viewspace_frustum_planes()
{
    XMStoreFloat4(&m_near_plane, XMVectorSet(0.f, 0.f, 1.f, -m_near));
    XMStoreFloat4(&m_far_plane, XMVectorSet(0.f, 0.f, -1.f, m_far));
    XMStoreFloat4(&m_left_plane, XMVector4Normalize(XMVectorSet(m_proj_dist, 0.f, m_aspect_ratio, 0.f)));
    XMStoreFloat4(&m_right_plane, XMVector4Normalize(XMVectorSet(-m_proj_dist, 0.f, m_aspect_ratio, 0.f)));
    XMStoreFloat4(&m_top_plane, XMVector4Normalize(XMVectorSet(0.f, -m_proj_dist, 1.f, 0.f)));
    XMStoreFloat4(&m_bottom_plane, XMVector4Normalize(XMVectorSet(0.f, m_proj_dist, 1.f, 0.f)));
}

std::vector<UINT16> camera::get_frustum_indices()
{
    std::vector<UINT16> frustum_indices = {0, 1, 1, 2, 2, 3, 3, 0,
                                           0, 4, 1, 5, 2, 6, 3, 7,
                                           4, 5, 5, 6, 6, 7, 7, 4,
                                           4, 8, 5, 9, 6, 10, 7, 11,
                                           8, 9, 9, 10, 10, 11, 11, 8};
    return frustum_indices;
}

std::vector<UINT16> camera::get_frustum_planes_indices()
{
    std::vector<UINT16> frustum_planes_indices = {
        0,
        1,
        2, // Far plane
        0,
        2,
        3,

        4,
        5,
        6, // Projection plane
        4,
        6,
        7,

        8,
        9,
        10, // Near plane
        8,
        10,
        11,

        2,
        3,
        10, // Left plane
        10,
        3,
        11,

        1,
        0,
        9, // Right plane
        9,
        0,
        8,

        1,
        2,
        10, // Top plane
        1,
        10,
        9,

        0,
        3,
        11, // Bottom plane
        0,
        11,
        8,
    };
    return frustum_planes_indices;
}
