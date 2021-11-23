#include "math_helpers.h"
#include <random>

using namespace DirectX;

DirectX::XMFLOAT4X4 Identity4x4()
{
    DirectX::XMFLOAT4X4 I(
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);

    return I;
}

DirectX::XMFLOAT3X3 Identity3x3()
{
    DirectX::XMFLOAT3X3 I(
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f);

    return I;
}

// TODO: This function is very slow. Need to find a better RNG.
float random_float(float min, float max)
{
    static std::random_device rd{};
    static std::mt19937 gen{rd()};

    std::uniform_real_distribution<float> distr{min, max};
    return distr(gen);
}

float gaussian_random_float(float min, float max)
{
    static std::random_device rd{};
    static std::default_random_engine gen{rd()};

    std::normal_distribution<float> distr{min, max};
    return distr(gen);
}

float plane_dot(XMVECTOR plane, XMVECTOR point)
{
    return XMVectorGetX(XMVector3Dot(plane, point)) + XMVectorGetW(plane);
}

bool is_aabb_visible(XMVECTOR planes[6], XMVECTOR center, XMVECTOR extents)
{
    for (size_t i = 0; i < 6; i++)
    {
        XMVECTOR plane = planes[i];
        float aabb_radius = fabs(XMVectorGetX(plane) * XMVectorGetX(extents)) + fabs(XMVectorGetY(plane) * XMVectorGetY(extents)) + fabs(XMVectorGetZ(plane) * XMVectorGetZ(extents));
        float to_center = plane_dot(plane, center);
        float neg_rg = -aabb_radius;
        if (to_center <= -aabb_radius)
            return false;
    }
    return true;
}

inline float fract(float value)
{
    float tmp;
    return modf(value, &tmp);
}
