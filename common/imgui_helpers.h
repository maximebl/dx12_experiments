#pragma once
#include "common.h"
#include <DirectXMath.h>
#include "gpu_interface.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

COMMON_API DirectX::XMFLOAT2 mouse_pos;
COMMON_API DirectX::XMFLOAT2 ndc_mouse_pos;

COMMON_API ImGuiContext *imgui_init(ComPtr<ID3D12Device> device, int num_back_buffers, DXGI_FORMAT back_buffer_format);
COMMON_API void imgui_render(ID3D12GraphicsCommandList *cmd_list);
COMMON_API void imgui_shutdown();
COMMON_API void imgui_mouse_pos();
COMMON_API void imgui_gpu_memory(IDXGIAdapter4 *adapter);
COMMON_API bool is_hovering_window();
COMMON_API void imgui_new_frame();
COMMON_API void imgui_wndproc(UINT msg, WPARAM wParam, LPARAM lParam);
