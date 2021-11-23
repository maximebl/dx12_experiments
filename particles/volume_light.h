#pragma once
#include "transform.h"
#include "mesh.h"
#include "DirectXMath.h"
#include <optional>

enum blend_modes
{
    alpha_transparency,
    additive_transparency,
    blend_modes_MAX
};

struct volume_light
{
    transform m_transform;
    mesh m_mesh;
    DirectX::XMFLOAT4 m_color;
    float m_radius;
    blend_modes m_blend_mode;
};
