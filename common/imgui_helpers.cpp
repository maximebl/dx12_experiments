#include "imgui_helpers.h"
#include "imgui.h"

static ID3D12DescriptorHeap *imgui_srv_heap = nullptr;

ImGuiContext *imgui_init(ComPtr<ID3D12Device> device, int num_back_buffers, DXGI_FORMAT back_buffer_format)
{
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_hwnd);

    D3D12_DESCRIPTOR_HEAP_DESC imgui_heap_desc;
    imgui_heap_desc.NodeMask = 0;
    imgui_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    imgui_heap_desc.NumDescriptors = 1;
    imgui_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check_hr(device->CreateDescriptorHeap(&imgui_heap_desc, IID_PPV_ARGS(&imgui_srv_heap)));
    NAME_D3D12_OBJECT(imgui_srv_heap);

    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_handle = imgui_srv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu_handle = imgui_srv_heap->GetGPUDescriptorHandleForHeapStart();

    ImGui_ImplDX12_Init(device.Get(),
                        num_back_buffers,
                        back_buffer_format,
                        imgui_srv_heap,
                        srv_cpu_handle,
                        srv_gpu_handle);

    return ctx;
}

void imgui_new_frame()
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void imgui_render(ID3D12GraphicsCommandList *cmd_list)
{
    cmd_list->SetDescriptorHeaps(1, &imgui_srv_heap);
    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd_list);
}

void imgui_shutdown()
{
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    if (ImGui::GetCurrentContext())
    {
        ImGui::DestroyContext();
    }
    safe_release(imgui_srv_heap);
}

void imgui_gpu_memory(IDXGIAdapter4 *adapter)
{
    UINT64 local_usage = 0;
    UINT64 local_budget = 0;
    UINT64 nonlocal_usage = 0;
    UINT64 nonlocal_budget = 0;

    // Ideally we would query for the video memory info right after resetting the command list.
    DXGI_QUERY_VIDEO_MEMORY_INFO mem_info;
    adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mem_info);
    local_usage = mem_info.CurrentUsage;
    local_budget = mem_info.Budget;

    adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &mem_info);
    nonlocal_usage = mem_info.CurrentUsage;
    nonlocal_budget = mem_info.Budget;

    ImGui::Text("Local (video) memory");
    ImGui::Indent(10.f);

    ImGui::Text("Current usage: %u", local_usage);
    ImGui::Text("Budget: %u", local_budget);
    ImGui::ProgressBar((float)local_usage / (float)local_budget, ImVec2(0.f, 0.f));
    ImGui::Unindent(10.f);

    ImGui::Text("Non-local (system) memory");
    ImGui::Indent(10.f);
    ImGui::Text("Current usage: %u", nonlocal_usage);
    ImGui::Text("Budget: %u", nonlocal_budget);
    ImGui::ProgressBar((float)nonlocal_usage / (float)nonlocal_budget, ImVec2(0.f, 0.f));
    ImGui::Unindent(10.f);
    ImGui::Separator();
}

void imgui_mouse_pos()
{
    mouse_pos.x = ImGui::GetMousePos().x;
    mouse_pos.y = ImGui::GetMousePos().y;
    ndc_mouse_pos.x = (2.f * mouse_pos.x / g_hwnd_width) - 1.f;
    ndc_mouse_pos.y = -(2.f * mouse_pos.y / g_hwnd_height) + 1.f;

    ImGui::Columns(3, "mouse_coords");
    ImGui::Separator();
    ImGui::Text("Mouse coords");
    ImGui::NextColumn();
    ImGui::Text("X");
    ImGui::NextColumn();
    ImGui::Text("Y");
    ImGui::NextColumn();
    ImGui::Separator();
    ImGui::Text("Screen");
    ImGui::Text("NDC");
    ImGui::NextColumn();

    ImGui::Text("%f", mouse_pos.x); // screen space x

    ImGui::Text("%f", ndc_mouse_pos.x); // NDC space x
    ImGui::NextColumn();

    ImGui::Text("%f", mouse_pos.y); // screen space y

    ImGui::Text("%f", ndc_mouse_pos.y); // NDC space y

    ImGui::Columns(1);
    ImGui::Separator();
}

bool is_hovering_window()
{
    return ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsAnyItemHovered() || ImGui::IsAnyItemFocused() || ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow);
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void imgui_wndproc(UINT msg, WPARAM wParam, LPARAM lParam)
{
    ImGui_ImplWin32_WndProcHandler(g_hwnd, msg, wParam, lParam);
}
