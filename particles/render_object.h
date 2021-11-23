#pragma once
#include "transform.h"
#include "mesh.h"
#include "DirectXMath.h"
#include <optional>

struct render_object
{
    transform m_transform;
    mesh m_mesh;
    DirectX::XMFLOAT3 m_color;
    DirectX::XMFLOAT2 m_roughness_metalness;

    // EXPERIMENTAL: Enum default parameter we can set to null instead of zero which often represents a default flag.
    std::vector<ComPtr<ID3D12Resource>> resources(std::optional<texture_type> textures = std::nullopt);
};
