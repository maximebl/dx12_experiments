#pragma once
#include "common.h"
#include <DirectXMath.h>

COMMON_API DirectX::XMFLOAT4X4 Identity4x4();
COMMON_API DirectX::XMFLOAT3X3 Identity3x3();
COMMON_API float random_float(float min, float max);
COMMON_API float gaussian_random_float(float min, float max);
COMMON_API float plane_dot(DirectX::XMVECTOR plane, DirectX::XMVECTOR point);
COMMON_API bool is_aabb_visible(DirectX::XMVECTOR planes[6], DirectX::XMVECTOR center, DirectX::XMVECTOR extents);
COMMON_API float fract(float value);
