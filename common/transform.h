#pragma once
#include <DirectXMath.h>

struct transform
{
    transform(DirectX::XMFLOAT3 in_translation = DirectX::XMFLOAT3(0.f, 0.f, 0.f),
              DirectX::XMFLOAT3 in_rotation = DirectX::XMFLOAT3(0.f, 0.f, 0.f),
              DirectX::XMFLOAT3 in_scale = DirectX::XMFLOAT3(1.f, 1.f, 1.f))
        : m_translation(in_translation),
          m_rotation(in_rotation),
          m_scale(in_scale)
    {
        update_world();
    }

    void set_translation(float x, float y, float z)
    {
        m_translation.x = x;
        m_translation.y = y;
        m_translation.z = z;
        update_world();
    }

    void set_scale(float x, float y, float z)
    {
        m_scale.x = x;
        m_scale.y = y;
        m_scale.z = z;
        update_world();
    }

    void set_rotation(float x, float y, float z)
    {
        m_rotation.x = x;
        m_rotation.y = y;
        m_rotation.z = z;
        update_world();
    }

    void update_world()
    {
        using namespace DirectX;

        XMMATRIX right_mat = XMMatrixRotationX(m_rotation.x);
        XMMATRIX up_mat = XMMatrixRotationY(m_rotation.y);
        XMMATRIX forward_mat = XMMatrixRotationZ(m_rotation.z);
        XMMATRIX rotation = right_mat * up_mat * forward_mat;

        XMMATRIX transform;
        XMMATRIX scale = XMMatrixScaling(m_scale.x, m_scale.y, m_scale.z);
        XMMATRIX translation = XMMatrixTranslation(m_translation.x, m_translation.y, m_translation.z);
        transform = (scale * rotation) * translation;

        XMVECTOR right = XMLoadFloat3(&m_right);
        right = right_mat.r[0];
        right = XMVector3TransformNormal(right, rotation);
        XMStoreFloat3(&m_right, right);

        XMVECTOR up = XMLoadFloat3(&m_up);
        up = up_mat.r[1];
        up = XMVector3TransformNormal(up, rotation);
        XMStoreFloat3(&m_up, up);

        XMVECTOR forward = XMLoadFloat3(&m_forward);
        forward = forward_mat.r[2];
        forward = XMVector3TransformNormal(forward, rotation);
        XMStoreFloat3(&m_forward, forward);

        XMStoreFloat4x4(&m_world, transform);
        XMStoreFloat4x4(&m_transposed_world, XMMatrixTranspose(transform));
    }

    DirectX::XMFLOAT3 m_right;
    DirectX::XMFLOAT3 m_up;
    DirectX::XMFLOAT3 m_forward;
    DirectX::XMFLOAT3 m_rotation;
    DirectX::XMFLOAT3 m_translation;
    DirectX::XMFLOAT3 m_scale;
    DirectX::XMFLOAT4X4 m_world;
    DirectX::XMFLOAT4X4 m_transposed_world;
};
