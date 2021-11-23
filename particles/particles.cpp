#include "common.h"
#include "gpu_interface.h"
#include "particles_graphics.h"
#include "ui_context.h"
#include "imgui_helpers.h"

extern "C" __declspec(dllexport) bool update_and_render();
extern "C" __declspec(dllexport) void resize(int, int);
extern "C" __declspec(dllexport) void on_mousemove();
extern "C" __declspec(dllexport) void wndproc(UINT msg, WPARAM wParam, LPARAM lParam);
extern "C" __declspec(dllexport) void cleanup();
extern "C" __declspec(dllexport) bool initialize(game_memory);

memory_arena app_arena;
particles_graphics *graphics;

bool test = true;
int *new_int_base = nullptr;

extern "C" __declspec(dllexport) bool update_and_render()
{
    // Update camera
    graphics->update_current_camera();

    // Update UI
    imgui_new_frame();
    ui_context::draw(graphics);

    // Render the scene
    graphics->render();

    if (test)
    {
        char buf[200];
        sprintf_s(buf, 200, "\n Particles.cpp reloaded again, state: %d \n", *new_int_base);
        OutputDebugStringA(buf);

        *new_int_base = 55;

        test = false;
    }
    return true;
}

extern "C" __declspec(dllexport) void resize(int width, int height)
{
    graphics->resize(width, height);
    return;
}

extern "C" __declspec(dllexport) void on_mousemove()
{
    static ImVec2 last_mouse_pos = {};

    ImVec2 current_mouse_pos = ImGui::GetMousePos();
    if (current_mouse_pos.x >= FLT_MAX || current_mouse_pos.x <= -FLT_MAX)
    {
        return;
    }

    if (current_mouse_pos.y >= FLT_MAX || current_mouse_pos.y <= -FLT_MAX)
    {
        return;
    }

    if (last_mouse_pos.x >= FLT_MAX || last_mouse_pos.x <= -FLT_MAX)
    {
        last_mouse_pos = ImGui::GetMousePos();
        return;
    }

    if (last_mouse_pos.y >= FLT_MAX || last_mouse_pos.y <= -FLT_MAX)
    {
        last_mouse_pos = ImGui::GetMousePos();
        return;
    }

    if (!is_hovering_window())
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            graphics->update_camera_yaw_pitch(last_mouse_pos);
        }
    }
    last_mouse_pos = ImGui::GetMousePos();
    return;
}

extern "C" __declspec(dllexport) void cleanup()
{
    graphics->m_gpu.flush_graphics_queue();
    imgui_shutdown();
    graphics->~particles_graphics();
#ifdef _DEBUG
    ComPtr<IDXGIDebug1> dxgiDebug;
    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
    {
        dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_FLAGS(DXGI_DEBUG_RLO_DETAIL |
                                                                          DXGI_DEBUG_RLO_IGNORE_INTERNAL));
    }
#endif
    return;
}

extern "C" __declspec(dllexport) bool initialize(game_memory memory)
{
    init_arena(&app_arena, memory.total_size, memory.memory);
    new_int_base = push_struct<int>(&app_arena);
    BYTE *end = (BYTE *)memory.memory + (memory.total_size - sizeof(int));
    *end = 77;

    graphics = push_struct<particles_graphics>(&app_arena);
    graphics->initialize();

    return true;
}

extern "C" __declspec(dllexport) void wndproc(UINT msg, WPARAM wParam, LPARAM lParam)
{
    imgui_wndproc(msg, wParam, lParam);
}
