#include "..\common\gpu_interface.h"
#include "particles_graphics.h"
#include "imgui_helpers.h"
#include "math_helpers.h"
#include "GeometryGenerator.h"
#include <pix3.h>

using namespace DirectX;

particles_graphics::~particles_graphics()
{
    for (size_t i = 0; i < PSOs_MAX; i++)
    {
        safe_release(m_PSOs[i]);
    }
}

void particles_graphics::initialize()
{
    check_hr(SetThreadDescription(GetCurrentThread(), L"main thread"));
    create_shadowmap_thread_contexts();
    create_compute_thread_contexts();

    // Settings default values.
    show_debug_camera = false;
    do_tonemapping = true;
    use_pcf_point_shadows = true;
    use_pcf_max_quality = false;
    is_drawing_bounds = false;
    draw_billboards = true;
    exposure = 1.f;
    clip_delta = 0.5f;
    specular_shading = 0;
    brdf_id = 0;

    // Initialize core objects.
    m_gpu.init_core(back_buffer_format);
    m_gpu.init_frame_resources(shadow_maps_descriptors +
                               particle_buffers_descriptors +
                               particle_lights_buffers_descriptors +
                               bounds_buffer_descriptors +
                               simulation_commands_buffer_descriptors +
                               draw_commands_buffer_descriptors);

    // Init ImGui.
    m_ctx = imgui_init(m_gpu.device, m_gpu.NUM_BACK_BUFFERS, back_buffer_format);
    ImGui::SetCurrentContext(m_ctx);

    // Create root signatures.
    m_graphics_rootsig = create_graphics_rootsig();
    NAME_D3D12_OBJECT(m_graphics_rootsig);
    m_compute_rootsig = create_compute_rootsig();
    NAME_D3D12_OBJECT(m_compute_rootsig);

    // Initialize cameras.
    m_cameras[main_camera] = camera(g_aspect_ratio, transform({0.f, 0.f, -10.f}));
    m_cameras[debug_camera] = camera(g_aspect_ratio, transform({0.f, 0.f, -10.f}), 1.f, 300.f);
    update_current_camera();

    create_render_targets();
    create_depth_target();
    create_PSOs();
    create_samplers();
    create_gbuffers();
    create_spotlight_shadowmaps();
    create_pointlight_shadowmaps();

    // Create command objects per frame, per thread context.
    for (UINT32 i = 0; i < gpu_interface::NUM_BACK_BUFFERS; i++)
    {
        // Staging command lists.
        check_hr(m_gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&staging_cmdallocs[i])));
        check_hr(m_gpu.device->CreateCommandList(gpu_interface::DEFAULT_NODE, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 staging_cmdallocs[i].Get(),
                                                 nullptr, IID_PPV_ARGS(&staging_cmdlists[i])));
        check_hr(staging_cmdlists[i]->Close());
        NAME_D3D12_OBJECT_INDEXED(staging_cmdlists[i], i);
        NAME_D3D12_OBJECT_INDEXED(staging_cmdallocs[i], i);

        // Shadow pass command lists.
        for (int j = 0; j < G_NUM_SHADOW_THREADS; j++)
        {
            check_hr(m_gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&shadow_cmdallocs[i][j])));
            check_hr(m_gpu.device->CreateCommandList(gpu_interface::DEFAULT_NODE, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     shadow_cmdallocs[i][j].Get(),
                                                     nullptr, IID_PPV_ARGS(&shadow_cmdlists[i][j])));
            check_hr(shadow_cmdlists[i][j]->Close());
            NAME_D3D12_OBJECT_INDEXED(shadow_cmdlists[i][j], j);
            NAME_D3D12_OBJECT_INDEXED(shadow_cmdallocs[i][j], j);
        }

        // Compute pass command lists.
        for (int j = 0; j < G_NUM_COMPUTE_THREADS; j++)
        {
            check_hr(m_gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&compute_cmdallocs[i][j])));
            check_hr(m_gpu.device->CreateCommandList(gpu_interface::DEFAULT_NODE, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                     compute_cmdallocs[i][j].Get(),
                                                     nullptr, IID_PPV_ARGS(&compute_cmdlists[i][j])));
            check_hr(compute_cmdlists[i][j]->Close());
            NAME_D3D12_OBJECT_INDEXED(compute_cmdlists[i][j], j);
            NAME_D3D12_OBJECT_INDEXED(compute_cmdallocs[i][j], j);
        }
    }

    // Graphics work preamble.
    ComPtr<ID3D12GraphicsCommandList> cmd_list = m_gpu.get_frame_resource()->cmd_list;
    ComPtr<ID3D12CommandAllocator> cmd_alloc = m_gpu.get_frame_resource()->cmd_alloc;
    check_hr(cmd_alloc->Reset());
    check_hr(cmd_list->Reset(cmd_alloc.Get(), nullptr));

    create_buffers(cmd_list);
    create_render_objects(cmd_list);

    std::vector<ComPtr<ID3D12Resource>> resources_to_transition = m_render_objects["sponza"].resources(all_textures);
    resources_to_transition.push_back(m_particle_lights_sb.default_resource);
    resources_to_transition.push_back(m_spotlights_sb.default_resource);
    m_gpu.transition(D3D12_RESOURCE_STATE_COPY_DEST,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                     resources_to_transition,
                     cmd_list,
                     D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                     D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
    m_gpu.transition(D3D12_RESOURCE_STATE_COPY_DEST,
                     D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                     {m_object_cb_vs.default_resource,
                      m_pass_cb.default_resource,
                      m_render_object_cb_ps.default_resource,
                      m_volume_light_cb_ps.default_resource,
                      m_shadowcasters_transforms},
                     cmd_list,
                     D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                     D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);

    create_camera_debug_vertices(cmd_list);
    create_camera_debug_indices(cmd_list);

    create_particle_systems_data(cmd_list);
    create_particle_simulation_commands(cmd_list);
    create_particle_draw_commands(cmd_list);

    create_bounds(cmd_list);
    create_bounds_calculations_commands(cmd_list);
    create_bounds_draw_commands(cmd_list);

    create_point_shadows_draw_commands(cmd_list);

    m_gpu.transition(D3D12_RESOURCE_STATE_COPY_DEST,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                     resources_to_transition,
                     cmd_list,
                     D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                     D3D12_RESOURCE_BARRIER_FLAG_END_ONLY);
    m_gpu.transition(D3D12_RESOURCE_STATE_COPY_DEST,
                     D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                     {m_object_cb_vs.default_resource,
                      m_pass_cb.default_resource,
                      m_render_object_cb_ps.default_resource,
                      m_volume_light_cb_ps.default_resource,
                      m_shadowcasters_transforms},
                     cmd_list,
                     D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                     D3D12_RESOURCE_BARRIER_FLAG_END_ONLY);

    // Execute initialization.
    check_hr(cmd_list->Close());
    ID3D12CommandList *cmd_lists[] = {cmd_list.Get()};
    m_gpu.graphics_cmd_queue->ExecuteCommandLists(_countof(cmd_lists), cmd_lists);
    m_gpu.flush_graphics_queue();

    // Compute work preamble.
    ComPtr<ID3D12GraphicsCommandList> compute_cmdlist = compute_cmdlists[0][0];
    ComPtr<ID3D12CommandAllocator> compute_cmdalloc = compute_cmdallocs[0][0];
    check_hr(compute_cmdalloc->Reset());
    check_hr(compute_cmdlist->Reset(compute_cmdalloc.Get(), nullptr));
    m_gpu.reset_staging_descriptors();
    m_gpu.set_staging_heaps(compute_cmdlist);
    compute_cmdlist->SetComputeRootSignature(m_compute_rootsig.Get());

    // Create textures required for image-based lighting.
    create_ibl_textures(compute_cmdlist);

    // Prepare to execute and flush the compute queue.
    check_hr(m_gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(compute_fence.GetAddressOf())));
    UINT64 cfence_val = 1;
    HANDLE compute_flush_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    check_hr(compute_fence->SetEventOnCompletion(cfence_val, compute_flush_event));

    // Execute compute.
    check_hr(compute_cmdlist->Close());
    m_gpu.compute_cmd_queue->ExecuteCommandLists(1, (ID3D12CommandList *const *)compute_cmdlist.GetAddressOf());
    check_hr(m_gpu.compute_cmd_queue->Signal(compute_fence.Get(), cfence_val));

    // Flush compute.
    WaitForSingleObject(compute_flush_event, INFINITE);
    CloseHandle(compute_flush_event);

    // Transition IBL textures to pixel shader resources.
    // This transition must be done on the graphics queue.
    ComPtr<ID3D12GraphicsCommandList> post_compute_cmdlist = m_gpu.get_frame_resource()->cmd_list;
    ComPtr<ID3D12CommandAllocator> post_compute_cmdalloc = m_gpu.get_frame_resource()->cmd_alloc;
    check_hr(post_compute_cmdalloc->Reset());
    check_hr(post_compute_cmdlist->Reset(post_compute_cmdalloc.Get(), nullptr));

    // Transition the specular irradiance cube map mip chain (except mip0) to pixel shader resource.
    std::vector<D3D12_RESOURCE_BARRIER> mipchain_to_read_state;
    D3D12_RESOURCE_DESC specular_irradiance_desc = m_specular_irradiance_tex.default_resource->GetDesc();
    mipchain_to_read_state.reserve(specular_irradiance_desc.DepthOrArraySize * specular_irradiance_desc.MipLevels);
    for (int mip_level = 1; mip_level < specular_irradiance_desc.MipLevels - 1; mip_level++)
    {
        for (UINT array_slice = 0; array_slice < specular_irradiance_desc.DepthOrArraySize; array_slice++)
        {
            UINT cube_face_mip_subresource = D3D12CalcSubresource(mip_level, array_slice, 0,
                                                                  specular_irradiance_desc.MipLevels, specular_irradiance_desc.DepthOrArraySize);
            mipchain_to_read_state.push_back(m_gpu.transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                              {m_specular_irradiance_tex.default_resource},
                                                              nullptr,
                                                              cube_face_mip_subresource)
                                                 .front());
        }
    }

    // Transition Mip0 of the specular irradiance cube map to pixel shader resource.
    for (UINT array_slice = 0; array_slice < specular_irradiance_desc.DepthOrArraySize; array_slice++)
    {
        UINT cube_face_mip_subresource = D3D12CalcSubresource(0, array_slice, 0,
                                                              specular_irradiance_desc.MipLevels, specular_irradiance_desc.DepthOrArraySize);
        mipchain_to_read_state.push_back(m_gpu.transition(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                          {m_specular_irradiance_tex.default_resource},
                                                          nullptr,
                                                          cube_face_mip_subresource)
                                             .front());
    }

    m_gpu.transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                     {m_diffuse_irradiance_tex.default_resource,
                      m_specular_brdf_lut.default_resource},
                     post_compute_cmdlist);
    post_compute_cmdlist->ResourceBarrier((UINT)mipchain_to_read_state.size(), mipchain_to_read_state.data());

    // Execute transitions.
    check_hr(post_compute_cmdlist->Close());
    ID3D12CommandList *post_compute_cmdlists[] = {post_compute_cmdlist.Get()};
    m_gpu.graphics_cmd_queue->ExecuteCommandLists(_countof(post_compute_cmdlists), post_compute_cmdlists);
    m_gpu.flush_graphics_queue();
}

void particles_graphics::resize(int width, int height)
{
    m_gpu.flush_graphics_queue();
    ImGui_ImplDX12_InvalidateDeviceObjects();
    m_gpu.resize(width, height, m_render_targets, _countof(m_render_targets));
    m_gpu.next_frame();

    ImGui_ImplDX12_CreateDeviceObjects();

    for (size_t i = 0; i < _countof(m_cameras); i++)
    {
        m_cameras[i].calc_projection();
    }
}

void particles_graphics::render()
{
    m_deltatime = (float)g_cpu_timer.tick();
    gpu_interface::frame_resource *frame_resource = m_gpu.get_frame_resource();
    PIXBeginEvent(0, "CPU render(%d)", frame_resource->fence_value); // cpu render.

    m_gpu.reset_staging_descriptors();

    // Compute pass.
    for (int i = 0; i < G_NUM_COMPUTE_THREADS; i++)
    {
        // Reset compute command objects.
        check_hr(compute_cmdallocs[m_gpu.frame_index][i]->Reset());
        check_hr(compute_cmdlists[m_gpu.frame_index][i]->Reset(compute_cmdallocs[m_gpu.frame_index][i].Get(),
                                                               m_PSOs[particle_sim_PSO]));
        // Tell each compute worker thread to begin the simulation pass.
        SetEvent(begin_compute_events[i]);
    }

    // Execute the compute work.
    // Wait for the previous frame's compute workers to be done.
    WaitForMultipleObjects(G_NUM_COMPUTE_THREADS, end_compute_events, true, INFINITE);

    ComPtr<ID3D12GraphicsCommandList> compute_cmdlist = compute_cmdlists[m_gpu.frame_index][0]; // 0: We only use one compute thread for now.
    check_hr(compute_cmdlist->Close());
    size_t frame_fence_value = frame_resource->fence_value;
    PIXBeginEvent(m_gpu.compute_cmd_queue.Get(), 0, "compute_cmd_queue executing: %d", frame_fence_value);
    m_gpu.compute_cmd_queue->ExecuteCommandLists(1, (ID3D12CommandList *const *)compute_cmdlist.GetAddressOf());
    m_gpu.compute_cmd_queue->Signal(compute_fence.Get(), frame_fence_value);
    // Wait for the graphics work to finish this frame before executing compute again.
    // TODO: This is missing a lot of opportunity for doing compute work asynchronously.
    m_gpu.compute_cmd_queue->Wait(m_gpu.fence.Get(), frame_fence_value);
    PIXEndEvent(m_gpu.compute_cmd_queue.Get());
    // Wait for the compute queue to finish.
    m_gpu.graphics_cmd_queue->Wait(compute_fence.Get(), frame_fence_value);

    // Graphics work preamble.
    ComPtr<ID3D12CommandAllocator> cmd_alloc = frame_resource->cmd_alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd_list = frame_resource->cmd_list;
    check_hr(cmd_alloc->Reset());
    check_hr(cmd_list->Reset(cmd_alloc.Get(), nullptr));

    cmd_list->SetGraphicsRootSignature(m_graphics_rootsig.Get());
    m_gpu.set_staging_heaps(cmd_list);

    // Point shadows pass.
    point_shadows_pass(cmd_list, frame_resource);

    // Staging pass.
    ComPtr<ID3D12CommandAllocator> staging_cmdalloc = staging_cmdallocs[m_gpu.frame_index];
    ComPtr<ID3D12GraphicsCommandList> staging_cmdlist = staging_cmdlists[m_gpu.frame_index];
    check_hr(staging_cmdalloc->Reset());
    check_hr(staging_cmdlist->Reset(staging_cmdalloc.Get(), nullptr));
    m_gpu.transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                     D3D12_RESOURCE_STATE_DEPTH_WRITE,
                     {m_spotlight_shadowmaps.default_resource},
                     staging_cmdlist);
    m_gpu.transition(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                     D3D12_RESOURCE_STATE_COPY_DEST,
                     {m_pass_cb.default_resource},
                     staging_cmdlist);
    camera *current_cam = &m_cameras[selected_cam];
    staging_pass(staging_cmdlist, current_cam);
    check_hr(staging_cmdlist->Close());
    ID3D12CommandList *staging_cmdlists[] = {staging_cmdlist.Get()};
    m_gpu.graphics_cmd_queue->ExecuteCommandLists(_countof(staging_cmdlists), staging_cmdlists);

    // Tell each shadow map worker thread to begin the shadow pass.
    for (int i = 0; i < G_NUM_SHADOW_THREADS; i++)
    {
        // Reset shadow map command objects.
        check_hr(shadow_cmdallocs[m_gpu.frame_index][i]->Reset());
        check_hr(shadow_cmdlists[m_gpu.frame_index][i]->Reset(shadow_cmdallocs[m_gpu.frame_index][i].Get(),
                                                              m_PSOs[shadow_pass_PSO]));
        SetEvent(begin_shadowpass_events[i]);
    }

    // Geometry pass.
    m_gpu.transition(D3D12_RESOURCE_STATE_COPY_DEST,
                     D3D12_RESOURCE_STATE_COMMON,
                     {m_particle_lights_sb.default_resource,
                      m_spotlights_sb.default_resource},
                     cmd_list);
    m_gpu.transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                     D3D12_RESOURCE_STATE_RENDER_TARGET,
                     {m_gbuffer0.rt_default_resource,
                      m_gbuffer1.rt_default_resource,
                      m_gbuffer2.rt_default_resource},
                     cmd_list);
    m_gpu.transition(D3D12_RESOURCE_STATE_PRESENT,
                     D3D12_RESOURCE_STATE_DEPTH_WRITE,
                     {depthtarget_default},
                     cmd_list);
    draw_geometry_pass(cmd_list, frame_resource, current_cam);

    // Lighting pass.
    m_gpu.transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                     D3D12_RESOURCE_STATE_RENDER_TARGET,
                     {m_render_targets[m_gpu.frame_index].rt_default_resource},
                     cmd_list);
    m_gpu.transition(D3D12_RESOURCE_STATE_RENDER_TARGET,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                     {m_gbuffer0.rt_default_resource,
                      m_gbuffer1.rt_default_resource,
                      m_gbuffer2.rt_default_resource},
                     cmd_list);
    m_gpu.transition(D3D12_RESOURCE_STATE_DEPTH_WRITE,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                     {m_spotlight_shadowmaps.default_resource,
                      depthtarget_default,
                      m_pointlight_shadowmaps.default_resource},
                     cmd_list);
    draw_lighting_pass(cmd_list, frame_resource);

    // Draw sky.
    draw_sky(cmd_list, frame_resource);

    // Draw volume lights.
    draw_volume_lights(cmd_list, m_object_cb_vs, m_volume_light_cb_ps, volume_lights, _countof(volume_lights), current_cam);

    // Draw particle systems.
    m_gpu.transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                     {particle_output_default},
                     cmd_list);
    m_gpu.transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                     D3D12_RESOURCE_STATE_DEPTH_READ,
                     {depthtarget_default},
                     cmd_list);
    draw_particle_systems(cmd_list, frame_resource);

    // Draw the bounding boxes visualization.
    if (is_drawing_bounds)
    {
        draw_bounding_boxes(cmd_list);
    }

    // Debug color pass.
    if (show_debug_camera)
    {
        draw_debug_objects(cmd_list);
    }

    // Post processing.
    m_gpu.transition(D3D12_RESOURCE_STATE_PRESENT,
                     D3D12_RESOURCE_STATE_RENDER_TARGET,
                     {frame_resource->back_buffer},
                     cmd_list);
    m_gpu.transition(D3D12_RESOURCE_STATE_RENDER_TARGET,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                     {m_render_targets[m_gpu.frame_index].rt_default_resource},
                     cmd_list);
    post_process(cmd_list, frame_resource);

    // Render UI.
    m_gpu.timer_start(cmd_list, "Render ImGui");
    imgui_render(cmd_list.Get());
    m_gpu.timer_stop(cmd_list, "Render ImGui");

    // Wait for the shadow map workers to be done.
    WaitForMultipleObjects(G_NUM_SHADOW_THREADS, end_shadowpass_events, true, INFINITE);

    // Execute.
    m_gpu.transition(D3D12_RESOURCE_STATE_RENDER_TARGET,
                     D3D12_RESOURCE_STATE_PRESENT,
                     {frame_resource->back_buffer},
                     cmd_list);
    m_gpu.transition(D3D12_RESOURCE_STATE_DEPTH_READ,
                     D3D12_RESOURCE_STATE_PRESENT,
                     {depthtarget_default},
                     cmd_list);
    check_hr(cmd_list->Close());
    ID3D12CommandList *cmd_lists[] = {cmd_list.Get()};
    PIXBeginEvent(m_gpu.graphics_cmd_queue.Get(), 0, "graphics_cmd_queue executing: %d", frame_fence_value);
    m_gpu.graphics_cmd_queue->ExecuteCommandLists(_countof(cmd_lists), cmd_lists);
    PIXEndEvent(m_gpu.graphics_cmd_queue.Get());

    PIXEndEvent(); // cpu render.

    // Move to next frame.
    m_gpu.next_frame();
}

void particles_graphics::staging_pass(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                                      camera *cam)
{
    // Update pass data.
    XMVECTOR eye_pos = XMLoadFloat3(&cam->m_transform.m_translation);
    XMMATRIX inv_view = XMLoadFloat4x4(&cam->m_inv_view);
    XMMATRIX view = XMLoadFloat4x4(&cam->m_view);
    XMMATRIX proj = XMLoadFloat4x4(&cam->m_proj);
    XMMATRIX world_to_screen = inv_view * proj;
    XMMATRIX screen_to_world = XMMatrixInverse(&XMMatrixDeterminant(world_to_screen), world_to_screen);
    pass_data pass = {};

    switch (selected_cam)
    {
    case main_camera:
        pass.frustum_planes[0] = m_cameras[debug_camera].m_bottom_plane;
        pass.frustum_planes[1] = m_cameras[debug_camera].m_far_plane;
        pass.frustum_planes[2] = m_cameras[debug_camera].m_left_plane;
        pass.frustum_planes[3] = m_cameras[debug_camera].m_near_plane;
        pass.frustum_planes[4] = m_cameras[debug_camera].m_right_plane;
        pass.frustum_planes[5] = m_cameras[debug_camera].m_top_plane;
        break;
    case debug_camera:
        pass.frustum_planes[0] = m_cameras[main_camera].m_bottom_plane;
        pass.frustum_planes[1] = m_cameras[main_camera].m_far_plane;
        pass.frustum_planes[2] = m_cameras[main_camera].m_left_plane;
        pass.frustum_planes[3] = m_cameras[main_camera].m_near_plane;
        pass.frustum_planes[4] = m_cameras[main_camera].m_right_plane;
        pass.frustum_planes[5] = m_cameras[main_camera].m_top_plane;
        break;
    }

    pass.aspect_ratio = g_aspect_ratio;
    pass.delta_time = m_deltatime;
    pass.time = (float)g_cpu_timer.get_current_time();
    XMStoreFloat4x4(&pass.inv_view, XMMatrixTranspose(inv_view));
    XMStoreFloat4x4(&pass.view, XMMatrixTranspose(view));
    XMStoreFloat4x4(&pass.proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&pass.screen_to_world, XMMatrixTranspose(screen_to_world));
    XMStoreFloat3(&pass.eye_pos, eye_pos);
    pass.eye_forward = cam->m_transform.m_forward;
    XMStoreFloat3(&pass.ambient_light, {0.001f, 0.001f, 0.001f});
    XMStoreFloat2(&pass.screen_size, {(float)g_hwnd_width, (float)g_hwnd_height});
    pass.specular_shading = specular_shading;
    pass.brdf_id = brdf_id;
    pass.do_tonemapping = (INT)do_tonemapping;
    pass.use_pcf_point_shadows = (INT)use_pcf_point_shadows;
    pass.use_pcf_max_quality = (INT)use_pcf_max_quality;
    pass.exposure = exposure;
    pass.near_plane = cam->m_near;
    pass.far_plane = cam->m_far;
    pass.indirect_diffuse_brdf = (INT)m_indirect_diffuse_brdf;
    pass.indirect_specular_brdf = (INT)m_indirect_specular_brdf;
    pass.direct_diffuse_brdf = (INT)m_direct_diffuse_brdf;
    pass.direct_specular_brdf = (INT)m_direct_specular_brdf;
    pass.clip_delta = clip_delta;
    m_pass_cb.update(&pass, cmd_list);

    // Clear all shadow maps.
    cmd_list->ClearDepthStencilView(m_spotlight_shadowmaps.dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);
}

void particles_graphics::point_shadows_pass(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                                            gpu_interface::frame_resource *frame)
{
    m_gpu.timer_start(cmd_list, "Draw attractor point lights shadows.");

    // Update point shadow casters.
    int ro_id = 0;
    for (auto &ro_pair : m_render_objects)
    {
        const render_object &ro = ro_pair.second;

        shadow_casters_transforms[ro_id].world = ro.m_transform.m_world;
        XMMATRIX world;
        world = XMLoadFloat4x4(&ro.m_transform.m_world);
        XMStoreFloat4x4(&shadow_casters_transforms[ro_id].world, XMMatrixTranspose(world));
        XMMATRIX view_proj = XMLoadFloat4x4(&m_cameras[main_camera].m_view_proj);
        XMStoreFloat4x4(&shadow_casters_transforms[ro_id].world_view_proj, XMMatrixTranspose(world * view_proj));
        ro_id++;
    }

    UINT8 *dest = frame->m_resources_buffer.allocate(shadow_transforms_cbv_size, shadow_transforms_cbv_size);
    memcpy(dest, shadow_casters_transforms, shadow_transforms_cbv_size);
    size_t offset = dest - frame->m_resources_buffer.m_begin;
    cmd_list->CopyBufferRegion(m_shadowcasters_transforms.Get(), 0,
                               frame->m_resources_buffer.m_upload_resource.Get(), offset,
                               shadow_transforms_cbv_size);

    m_gpu.transition(D3D12_RESOURCE_STATE_COPY_DEST,
                     D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                     {m_pass_cb.default_resource,
                      m_shadowcasters_transforms},
                     cmd_list);
    m_gpu.transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                     D3D12_RESOURCE_STATE_DEPTH_WRITE,
                     {m_pointlight_shadowmaps.default_resource},
                     cmd_list);

    // Set a square scissor rect and viewport.
    D3D12_RECT shadow_rect;
    shadow_rect.left = 0;
    shadow_rect.top = 0;
    shadow_rect.right = (LONG)shadowmap_width;
    shadow_rect.bottom = (LONG)shadowmap_height;

    D3D12_VIEWPORT shadow_vp;
    shadow_vp.TopLeftY = 0;
    shadow_vp.TopLeftX = 0;
    shadow_vp.Width = (float)shadowmap_width;
    shadow_vp.Height = (float)shadowmap_height;
    shadow_vp.MaxDepth = 1.0f;
    shadow_vp.MinDepth = 0.0f;

    cmd_list->RSSetViewports(1, &shadow_vp);
    cmd_list->RSSetScissorRects(1, &shadow_rect);

    // Draw point lights shadow casters.
    cmd_list->SetPipelineState(m_PSOs[cube_shadow_pass_PSO]);
    cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd_list->OMSetRenderTargets(0, nullptr, false, &m_pointlight_shadowmaps.dsv_handle);
    cmd_list->ClearDepthStencilView(m_pointlight_shadowmaps.dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);

    cmd_list->SetGraphicsRootDescriptorTable(12, cbv_gpu_shadowcasters_transforms_base[m_gpu.frame_index]);
    cmd_list->SetGraphicsRootShaderResourceView(14, m_attractors_sb.default_resource->GetGPUVirtualAddress());

    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, CBV, 0, m_pass_cb.cpu_handle);
    m_gpu.set_descriptor_tables(cmd_list);

    cmd_list->ExecuteIndirect(render_point_shadows_cmdsig.Get(), (UINT)num_point_shadow_cmds,
                              render_point_shadows_cmds_default.Get(), 0,
                              nullptr, 0);
    m_gpu.timer_stop(cmd_list, "Draw attractor point lights shadows.");

    // Set back the original viewport and scissor rect.
    cmd_list->RSSetViewports(1, &m_gpu.viewport);
    cmd_list->RSSetScissorRects(1, &m_gpu.scissor_rect);
}

void particles_graphics::draw_geometry_pass(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                                            gpu_interface::frame_resource *frame,
                                            camera *cam)
{
    m_gpu.timer_start(cmd_list, "Geometry pass");

    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, VS, CBV, 1, m_object_cb_vs.cpu_handle);
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, CBV, 0, m_pass_cb.cpu_handle);
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, CBV, 1, m_render_object_cb_ps.cpu_handle);
    frame->sampler_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, sampler, 0, m_samplers[linear_wrap]);
    m_gpu.set_descriptor_tables(cmd_list);

    cmd_list->SetPipelineState(m_PSOs[pbr_simple_PSO]);
    cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Set the gbuffers as the render targets.
    D3D12_CPU_DESCRIPTOR_HANDLE gbuffers_rt_handles[3];
    gbuffers_rt_handles[0] = m_gbuffer0.rtv_handle;
    gbuffers_rt_handles[1] = m_gbuffer1.rtv_handle;
    gbuffers_rt_handles[2] = m_gbuffer2.rtv_handle;
    cmd_list->OMSetRenderTargets(_countof(gbuffers_rt_handles), gbuffers_rt_handles, FALSE,
                                 &depthtarget_dsv_handle);

    // Clear gbuffer render targets.

    const float gbuffer_clear_color[] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (size_t i = 0; i < _countof(gbuffers_rt_handles); i++)
    {
        cmd_list->ClearRenderTargetView(gbuffers_rt_handles[i], gbuffer_clear_color, 0, nullptr);
    }
    cmd_list->ClearDepthStencilView(depthtarget_dsv_handle,
                                    D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                    1.f,         // Depth.
                                    0,           // Stencil.
                                    0, nullptr); // Rects.

    // Draw geometry to gbuffers.
    XMMATRIX cam_viewproj = XMLoadFloat4x4(&cam->m_view_proj);
    std::string timer_name;
    for (auto &ro_pair : m_render_objects)
    {
        timer_name = "Render " + ro_pair.first;
        m_gpu.timer_start(cmd_list, timer_name);
        draw_render_objects(cmd_list, &m_object_cb_vs, &m_render_object_cb_ps, &ro_pair.second, 1, cam_viewproj, true);
        m_gpu.timer_stop(cmd_list, timer_name);
    }

    m_gpu.timer_stop(cmd_list, "Geometry pass");
}

void particles_graphics::draw_lighting_pass(ComPtr<ID3D12GraphicsCommandList> cmd_list, gpu_interface::frame_resource *frame)
{
    m_gpu.timer_start(cmd_list, "Lighting pass");

    cmd_list->SetPipelineState(m_PSOs[lighting_pass_PSO]);
    cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    cmd_list->OMSetRenderTargets(1, &m_render_targets[m_gpu.frame_index].rtv_hdr, FALSE, &depthtarget_dsv_handle);

    // Clear the back buffer RTV and DSV.
    const float clear_color[] = {0.0f, 0.0f, 0.0f, 1.0f};
    cmd_list->ClearRenderTargetView(m_render_targets[m_gpu.frame_index].rtv_hdr, clear_color, 0, nullptr);
    cmd_list->ClearDepthStencilView(m_gpu.dsv_heap->GetCPUDescriptorHandleForHeapStart(),
                                    D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                    1.0f,        // Depth
                                    0,           // Stencil
                                    0, nullptr); // Rects

    // Bind pass data and samplers.
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, GS, CBV, 0, m_pass_cb.cpu_handle);
    frame->sampler_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, sampler, 1, m_samplers[shadow_sampler]);
    frame->sampler_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, sampler, 2, m_samplers[linear_clamp]);

    // Bind the gbuffers.
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, SRV, 0, m_gbuffer0.srv_handle);
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, SRV, 1, m_gbuffer1.srv_handle);
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, SRV, 2, m_gbuffer2.srv_handle);

    // Bind the geometry pass depth as an SRV to reconstruct world space positions from.
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, SRV, 3, depthtarget_srv_handle);

    // Bind light data.
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, SRV, 4, m_particle_lights_sb.srv_cpu_handle);
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, SRV, 5, m_spotlights_sb.srv_cpu_handle);
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, SRV, 6, m_attractors_sb.srv_cpu_handle);
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, SRV, 7, m_diffuse_irradiance_tex.srv_handle);
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, SRV, 8, m_specular_irradiance_tex.srv_handle);
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, SRV, 9, m_specular_brdf_lut.srv_handle);

    // Bind counters.
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, CBV, 1, m_particle_lights_counter.cpu_handle);

    // Bind shadow maps.
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, SRV, 10, m_spotlight_shadowmaps.srv_handle);
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, SRV, 11, m_pointlight_shadowmaps.srv_handle);
    m_gpu.set_descriptor_tables(cmd_list);

    // Draw a triangle over the viewport.
    cmd_list->DrawInstanced(3, 1, 0, 0);
    m_gpu.timer_stop(cmd_list, "Lighting pass");
}

void particles_graphics::draw_sky(ComPtr<ID3D12GraphicsCommandList> cmd_list, gpu_interface::frame_resource *frame)
{
    m_gpu.timer_start(cmd_list, "Draw sky");
    cmd_list->SetPipelineState(m_PSOs[draw_sky_PSO]);

    // Bind the sky environment map.
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, SRV, 0, m_unfiltered_tex.srv_handle);
    m_gpu.set_descriptor_tables(cmd_list);

    cmd_list->DrawInstanced(3, 1, 0, 0);
    m_gpu.timer_stop(cmd_list, "Draw sky");
}

void particles_graphics::draw_particle_systems(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                                               gpu_interface::frame_resource *frame)
{
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, VS, CBV, 0, m_pass_cb.cpu_handle);
    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, SRV, 0, m_fire_sprite.srv_handle);
    m_gpu.set_descriptor_tables(cmd_list);
    cmd_list->RSSetViewports(1, &m_gpu.viewport);
    cmd_list->RSSetScissorRects(1, &m_gpu.scissor_rect);
    cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);

    if (draw_billboards)
    {
        // Draw particle systems as billboards.
        cmd_list->SetPipelineState(m_PSOs[billboards_PSO]);
    }
    else
    {
        // Draw particle systems as points.
        cmd_list->SetPipelineState(m_PSOs[particle_point_draw_PSO]);
    }

    m_gpu.timer_start(cmd_list, "Draw particle systems");
    cmd_list->ExecuteIndirect(particle_draw_cmdsig.Get(), num_particle_systems,
                              particle_drawcmds_filtered_default.Get(), 0,
                              particle_drawcmds_counter_default[m_gpu.frame_index].Get(), 0);
    m_gpu.timer_stop(cmd_list, "Draw particle systems");
}

void particles_graphics::draw_bounding_boxes(ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    m_gpu.timer_start(cmd_list, "Draw bounding boxes");
    cmd_list->SetPipelineState(m_PSOs[draw_bounds_PSO]);
    cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    cmd_list->ExecuteIndirect(bounds_draw_cmdsig.Get(), num_particle_systems,
                              bounds_drawcmds_default.Get(), 0,
                              nullptr, 0);
    m_gpu.timer_stop(cmd_list, "Draw bounding boxes");
}

void particles_graphics::draw_debug_objects(ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    m_gpu.timer_start(cmd_list, "Debug color pass");

    // Update debug camera frustum vertices.
    std::vector<camera::vertex_debug> frustum_vertices = m_cameras[debug_camera].get_debug_frustum_vertices();
    m_debugcam_frustum_vertices.update(frustum_vertices.data());

    // Draw debug camera frustum lines.
    cmd_list->SetPipelineState(m_PSOs[debug_line_PSO]);
    cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);

    D3D12_VERTEX_BUFFER_VIEW debug_cam_vertices_vbv = {};
    debug_cam_vertices_vbv.BufferLocation = m_debugcam_frustum_vertices.m_gpu_va;
    debug_cam_vertices_vbv.SizeInBytes = (UINT)m_debugcam_frustum_vertices.m_aligned_size;
    debug_cam_vertices_vbv.StrideInBytes = (UINT)m_debugcam_frustum_vertices.m_datum_size;

    cmd_list->IASetVertexBuffers(0, 1, &debug_cam_vertices_vbv);
    cmd_list->IASetIndexBuffer(&m_debug_cam_frustum.m_mesh.m_ibv);

    mesh::submesh debug_cam_submesh = m_debug_cam_frustum.m_mesh.m_submeshes[0];
    cmd_list->DrawIndexedInstanced(debug_cam_submesh.index_count, 1,
                                   debug_cam_submesh.start_index_location,
                                   debug_cam_submesh.base_vertex_location, 0);

    // Draw debug camera frustum planes.
    cmd_list->SetPipelineState(m_PSOs[debug_plane_PSO]);
    cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    cmd_list->IASetIndexBuffer(&m_debug_cam_frustum_planes.m_mesh.m_ibv);

    mesh::submesh debug_cam_planes_submesh = m_debug_cam_frustum_planes.m_mesh.m_submeshes[0];
    cmd_list->DrawIndexedInstanced(debug_cam_planes_submesh.index_count, 1,
                                   debug_cam_planes_submesh.start_index_location,
                                   debug_cam_planes_submesh.base_vertex_location, 0);
    m_gpu.timer_stop(cmd_list, "Debug color pass");
}

void particles_graphics::post_process(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                                      gpu_interface::frame_resource *frame)
{
    m_gpu.timer_start(cmd_list, "Postprocess");
    cmd_list->SetPipelineState(m_PSOs[tonemapping_PSO]);
    cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Set the back buffer as the render target.
    cmd_list->OMSetRenderTargets(1, &m_render_targets[m_gpu.frame_index].rtv_backbuffer, FALSE, nullptr);

    frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, SRV, 0, m_render_targets[m_gpu.frame_index].srv_handle);
    m_gpu.set_descriptor_tables(cmd_list);

    // Draw a triangle over the viewport.
    cmd_list->DrawInstanced(3, 1, 0, 0);
    m_gpu.timer_stop(cmd_list, "Postprocess");
}

void particles_graphics::draw_render_objects(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                                             gpu_interface::constant_buffer<object_data_vs> *vs_cb, gpu_interface::constant_buffer<render_object_data_ps> *ps_cb,
                                             const render_object *render_objects, size_t count, XMMATRIX view_proj, bool is_scene_pass)
{
    for (int i = 0; i < count; i++)
    {
        {
            std::vector<D3D12_RESOURCE_BARRIER> barriers_to_copydest;
            D3D12_RESOURCE_BARRIER transition;
            transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            transition.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            transition.Transition.StateBefore = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

            transition.Transition.pResource = vs_cb->default_resource.Get();
            barriers_to_copydest.push_back(transition);

            if (is_scene_pass)
            {
                transition.Transition.pResource = ps_cb->default_resource.Get();
                barriers_to_copydest.push_back(transition);
            }

            cmd_list->ResourceBarrier((UINT)barriers_to_copydest.size(), barriers_to_copydest.data());
        }

        // Update per-object vertex constant buffer.
        const render_object *ro = &render_objects[i];
        XMMATRIX world;
        object_data_vs obj_data_vs = {};
        world = XMLoadFloat4x4(&ro->m_transform.m_world);
        XMStoreFloat4x4(&obj_data_vs.world, XMMatrixTranspose(world));

        XMMATRIX mvp = world * view_proj;
        XMStoreFloat4x4(&obj_data_vs.world_view_proj, XMMatrixTranspose(mvp));

        // Update per-object pixel constant buffer.
        render_object_data_ps obj_data_ps = {};
        obj_data_ps.roughness_metalness = ro->m_roughness_metalness;
        obj_data_ps.color = ro->m_color;
        obj_data_ps.object_id = i;

        mtx.lock();
        vs_cb->update(&obj_data_vs, cmd_list);
        if (is_scene_pass)
        {
            ps_cb->update(&obj_data_ps, cmd_list);
        }
        mtx.unlock();

        {
            // Transition to proper state for drawing.
            std::vector<D3D12_RESOURCE_BARRIER> barriers_to_read;
            D3D12_RESOURCE_BARRIER transition;
            transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            transition.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            transition.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            transition.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;

            transition.Transition.pResource = vs_cb->default_resource.Get();
            barriers_to_read.push_back(transition);

            if (is_scene_pass)
            {
                transition.Transition.pResource = ps_cb->default_resource.Get();
                barriers_to_read.push_back(transition);
            }
            cmd_list->ResourceBarrier((UINT)barriers_to_read.size(), barriers_to_read.data());
        }

        // Set vertex and index buffers.
        const mesh *current_mesh = &ro->m_mesh;
        cmd_list->IASetVertexBuffers(0, 1, &current_mesh->m_vbv);
        cmd_list->IASetIndexBuffer(&current_mesh->m_ibv);

        // Draw each submesh.
        for (const mesh::submesh &submesh : ro->m_mesh.m_submeshes)
        {
            gpu_interface::frame_resource *frame = m_gpu.get_frame_resource();

            if (is_scene_pass)
            {
                frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, SRV, 0, submesh.SRVs[diffuse]);
                frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, SRV, 1, submesh.SRVs[normal]);
                frame->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, SRV, 2, submesh.SRVs[metallic_roughness]);
                m_gpu.set_descriptor_tables(cmd_list);
            }
            cmd_list->DrawIndexedInstanced(submesh.index_count, 1,
                                           submesh.start_index_location,
                                           submesh.base_vertex_location, 0);
        }
    }
}

void particles_graphics::draw_volume_lights(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                                            gpu_interface::constant_buffer<object_data_vs> vs_cb, gpu_interface::constant_buffer<volume_light_data_ps> ps_cb,
                                            const volume_light *volume_lights, size_t count,
                                            const camera *current_cam)
{
    m_gpu.timer_start(cmd_list, "Draw volume lights");
    cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_gpu.get_frame_resource()->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, PS, CBV, 2, m_volume_light_cb_ps.cpu_handle);
    m_gpu.set_descriptor_tables(cmd_list);

    for (int i = 0; i < count; i++)
    {
        m_gpu.transition(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                         D3D12_RESOURCE_STATE_COPY_DEST,
                         {vs_cb.default_resource,
                          ps_cb.default_resource},
                         cmd_list);

        const volume_light *vl = &volume_lights[i];

        switch (vl->m_blend_mode)
        {
        case alpha_transparency:
            cmd_list->SetPipelineState(m_PSOs[volume_light_alpha_transparency_PSO]);
            break;
        case additive_transparency:
            cmd_list->SetPipelineState(m_PSOs[volume_light_additive_transparency_PSO]);
            break;
        default:
            cmd_list->SetPipelineState(m_PSOs[volume_light_additive_transparency_PSO]);
            break;
        }

        // Update per-object vertex constant buffer.
        XMMATRIX world = XMLoadFloat4x4(&vl->m_transform.m_world);
        XMMATRIX view_proj = XMLoadFloat4x4(&current_cam->m_view_proj);
        XMMATRIX mvp = world * view_proj;

        object_data_vs obj_data_vs = {};
        XMStoreFloat4x4(&obj_data_vs.world_view_proj, XMMatrixTranspose(mvp));
        XMStoreFloat4x4(&obj_data_vs.world, XMMatrixTranspose(world));

        // Update per-object pixel constant buffer.
        XMMATRIX object_to_world = XMLoadFloat4x4(&vl->m_transform.m_world);
        XMVECTOR eye_pos = XMLoadFloat3(&current_cam->m_transform.m_translation);
        XMVECTOR eye_forward = XMLoadFloat3(&current_cam->m_transform.m_forward);
        XMMATRIX world_to_object = XMMatrixInverse(&XMMatrixDeterminant(object_to_world), object_to_world);

        volume_light_data_ps volume_light_data = {};
        volume_light_data.radius = vl->m_radius;
        volume_light_data.color = vl->m_color;
        XMStoreFloat4x4(&volume_light_data.world_to_object, XMMatrixTranspose(world_to_object));
        XMStoreFloat3(&volume_light_data.object_space_cam_pos, XMVector3TransformCoord(eye_pos, world_to_object));
        XMStoreFloat3(&volume_light_data.object_space_cam_forward, XMVector4Transform(eye_forward, world_to_object));

        // Transfer updates to the GPU.
        mtx.lock();
        vs_cb.update(&obj_data_vs, cmd_list);
        ps_cb.update(&volume_light_data, cmd_list);
        mtx.unlock();

        m_gpu.transition(D3D12_RESOURCE_STATE_COPY_DEST,
                         D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                         {vs_cb.default_resource,
                          ps_cb.default_resource},
                         cmd_list);

        // Set vertex and index buffers.
        const mesh *current_mesh = &vl->m_mesh;
        cmd_list->IASetVertexBuffers(0, 1, &current_mesh->m_vbv);
        cmd_list->IASetIndexBuffer(&current_mesh->m_ibv);

        // Draw each submesh.
        for (const mesh::submesh &submesh : vl->m_mesh.m_submeshes)
        {
            cmd_list->DrawIndexedInstanced(submesh.index_count, 1,
                                           submesh.start_index_location,
                                           submesh.base_vertex_location, 0);
        }
    }
    m_gpu.timer_stop(cmd_list, "Draw volume lights");
}

void particles_graphics::create_particle_systems_data(ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    // Initialize particle simulation data.
    for (size_t i = 0; i < num_particle_systems; i++)
    {
        particle_system_transforms[i].set_translation(7.f * i, 1.0f, 0.f);

        for (size_t j = 0; j < num_particles_per_system; j++)
        {
            XMVECTOR position = XMVectorSet(gaussian_random_float(0.01f, 1.0f),
                                            gaussian_random_float(0.01f, 1.f),
                                            gaussian_random_float(0.01f, 1.f),
                                            0.f);

            position = XMVector3Normalize(position);
            XMStoreFloat3(&particles[i][j].position, position);
            particles[i][j].size = 0.17f;
            particles[i][j].age = 1.f;
            particles[i][j].velocity = {0.f, 0.0f, 0.f};
        }
    }

    size_t particle_buffer_size = num_particles_per_system * sizeof(particle::aligned_aos);
    size_t total_particles_buffer_size = particle_buffer_size * num_particle_systems;
    size_t cbv_particle_buffer_size = align_up(particle_buffer_size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    m_gpu.default_resource_from_uploader(cmd_list, particle_initial_default.GetAddressOf(),
                                         particles,
                                         total_particles_buffer_size,
                                         D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    NAME_D3D12_OBJECT(particle_initial_default);

    // Create input particles buffer filled with initial simulation data.
    m_gpu.default_resource_from_uploader(cmd_list, particle_input_default.GetAddressOf(),
                                         particles,
                                         num_particles_total * sizeof(particle::aligned_aos),
                                         sizeof(particle::aligned_aos),
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    NAME_D3D12_OBJECT(particle_input_default);

    // Create empty output particles buffer.
    check_hr(m_gpu.device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(num_particles_total * sizeof(particle::aligned_aos),
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(particle_output_default.GetAddressOf())));
    NAME_D3D12_OBJECT(particle_output_default);

    // Create a SRV/UAV for each particle system inside each of the descriptor heaps.
    // UAV description.
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    uav_desc.Buffer.NumElements = num_particles_per_system;
    uav_desc.Buffer.StructureByteStride = sizeof(particle::aligned_aos);

    // SRV description.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = DXGI_FORMAT_UNKNOWN;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    srv_desc.Buffer.NumElements = num_particles_per_system;
    srv_desc.Buffer.StructureByteStride = sizeof(particle::aligned_aos);

    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    size_t offset_in_descriptors = 0;
    size_t offset_in_bytes = 0;
    for (size_t i = 0; i < m_gpu.NUM_BACK_BUFFERS; i++)
    {
        auto csu_table_alloc = m_gpu.frames[i].csu_table_allocator;
        ComPtr<ID3D12DescriptorHeap> csu_heap_gpu = csu_table_alloc.m_heap_gpu;

        for (size_t j = 0; j < num_particle_systems; j++)
        {
            uav_desc.Buffer.FirstElement = j * num_particles_per_system;
            uav_desc.Buffer.CounterOffsetInBytes = 0;

            // Create input particles UAV.
            offset_in_descriptors = uav_particle_inputs + j;
            offset_in_bytes = offset_in_descriptors * m_gpu.csu_descriptor_size;
            handle.ptr = csu_heap_gpu->GetCPUDescriptorHandleForHeapStart().ptr + offset_in_bytes;
            m_gpu.device->CreateUnorderedAccessView(particle_input_default.Get(), nullptr, &uav_desc, handle);

            // Create output particles UAV.
            offset_in_descriptors = uav_particle_outputs + j;
            offset_in_bytes = offset_in_descriptors * m_gpu.csu_descriptor_size;
            handle.ptr = csu_heap_gpu->GetCPUDescriptorHandleForHeapStart().ptr + offset_in_bytes;
            m_gpu.device->CreateUnorderedAccessView(particle_output_default.Get(), nullptr, &uav_desc, handle);

            // Create output particles SRV.
            srv_desc.Buffer.FirstElement = j * num_particles_per_system;
            offset_in_descriptors = srv_particle_outputs + j;
            offset_in_bytes = offset_in_descriptors * m_gpu.csu_descriptor_size;
            handle.ptr = csu_heap_gpu->GetCPUDescriptorHandleForHeapStart().ptr + offset_in_bytes;
            m_gpu.device->CreateShaderResourceView(particle_output_default.Get(), &srv_desc, handle);

            // Create initial particles SRV.
            offset_in_descriptors = srv_particle_initial + j;
            offset_in_bytes = offset_in_descriptors * m_gpu.csu_descriptor_size;
            handle.ptr = csu_heap_gpu->GetCPUDescriptorHandleForHeapStart().ptr + offset_in_bytes;
            m_gpu.device->CreateShaderResourceView(particle_initial_default.Get(), &srv_desc, handle);
        }

        // Calculate base pointers to particle descriptors.
        size_t offset_to_particles = srv_particle_initial * csu_table_alloc.m_descriptor_size;
        particles_base_cpu[i].ptr = csu_table_alloc.m_heap_gpu->GetCPUDescriptorHandleForHeapStart().ptr + offset_to_particles;
        compute_descriptors_base_gpu_handle[i].ptr = csu_table_alloc.m_heap_gpu->GetGPUDescriptorHandleForHeapStart().ptr + offset_to_particles;
    }

    // Particle system info.
    for (int i = 0; i < num_particle_systems; i++)
    {
        XMMATRIX world = XMLoadFloat4x4(&particle_system_transforms[i].m_world);
        XMStoreFloat4x4(&particle_systems_infos[i].world, XMMatrixTranspose(world));

        particle_systems_infos[i].particle_system_index = i;
        particle_systems_infos[i].particle_lights_enabled = 1;
        particle_systems_infos[i].amplitude = 1.0f;
        particle_systems_infos[i].frequency = 1.7f;
        particle_systems_infos[i].depth_fade = 0.05f;
        particle_systems_infos[i].speed = 0.3f;
        particle_systems_infos[i].light.id = i;
        particle_systems_infos[i].light.color = {1.f, 0.65f, 0.43f};
        particle_systems_infos[i].light.strenght = {0.7f, 0.7f, 0.7f};
        particle_systems_infos[i].light.falloff_end = 1.1f;
        particle_systems_infos[i].light.falloff_start = 0.01f;
    }
    m_particle_system_info = m_gpu.create_constant_buffer<particle_system_info>(particle_systems_infos, _countof(particle_systems_infos));
    NAME_D3D12_OBJECT(m_particle_system_info.default_resource);

    // Create the particle textures heap.
    const int max_tex_width = 256;
    const int max_tex_height = 256;
    const int max_num_mips = int_log2(max_tex_width);
    const int bytes_per_pixel = 4;
    const int max_sprite_size = (max_tex_width * max_tex_height) * (max_num_mips * bytes_per_pixel);

    D3D12_HEAP_DESC sprite_textures_heap_desc = {};
    sprite_textures_heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    sprite_textures_heap_desc.Flags = D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES | D3D12_HEAP_FLAG_DENY_BUFFERS;
    sprite_textures_heap_desc.Properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    sprite_textures_heap_desc.SizeInBytes = num_particle_systems * num_textures_per_particle_system * max_sprite_size;

    check_hr(m_gpu.device->CreateHeap(&sprite_textures_heap_desc, IID_PPV_ARGS(&m_sprite_textures_heap)));
    NAME_D3D12_OBJECT(m_sprite_textures_heap);

    // Load the fire sprite texture.
    m_gpu.upload_dds(L"..\\particles\\textures\\fire.dds",
                     cmd_list,
                     m_fire_sprite.default_resource.GetAddressOf(), false, m_sprite_textures_heap);

    // Create the texture SRV.
    D3D12_RESOURCE_DESC tex_desc = m_fire_sprite.default_resource->GetDesc();

    D3D12_SHADER_RESOURCE_VIEW_DESC tex_srv_desc = {};
    tex_srv_desc.Format = tex_desc.Format;
    tex_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    tex_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    tex_srv_desc.Texture2D.MipLevels = tex_desc.MipLevels;
    tex_srv_desc.Texture2D.MostDetailedMip = 0;
    tex_srv_desc.Texture2D.PlaneSlice = 0;
    tex_srv_desc.Texture2D.ResourceMinLODClamp = 0.f;

    m_fire_sprite.srv_handle.ptr = m_gpu.csu_allocator.allocate();
    m_gpu.device->CreateShaderResourceView(m_fire_sprite.default_resource.Get(),
                                           &tex_srv_desc,
                                           m_fire_sprite.srv_handle);
}

void particles_graphics::create_particle_simulation_commands(ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    struct particle_simulation_command
    {
        D3D12_GPU_VIRTUAL_ADDRESS cbv_particle_system_info;
        UINT32 input_buffer_index;
        UINT32 output_buffer_index;
        D3D12_DISPATCH_ARGUMENTS dispatch_args;
    };

    // Define the particle simulation command signature.
    D3D12_INDIRECT_ARGUMENT_DESC simulation_indirect_args[3] = {};
    simulation_indirect_args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
    simulation_indirect_args[0].ConstantBufferView.RootParameterIndex = 4;
    simulation_indirect_args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    simulation_indirect_args[1].Constant.DestOffsetIn32BitValues = 0;
    simulation_indirect_args[1].Constant.Num32BitValuesToSet = 2;
    simulation_indirect_args[1].Constant.RootParameterIndex = 6;
    simulation_indirect_args[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

    size_t simulation_command_size = sizeof(particle_simulation_command);
    size_t simulation_commands_size = simulation_command_size * num_particle_systems;

    D3D12_COMMAND_SIGNATURE_DESC particle_cmdsig_desc = {};
    particle_cmdsig_desc.NumArgumentDescs = _countof(simulation_indirect_args);
    particle_cmdsig_desc.pArgumentDescs = simulation_indirect_args;
    particle_cmdsig_desc.ByteStride = (UINT)simulation_command_size;
    check_hr(m_gpu.device->CreateCommandSignature(&particle_cmdsig_desc, m_compute_rootsig.Get(),
                                                  IID_PPV_ARGS(particle_sim_cmdsig.GetAddressOf())));
    NAME_D3D12_OBJECT(particle_sim_cmdsig);

    // Create indirect simulation commands.
    D3D12_DISPATCH_ARGUMENTS dispatch_args = {num_sim_threadgroups, 1, 1};
    size_t particle_system_info_size = sizeof(particle_system_info);

    particle_simulation_command particle_sim_cmds[num_particle_systems];
    for (int i = 0; i < num_particle_systems; i++)
    {
        // SetComputeRootConstantBufferView.
        particle_sim_cmds[i].cbv_particle_system_info = m_particle_system_info.default_resource->GetGPUVirtualAddress() +
                                                        (particle_system_info_size * i);

        // SetComputeRootConstants.
        particle_sim_cmds[i].input_buffer_index = i;
        particle_sim_cmds[i].output_buffer_index = i + num_particle_systems;

        // Dispatch.
        particle_sim_cmds[i].dispatch_args = dispatch_args;
    }
    m_gpu.default_resource_from_uploader(cmd_list, particle_simcmds_default.GetAddressOf(),
                                         particle_sim_cmds, simulation_commands_size, simulation_command_size,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    NAME_D3D12_OBJECT(particle_simcmds_default);

    // Create resource to hold the filtered particle simulation commands.
    m_gpu.default_resource_from_uploader(cmd_list, particle_simcmds_filtered_default.GetAddressOf(),
                                         nullptr, simulation_commands_size, simulation_command_size,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    NAME_D3D12_OBJECT(particle_simcmds_filtered_default);

    // Create indirect simulation commands that point to CBVs that contain swapped buffer indices.
    for (int i = 0; i < num_particle_systems; i++)
    {
        // SetComputeRootConstants.
        particle_sim_cmds[i].input_buffer_index = i + num_particle_systems;
        particle_sim_cmds[i].output_buffer_index = i;
    }
    m_gpu.default_resource_from_uploader(cmd_list, particle_simcmds_swap_default.GetAddressOf(),
                                         particle_sim_cmds, simulation_commands_size, simulation_command_size,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    NAME_D3D12_OBJECT(particle_simcmds_swap_default);

    // Create filtered particle simulation descriptors.
    D3D12_SHADER_RESOURCE_VIEW_DESC particle_simcmds_srv_desc = {};
    particle_simcmds_srv_desc.Format = DXGI_FORMAT_UNKNOWN;
    particle_simcmds_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    particle_simcmds_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    particle_simcmds_srv_desc.Buffer.FirstElement = 0;
    particle_simcmds_srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    particle_simcmds_srv_desc.Buffer.NumElements = num_particle_systems;
    particle_simcmds_srv_desc.Buffer.StructureByteStride = (UINT)simulation_command_size;

    D3D12_UNORDERED_ACCESS_VIEW_DESC particle_simcmds_filtered_uav_desc = {};
    particle_simcmds_filtered_uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    particle_simcmds_filtered_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    particle_simcmds_filtered_uav_desc.Buffer.FirstElement = 0;
    particle_simcmds_filtered_uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    particle_simcmds_filtered_uav_desc.Buffer.NumElements = num_particle_systems;
    particle_simcmds_filtered_uav_desc.Buffer.StructureByteStride = (UINT)simulation_command_size;
    particle_simcmds_filtered_uav_desc.Buffer.CounterOffsetInBytes = 0;

    for (UINT32 i = 0; i < gpu_interface::NUM_BACK_BUFFERS; i++)
    {
        auto csu_table_alloc = m_gpu.frames[i].csu_table_allocator;
        ComPtr<ID3D12DescriptorHeap> csu_heap_gpu = csu_table_alloc.m_heap_gpu;

        // Create filtered simulation commands UAV and it's associated counter.
        check_hr(m_gpu.device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                                                       D3D12_RESOURCE_STATE_COMMON,
                                                       nullptr,
                                                       IID_PPV_ARGS(&particle_simcmds_counter_default[i])));
        NAME_D3D12_OBJECT_INDEXED(particle_simcmds_counter_default[i], i);

        size_t offset_to_simcmds = uav_simulation_commands_buffer * csu_table_alloc.m_descriptor_size;
        cpu_uav_particle_simcmds_filtered_handle[i].ptr = csu_heap_gpu->GetCPUDescriptorHandleForHeapStart().ptr + offset_to_simcmds;
        m_gpu.device->CreateUnorderedAccessView(particle_simcmds_filtered_default.Get(), particle_simcmds_counter_default[i].Get(),
                                                &particle_simcmds_filtered_uav_desc, cpu_uav_particle_simcmds_filtered_handle[i]);
    }
}

void particles_graphics::create_particle_draw_commands(ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    struct particle_drawing_command
    {
        D3D12_VERTEX_BUFFER_VIEW vbv;
        D3D12_GPU_VIRTUAL_ADDRESS cbv_particle_system_info;
        D3D12_DRAW_ARGUMENTS draw_args;
    };

    // Define particle indirect draw commands.
    D3D12_INDIRECT_ARGUMENT_DESC draw_indirect_args[3] = {};
    draw_indirect_args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
    draw_indirect_args[0].VertexBuffer.Slot = 0;
    draw_indirect_args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
    draw_indirect_args[1].ConstantBufferView.RootParameterIndex = 13;
    draw_indirect_args[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

    size_t draw_command_size = sizeof(particle_drawing_command);
    size_t draw_commands_size = draw_command_size * num_particle_systems;

    D3D12_COMMAND_SIGNATURE_DESC particle_draw_cmdsig_desc = {};
    particle_draw_cmdsig_desc.NumArgumentDescs = _countof(draw_indirect_args);
    particle_draw_cmdsig_desc.pArgumentDescs = draw_indirect_args;
    particle_draw_cmdsig_desc.ByteStride = (UINT)draw_command_size;
    check_hr(m_gpu.device->CreateCommandSignature(&particle_draw_cmdsig_desc, m_graphics_rootsig.Get(),
                                                  IID_PPV_ARGS(particle_draw_cmdsig.GetAddressOf())));
    NAME_D3D12_OBJECT(particle_draw_cmdsig);

    // Create indirect particle draw commands.
    D3D12_DRAW_ARGUMENTS draw_args = {};
    draw_args.InstanceCount = 1;
    draw_args.VertexCountPerInstance = num_particles_per_system;
    draw_args.StartInstanceLocation = 0;
    draw_args.StartVertexLocation = 0;

    size_t particle_size = sizeof(particle::aligned_aos);
    size_t particles_size = particle_size * num_particles_per_system;

    particle_drawing_command particle_draw_cmds[num_particle_systems];
    for (int i = 0; i < num_particle_systems; i++)
    {
        // IASetVertexBuffer.
        particle_draw_cmds[i].vbv.StrideInBytes = (UINT)particle_size;
        particle_draw_cmds[i].vbv.SizeInBytes = (UINT)particles_size;
        size_t offset_in_bytes = (particles_size)*i;
        particle_draw_cmds[i].vbv.BufferLocation = particle_output_default->GetGPUVirtualAddress() + offset_in_bytes;

        // SetGraphicsRootConstantBufferView.
        particle_draw_cmds[i].cbv_particle_system_info = m_particle_system_info.default_resource->GetGPUVirtualAddress() +
                                                         (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT * i);
        // DrawIndexedInstanced.
        particle_draw_cmds[i].draw_args = draw_args;
    }
    m_gpu.default_resource_from_uploader(cmd_list, particle_drawcmds_default.GetAddressOf(),
                                         particle_draw_cmds, draw_commands_size,
                                         draw_command_size,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    NAME_D3D12_OBJECT(particle_drawcmds_default);

    // Create resource to hold the filtered particle draw commands.
    check_hr(m_gpu.device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &CD3DX12_RESOURCE_DESC::Buffer(draw_commands_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                                                   D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                                                   nullptr,
                                                   IID_PPV_ARGS(&particle_drawcmds_filtered_default)));
    NAME_D3D12_OBJECT(particle_drawcmds_filtered_default);

    // Create filtered particle drawing descriptors.
    D3D12_SHADER_RESOURCE_VIEW_DESC particle_drawcmds_filtered_srv_desc = {};
    particle_drawcmds_filtered_srv_desc.Format = DXGI_FORMAT_UNKNOWN;
    particle_drawcmds_filtered_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    particle_drawcmds_filtered_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    particle_drawcmds_filtered_srv_desc.Buffer.FirstElement = 0;
    particle_drawcmds_filtered_srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    particle_drawcmds_filtered_srv_desc.Buffer.NumElements = num_particle_systems;
    particle_drawcmds_filtered_srv_desc.Buffer.StructureByteStride = (UINT)draw_command_size;

    D3D12_UNORDERED_ACCESS_VIEW_DESC particle_drawcmds_filtered_uav_desc = {};
    particle_drawcmds_filtered_uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    particle_drawcmds_filtered_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    particle_drawcmds_filtered_uav_desc.Buffer.FirstElement = 0;
    particle_drawcmds_filtered_uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    particle_drawcmds_filtered_uav_desc.Buffer.NumElements = num_particle_systems;
    particle_drawcmds_filtered_uav_desc.Buffer.StructureByteStride = (UINT)draw_command_size;
    particle_drawcmds_filtered_uav_desc.Buffer.CounterOffsetInBytes = 0;

    for (UINT32 i = 0; i < gpu_interface::NUM_BACK_BUFFERS; i++)
    {
        auto csu_table_alloc = m_gpu.frames[i].csu_table_allocator;
        ComPtr<ID3D12DescriptorHeap> csu_heap_gpu = csu_table_alloc.m_heap_gpu;

        // Create filtered simulation commands SRV.
        size_t offset_to_drawcmds = srv_draw_commands_buffer * csu_table_alloc.m_descriptor_size;
        cpu_srv_particle_drawcmds_filtered_handle[i].ptr = csu_heap_gpu->GetCPUDescriptorHandleForHeapStart().ptr + offset_to_drawcmds;
        m_gpu.device->CreateShaderResourceView(particle_drawcmds_default.Get(),
                                               &particle_drawcmds_filtered_srv_desc, cpu_srv_particle_drawcmds_filtered_handle[i]);

        // Create filtered simulation commands UAV and it's associated counter.
        check_hr(m_gpu.device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                                                       D3D12_RESOURCE_STATE_COMMON,
                                                       nullptr,
                                                       IID_PPV_ARGS(&particle_drawcmds_counter_default[i])));
        NAME_D3D12_OBJECT_INDEXED(particle_drawcmds_counter_default[i], i);

        offset_to_drawcmds = uav_draw_commands_buffer * csu_table_alloc.m_descriptor_size;
        cpu_uav_particle_drawcmds_filtered_handle[i].ptr = csu_heap_gpu->GetCPUDescriptorHandleForHeapStart().ptr + offset_to_drawcmds;
        m_gpu.device->CreateUnorderedAccessView(particle_drawcmds_filtered_default.Get(), particle_drawcmds_counter_default[i].Get(),
                                                &particle_drawcmds_filtered_uav_desc, cpu_uav_particle_drawcmds_filtered_handle[i]);
    }
}

void particles_graphics::create_ibl_textures(ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    // Load equirectangular environment texture data.
    m_gpu.upload_dds(L"..\\particles\\textures\\dikholo.hdr",
                     cmd_list, m_equirect_tex.default_resource.GetAddressOf(),
                     true, nullptr, 0);
    NAME_D3D12_OBJECT(m_equirect_tex.default_resource);

    D3D12_RESOURCE_DESC tex_desc = m_equirect_tex.default_resource->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC tex_srv_desc = {};
    tex_srv_desc.Format = tex_desc.Format;
    tex_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    tex_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    tex_srv_desc.Texture2D.MipLevels = tex_desc.MipLevels;
    tex_srv_desc.Texture2D.MostDetailedMip = 0;
    tex_srv_desc.Texture2D.PlaneSlice = 0;
    tex_srv_desc.Texture2D.ResourceMinLODClamp = 0.f;

    m_equirect_tex.srv_handle.ptr = m_gpu.csu_allocator.allocate();
    m_gpu.device->CreateShaderResourceView(m_equirect_tex.default_resource.Get(),
                                           &tex_srv_desc,
                                           m_equirect_tex.srv_handle);

    // Create texture to hold the unfiltered environment map.
    D3D12_RESOURCE_DESC unfiltered_envmap_tex_desc = {};
    unfiltered_envmap_tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    unfiltered_envmap_tex_desc.DepthOrArraySize = 6;
    unfiltered_envmap_tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    unfiltered_envmap_tex_desc.Format = hdr_buffer_format;
    unfiltered_envmap_tex_desc.Width = envmap_res;
    unfiltered_envmap_tex_desc.Height = envmap_res;
    unfiltered_envmap_tex_desc.MipLevels = 1;
    unfiltered_envmap_tex_desc.SampleDesc.Count = 1;
    unfiltered_envmap_tex_desc.SampleDesc.Quality = 0;
    unfiltered_envmap_tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    unfiltered_envmap_tex_desc.Alignment = m_gpu.device->GetResourceAllocationInfo(0, 1, &unfiltered_envmap_tex_desc).Alignment;

    // Create the unfiltered environment map resource.
    D3D12_CLEAR_VALUE clear_value = {};
    clear_value.Format = hdr_buffer_format;
    clear_value.Color[0] = 0.f;
    clear_value.Color[1] = 0.f;
    clear_value.Color[2] = 0.f;
    clear_value.Color[3] = 0.f;
    check_hr(m_gpu.device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &unfiltered_envmap_tex_desc,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                   &clear_value,
                                                   IID_PPV_ARGS(m_unfiltered_tex.default_resource.GetAddressOf())));
    NAME_D3D12_OBJECT(m_unfiltered_tex.default_resource);

    // Unfiltered environment map UAV.
    D3D12_UNORDERED_ACCESS_VIEW_DESC hdr_cubemap_uav_desc = {};
    hdr_cubemap_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    hdr_cubemap_uav_desc.Format = hdr_buffer_format;
    hdr_cubemap_uav_desc.Texture2DArray.ArraySize = 6;
    hdr_cubemap_uav_desc.Texture2DArray.FirstArraySlice = 0;
    hdr_cubemap_uav_desc.Texture2DArray.MipSlice = 0;
    hdr_cubemap_uav_desc.Texture2DArray.PlaneSlice = 0;

    m_unfiltered_tex.uav_handle.ptr = m_gpu.csu_allocator.allocate();
    m_gpu.device->CreateUnorderedAccessView(m_unfiltered_tex.default_resource.Get(), nullptr,
                                            &hdr_cubemap_uav_desc, m_unfiltered_tex.uav_handle);

    // Unfiltered environment map SRV.
    D3D12_SHADER_RESOURCE_VIEW_DESC hdr_cubemap_srv_desc = {};
    hdr_cubemap_srv_desc.Format = hdr_buffer_format;
    hdr_cubemap_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    hdr_cubemap_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    hdr_cubemap_srv_desc.TextureCube.MipLevels = 1;
    hdr_cubemap_srv_desc.TextureCube.MostDetailedMip = 0;
    hdr_cubemap_srv_desc.TextureCube.ResourceMinLODClamp = 0.f;

    m_unfiltered_tex.srv_handle.ptr = m_gpu.csu_allocator.allocate();
    m_gpu.device->CreateShaderResourceView(m_unfiltered_tex.default_resource.Get(),
                                           &hdr_cubemap_srv_desc, m_unfiltered_tex.srv_handle);

    // Create the resource to hold the diffuse irradiance map.
    D3D12_RESOURCE_DESC diffuse_irradiance_map_desc = unfiltered_envmap_tex_desc;
    diffuse_irradiance_map_desc.Width = 64;
    diffuse_irradiance_map_desc.Height = 64;
    check_hr(m_gpu.device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &diffuse_irradiance_map_desc,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                   &clear_value,
                                                   IID_PPV_ARGS(m_diffuse_irradiance_tex.default_resource.GetAddressOf())));

    // Diffuse irradiance map UAV.
    m_diffuse_irradiance_tex.uav_handle.ptr = m_gpu.csu_allocator.allocate();
    m_gpu.device->CreateUnorderedAccessView(m_diffuse_irradiance_tex.default_resource.Get(), nullptr,
                                            &hdr_cubemap_uav_desc, m_diffuse_irradiance_tex.uav_handle);

    // Diffuse irradiance map SRV.
    m_diffuse_irradiance_tex.srv_handle.ptr = m_gpu.csu_allocator.allocate();
    m_gpu.device->CreateShaderResourceView(m_diffuse_irradiance_tex.default_resource.Get(),
                                           &hdr_cubemap_srv_desc, m_diffuse_irradiance_tex.srv_handle);

    // Create specular irradiance resource.
    D3D12_RESOURCE_DESC specular_irradiance_desc = unfiltered_envmap_tex_desc;
    specular_irradiance_desc.MipLevels = (UINT16)num_mipmap_levels(unfiltered_envmap_tex_desc.Width, (UINT64)unfiltered_envmap_tex_desc.Height);
    specular_irradiance_desc.Alignment = m_gpu.device->GetResourceAllocationInfo(0, 1, &specular_irradiance_desc).Alignment;

    check_hr(m_gpu.device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &specular_irradiance_desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST,
                                                   &clear_value,
                                                   IID_PPV_ARGS(m_specular_irradiance_tex.default_resource.GetAddressOf())));
    NAME_D3D12_OBJECT(m_specular_irradiance_tex.default_resource);

    // Create a SRV for the full cube texture mipmap of the specualar irradiance map.
    D3D12_SHADER_RESOURCE_VIEW_DESC hdr_cubemap_srv_fullmipmap_desc = hdr_cubemap_srv_desc;
    hdr_cubemap_srv_fullmipmap_desc.Texture2D.MipLevels = specular_irradiance_desc.MipLevels;
    m_specular_irradiance_tex.srv_handle.ptr = m_gpu.csu_allocator.allocate();
    m_gpu.device->CreateShaderResourceView(m_specular_irradiance_tex.default_resource.Get(),
                                           &hdr_cubemap_srv_fullmipmap_desc,
                                           m_specular_irradiance_tex.srv_handle);

    // Create a SRV for mip0 as a cube texture of the specualar irradiance map.
    D3D12_SHADER_RESOURCE_VIEW_DESC hdr_cubemap_srv_cube_mip0_desc = hdr_cubemap_srv_desc;
    hdr_cubemap_srv_cube_mip0_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    hdr_cubemap_srv_cube_mip0_desc.TextureCube.MipLevels = 1;
    hdr_cubemap_srv_cube_mip0_desc.TextureCube.MostDetailedMip = 0;
    hdr_cubemap_srv_cube_mip0_desc.TextureCube.ResourceMinLODClamp = 0.f;

    D3D12_CPU_DESCRIPTOR_HANDLE mip0_cube_descriptor;
    mip0_cube_descriptor.ptr = m_gpu.csu_allocator.allocate();
    m_gpu.device->CreateShaderResourceView(m_specular_irradiance_tex.default_resource.Get(),
                                           &hdr_cubemap_srv_cube_mip0_desc, mip0_cube_descriptor);
    m_specular_irradiance_tex.additional_SRVs["mip0_cube"] = mip0_cube_descriptor;

    // Create a UAV and SRV for each mip of the specular irradiance mipmap.
    m_specular_irradiance_tex.mips_uav_handles.resize(specular_irradiance_desc.MipLevels);
    m_specular_irradiance_tex.mips_srv_handles.resize(specular_irradiance_desc.MipLevels);

    D3D12_SHADER_RESOURCE_VIEW_DESC hdr_cubemap_src_mips_desc = {};
    hdr_cubemap_src_mips_desc.Format = hdr_buffer_format;
    hdr_cubemap_src_mips_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    hdr_cubemap_src_mips_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    hdr_cubemap_src_mips_desc.Texture2DArray.PlaneSlice = 0;
    hdr_cubemap_src_mips_desc.Texture2DArray.ResourceMinLODClamp = 0.f;
    hdr_cubemap_src_mips_desc.Texture2DArray.MipLevels = 1;
    hdr_cubemap_src_mips_desc.Texture2DArray.ArraySize = 6;
    hdr_cubemap_src_mips_desc.Texture2DArray.FirstArraySlice = 0;

    D3D12_UNORDERED_ACCESS_VIEW_DESC hdr_cubemap_uav_mips_desc = hdr_cubemap_uav_desc;

    for (int mip_level = 0; mip_level < specular_irradiance_desc.MipLevels; mip_level++)
    {
        // Specular irradiance map UAVs.
        hdr_cubemap_uav_mips_desc.Texture2DArray.MipSlice = mip_level;
        D3D12_CPU_DESCRIPTOR_HANDLE mip_uav_handle;
        mip_uav_handle.ptr = m_gpu.csu_allocator.allocate();
        m_specular_irradiance_tex.mips_uav_handles[mip_level] = mip_uav_handle;
        m_gpu.device->CreateUnorderedAccessView(m_specular_irradiance_tex.default_resource.Get(), nullptr,
                                                &hdr_cubemap_uav_mips_desc,
                                                mip_uav_handle);

        // Specular irradiance map SRVs.
        hdr_cubemap_src_mips_desc.Texture2DArray.MostDetailedMip = mip_level;
        D3D12_CPU_DESCRIPTOR_HANDLE mip_srv_handle;
        mip_srv_handle.ptr = m_gpu.csu_allocator.allocate();
        m_specular_irradiance_tex.mips_srv_handles[mip_level] = mip_srv_handle;
        m_gpu.device->CreateShaderResourceView(m_specular_irradiance_tex.default_resource.Get(),
                                               &hdr_cubemap_src_mips_desc,
                                               mip_srv_handle);
    }

    // Create the texture to hold the pre-computed specular BRDF lut.
    D3D12_RESOURCE_DESC specular_brdf_lut_desc = {};
    specular_brdf_lut_desc.Width = 256;
    specular_brdf_lut_desc.Height = 256;
    specular_brdf_lut_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    specular_brdf_lut_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    specular_brdf_lut_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    specular_brdf_lut_desc.DepthOrArraySize = 1;
    specular_brdf_lut_desc.MipLevels = 1;
    specular_brdf_lut_desc.SampleDesc.Count = 1;
    specular_brdf_lut_desc.SampleDesc.Quality = 0;
    check_hr(m_gpu.device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &specular_brdf_lut_desc,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                   nullptr,
                                                   IID_PPV_ARGS(m_specular_brdf_lut.default_resource.GetAddressOf())));
    NAME_D3D12_OBJECT(m_specular_brdf_lut.default_resource);

    // Create UAV and SRV for specular brdf lut.
    m_specular_brdf_lut.srv_handle.ptr = m_gpu.csu_allocator.allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC specular_brdf_lut_srv_desc = {};
    specular_brdf_lut_srv_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    specular_brdf_lut_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    specular_brdf_lut_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    specular_brdf_lut_srv_desc.Texture2D.MipLevels = 1;
    specular_brdf_lut_srv_desc.Texture2D.MostDetailedMip = 0;
    specular_brdf_lut_srv_desc.Texture2D.PlaneSlice = 0;
    specular_brdf_lut_srv_desc.Texture2D.ResourceMinLODClamp = 0.f;
    m_gpu.device->CreateShaderResourceView(m_specular_brdf_lut.default_resource.Get(), &specular_brdf_lut_srv_desc, m_specular_brdf_lut.srv_handle);

    m_specular_brdf_lut.uav_handle.ptr = m_gpu.csu_allocator.allocate();
    D3D12_UNORDERED_ACCESS_VIEW_DESC specular_brdf_lut_uav_desc = {};
    specular_brdf_lut_uav_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    specular_brdf_lut_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    specular_brdf_lut_uav_desc.Texture2D.MipSlice = 0;
    specular_brdf_lut_uav_desc.Texture2D.PlaneSlice = 0;
    m_gpu.device->CreateUnorderedAccessView(m_specular_brdf_lut.default_resource.Get(),
                                            nullptr, &specular_brdf_lut_uav_desc, m_specular_brdf_lut.uav_handle);

    // Convert equirectangular environment map to a cube texture.
    m_gpu.transition(D3D12_RESOURCE_STATE_COPY_DEST,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                     {m_equirect_tex.default_resource},
                     cmd_list);
    cmd_list->SetPipelineState(m_PSOs[equirect_to_cube_PSO]);

    gpu_interface::frame_resource *frame_resource = m_gpu.get_frame_resource();
    frame_resource->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, CS, UAV, 0, m_unfiltered_tex.uav_handle);
    frame_resource->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, CS, SRV, 0, m_equirect_tex.srv_handle);
    frame_resource->sampler_table_allocator.stage_to_cpu_heap(m_gpu.device, CS, sampler, 0, m_samplers[linear_wrap]);
    m_gpu.set_descriptor_tables(cmd_list);

    D3D12_RESOURCE_DESC unfiltered_envmap_desc = m_unfiltered_tex.default_resource->GetDesc();
    cmd_list->Dispatch((UINT)unfiltered_envmap_desc.Width / 32,
                       unfiltered_envmap_desc.Height / 32,
                       6);

    // Copy the unfiltered environment mip0 into mip0 of the specular irradiance map.
    // This is because mip0 will contain the highest frequency details, thus it doesn't get convolved at all.
    m_gpu.transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_COPY_SOURCE,
                     {m_unfiltered_tex.default_resource},
                     cmd_list);

    for (UINT array_slice = 0; array_slice < specular_irradiance_desc.DepthOrArraySize; array_slice++)
    {
        UINT subresource = D3D12CalcSubresource(0, array_slice, 0,
                                                specular_irradiance_desc.MipLevels, specular_irradiance_desc.DepthOrArraySize);
        D3D12_TEXTURE_COPY_LOCATION spec_irradiance_dst;
        spec_irradiance_dst.pResource = m_specular_irradiance_tex.default_resource.Get();
        spec_irradiance_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        spec_irradiance_dst.SubresourceIndex = subresource;

        D3D12_TEXTURE_COPY_LOCATION unfiltered_envmap_src;
        unfiltered_envmap_src.pResource = m_unfiltered_tex.default_resource.Get();
        unfiltered_envmap_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        unfiltered_envmap_src.SubresourceIndex = array_slice;
        cmd_list->CopyTextureRegion(&spec_irradiance_dst, 0, 0, 0,
                                    &unfiltered_envmap_src, nullptr);
    }

    // Generate mipmap for the specular irradiance map.
    m_gpu.transition(D3D12_RESOURCE_STATE_COPY_DEST,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                     {m_specular_irradiance_tex.default_resource},
                     cmd_list);
    cmd_list->SetPipelineState(m_PSOs[generate_mipmap_PSO]);

    std::vector<D3D12_RESOURCE_BARRIER> to_write_state(specular_irradiance_desc.DepthOrArraySize);
    std::vector<D3D12_RESOURCE_BARRIER> to_read_state(specular_irradiance_desc.DepthOrArraySize);
    for (int mip_level = 0, mip_width = (int)specular_irradiance_desc.Width / 2, mip_height = specular_irradiance_desc.Height / 2;
         mip_level < specular_irradiance_desc.MipLevels - 1;
         mip_level++, mip_width /= 2, mip_height /= 2)
    {

        // Transition each subresource of the cube map current mip level.
        for (UINT cube_face_index = 0;
             cube_face_index < specular_irradiance_desc.DepthOrArraySize;
             cube_face_index++)
        {
            UINT subresource = D3D12CalcSubresource(mip_level + 1, cube_face_index, 0,
                                                    specular_irradiance_desc.MipLevels, specular_irradiance_desc.DepthOrArraySize);
            to_write_state[cube_face_index] = m_gpu.transition(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                               {m_specular_irradiance_tex.default_resource},
                                                               nullptr,
                                                               subresource)
                                                  .front();
            to_read_state[cube_face_index] = m_gpu.transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                              {m_specular_irradiance_tex.default_resource},
                                                              nullptr,
                                                              subresource)
                                                 .front();
        }

        cmd_list->ResourceBarrier((UINT)to_write_state.size(), to_write_state.data());

        frame_resource->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, CS, SRV, 2,
                                                              m_specular_irradiance_tex.mips_srv_handles[mip_level]);
        frame_resource->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, CS, UAV, 2,
                                                              m_specular_irradiance_tex.mips_uav_handles[mip_level + 1]);
        m_gpu.set_descriptor_tables(cmd_list);

        UINT tgroups_x = (std::max)(1, mip_width / 8);
        UINT tgroups_y = (std::max)(1, mip_height / 8);
        UINT tgroups_z = specular_irradiance_desc.DepthOrArraySize;
        cmd_list->Dispatch(tgroups_x, tgroups_y, tgroups_z);
        cmd_list->ResourceBarrier((UINT)to_read_state.size(), to_read_state.data());
    }

    m_gpu.transition(D3D12_RESOURCE_STATE_COPY_SOURCE,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                     {m_unfiltered_tex.default_resource},
                     cmd_list);

    // Transition the specular irradiance cube map mip chain (except mip0) to write-only.
    size_t mipchain_levels = specular_irradiance_desc.MipLevels - 1;
    std::vector<D3D12_RESOURCE_BARRIER> mipchain_to_write_state;
    mipchain_to_write_state.reserve(specular_irradiance_desc.DepthOrArraySize * mipchain_levels);
    for (UINT mip_level = 1; mip_level < mipchain_levels; mip_level++)
    {
        for (UINT array_slice = 0; array_slice < specular_irradiance_desc.DepthOrArraySize; array_slice++)
        {
            UINT cube_face_mip_subresource = D3D12CalcSubresource(mip_level, array_slice, 0,
                                                                  specular_irradiance_desc.MipLevels, specular_irradiance_desc.DepthOrArraySize);
            mipchain_to_write_state.push_back(m_gpu.transition(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                               {m_specular_irradiance_tex.default_resource},
                                                               nullptr,
                                                               cube_face_mip_subresource)
                                                  .front());
        }
    }
    cmd_list->ResourceBarrier((UINT)mipchain_to_write_state.size(), mipchain_to_write_state.data());

    // Filter the mip chain of the unfiltered environment map to obtain a specular irradiance map.
    cmd_list->SetPipelineState(m_PSOs[filter_specular_irradiance_map_PSO]);

    // Get the width of the Mip1 subresource.
    UINT mip1_subresource = D3D12CalcSubresource(1, 0, 0,
                                                 specular_irradiance_desc.MipLevels, specular_irradiance_desc.DepthOrArraySize);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT mip1_layout;
    m_gpu.device->GetCopyableFootprints(&specular_irradiance_desc, mip1_subresource, 1, 0,
                                        &mip1_layout, nullptr, nullptr, nullptr);

    float delta_roughness = 1.f / float(mipchain_levels);
    for (int mip_level = 1, mip_width = mip1_layout.Footprint.Width, mip_height = mip1_layout.Footprint.Height;
         mip_level < mipchain_levels;
         mip_level++, mip_width /= 2, mip_height /= 2)
    {
        float roughness_per_mip = delta_roughness * mip_level;

        cmd_list->SetComputeRoot32BitConstant(6, (UINT)roughness_per_mip, 0);

        // Mip0 of the specular irradiance map will act as our unfiltered environment map.
        frame_resource->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, CS, SRV, 2,
                                                              m_specular_irradiance_tex.additional_SRVs["mip0_cube"]);
        frame_resource->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, CS, UAV, 2,
                                                              m_specular_irradiance_tex.mips_uav_handles[mip_level]);
        m_gpu.set_descriptor_tables(cmd_list);

        UINT tgroups_x = (UINT)((std::max)(1, mip_width / 8));
        UINT tgroups_y = (UINT)((std::max)(1, mip_height / 8));
        UINT tgroups_z = specular_irradiance_desc.DepthOrArraySize;
        cmd_list->Dispatch(tgroups_x,
                           tgroups_y,
                           tgroups_z);
    }

    // Filter the environment map to obtain a diffuse irradiance map.
    cmd_list->SetPipelineState(m_PSOs[filter_diffuse_irradiance_map_PSO]);
    frame_resource->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, CS, SRV, 1, m_unfiltered_tex.srv_handle);
    frame_resource->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, CS, UAV, 1, m_diffuse_irradiance_tex.uav_handle);
    m_gpu.set_descriptor_tables(cmd_list);

    cmd_list->Dispatch((UINT)diffuse_irradiance_map_desc.Width / 32,
                       diffuse_irradiance_map_desc.Height / 32,
                       6);

    // Pre-integrate the Cook-Torrance specular BRDF for varying roughness and viewing directions inside of a look-up table.
    cmd_list->SetPipelineState(m_PSOs[pre_integrate_specular_brdf_PSO]);
    frame_resource->sampler_table_allocator.stage_to_cpu_heap(m_gpu.device, CS, sampler, 1, m_samplers[linear_clamp]);
    frame_resource->csu_table_allocator.stage_to_cpu_heap(m_gpu.device, CS, UAV, 0, m_specular_brdf_lut.uav_handle);
    m_gpu.set_descriptor_tables(cmd_list);

    D3D12_RESOURCE_DESC spec_brdf_lut_desc = m_specular_brdf_lut.default_resource->GetDesc();
    cmd_list->Dispatch((UINT)spec_brdf_lut_desc.Width / 32,
                       spec_brdf_lut_desc.Height / 32,
                       1);
}

void particles_graphics::create_point_shadows_draw_commands(ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    // Define command to render point shadows.
    D3D12_INDIRECT_ARGUMENT_DESC render_point_shadows_indirect_args[4] = {};
    render_point_shadows_indirect_args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
    render_point_shadows_indirect_args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
    render_point_shadows_indirect_args[1].VertexBuffer.Slot = 0;
    render_point_shadows_indirect_args[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    render_point_shadows_indirect_args[2].Constant.Num32BitValuesToSet = 2;
    render_point_shadows_indirect_args[2].Constant.RootParameterIndex = 11;
    render_point_shadows_indirect_args[2].Constant.DestOffsetIn32BitValues = 0;
    render_point_shadows_indirect_args[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    struct point_shadow_draw_command
    {
        D3D12_INDEX_BUFFER_VIEW ibv;
        D3D12_VERTEX_BUFFER_VIEW vbv;
        UINT cb_shadow_caster_id;
        UINT cb_light_id;
        D3D12_DRAW_INDEXED_ARGUMENTS draw_args;
    };

    D3D12_COMMAND_SIGNATURE_DESC render_point_shadows_cmdsig_desc = {};
    render_point_shadows_cmdsig_desc.NumArgumentDescs = _countof(render_point_shadows_indirect_args);
    render_point_shadows_cmdsig_desc.pArgumentDescs = render_point_shadows_indirect_args;
    render_point_shadows_cmdsig_desc.ByteStride = (UINT)sizeof(point_shadow_draw_command);
    check_hr(m_gpu.device->CreateCommandSignature(&render_point_shadows_cmdsig_desc, m_graphics_rootsig.Get(),
                                                  IID_PPV_ARGS(render_point_shadows_cmdsig.GetAddressOf())));
    NAME_D3D12_OBJECT(render_point_shadows_cmdsig);

    // Create the buffer of point shadow rendering commands.
    std::vector<point_shadow_draw_command> render_shadows_cmds;
    num_point_shadow_cmds = num_pointlight_cube_shadowmaps * num_total_submeshes;
    render_shadows_cmds.reserve(num_point_shadow_cmds);
    for (UINT light_id = 0; light_id < num_pointlight_cube_shadowmaps; light_id++)
    {
        int ro_id = 0;
        for (auto &ro_pair : m_render_objects)
        {
            render_object &ro = ro_pair.second;

            for (mesh::submesh &sm : ro.m_mesh.m_submeshes)
            {
                point_shadow_draw_command cmd;

                // Index buffer view.
                cmd.ibv = ro.m_mesh.m_ibv;

                // Vertex buffer view.
                cmd.vbv = ro.m_mesh.m_vbv;

                // Shadow caster ID root constant.
                cmd.cb_shadow_caster_id = ro_id;
                cmd.cb_light_id = light_id;

                // Draw info.
                cmd.draw_args.IndexCountPerInstance = sm.index_count;
                cmd.draw_args.InstanceCount = 1;
                cmd.draw_args.StartIndexLocation = sm.start_index_location;
                cmd.draw_args.BaseVertexLocation = sm.base_vertex_location;
                cmd.draw_args.StartInstanceLocation = 0;

                render_shadows_cmds.push_back(cmd);
            }
            ro_id++;
        }
    }

    // Group all the point shadow render commands together and create the default resource to hold them.
    m_gpu.default_resource_from_uploader(cmd_list, render_point_shadows_cmds_default.GetAddressOf(),
                                         render_shadows_cmds.data(), num_point_shadow_cmds * sizeof(point_shadow_draw_command),
                                         sizeof(point_shadow_draw_command),
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    NAME_D3D12_OBJECT(render_point_shadows_cmds_default);
}

void particles_graphics::create_render_targets()
{
    for (int i = 0; i < gpu_interface::NUM_BACK_BUFFERS; i++)
    {
        m_render_targets[i] = m_gpu.create_render_target(i, hdr_buffer_format);
        NAME_D3D12_OBJECT_INDEXED(m_render_targets[i].rt_default_resource, i);
    }
}

void particles_graphics::create_bounds(ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    // Bounds indices.
    UINT16 bounds_indices[24] = {
        0, 1, 0, 2,
        0, 4, 1, 3,
        1, 5, 2, 3,
        2, 6, 3, 7,
        4, 5, 4, 6,
        5, 7, 6, 7};
    size_t index_size = sizeof(UINT16);
    num_bb_indices = _countof(bounds_indices);
    size_t bounds_indices_size = index_size * num_bb_indices;

    // Create the bounds indices resource.
    m_gpu.default_resource_from_uploader(cmd_list, bounds_indices_resource.GetAddressOf(),
                                         bounds_indices, bounds_indices_size, bounds_indices_size);
    NAME_D3D12_OBJECT(bounds_indices_resource);
    bb_ibv.BufferLocation = bounds_indices_resource->GetGPUVirtualAddress();
    bb_ibv.Format = DXGI_FORMAT_R16_UINT;
    bb_ibv.SizeInBytes = (UINT)bounds_indices_size;

    size_t bb_size = sizeof(bounding_box);
    size_t bounds_vertices_size = bb_size * num_particle_systems;

    // Bounds vertices resources.
    check_hr(m_gpu.device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &CD3DX12_RESOURCE_DESC::Buffer(bounds_vertices_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                                                   D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                                                   nullptr,
                                                   IID_PPV_ARGS(bounds_vertices_resource.GetAddressOf())));
    NAME_D3D12_OBJECT(bounds_vertices_resource);

    // Bounds vertices SRV description.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_bounds_vertices_desc = {};
    srv_bounds_vertices_desc.Format = DXGI_FORMAT_UNKNOWN;
    srv_bounds_vertices_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv_bounds_vertices_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_bounds_vertices_desc.Buffer.StructureByteStride = (UINT)bb_size;
    srv_bounds_vertices_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    srv_bounds_vertices_desc.Buffer.FirstElement = 0;
    srv_bounds_vertices_desc.Buffer.NumElements = num_particle_systems;

    // Bounds vertices UAV description.
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_bounds_vertices_desc;
    uav_bounds_vertices_desc.Format = DXGI_FORMAT_UNKNOWN;
    uav_bounds_vertices_desc.Buffer.CounterOffsetInBytes = 0;
    uav_bounds_vertices_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav_bounds_vertices_desc.Buffer.StructureByteStride = (UINT)bb_size;
    uav_bounds_vertices_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    uav_bounds_vertices_desc.Buffer.FirstElement = 0;
    uav_bounds_vertices_desc.Buffer.NumElements = num_particle_systems;

    // Create the bounds SRVs and UAVs for each frame.
    for (size_t i = 0; i < m_gpu.NUM_BACK_BUFFERS; i++)
    {

        auto csu_table_alloc = m_gpu.frames[i].csu_table_allocator;
        ComPtr<ID3D12DescriptorHeap> csu_heap_gpu = csu_table_alloc.m_heap_gpu;

        // Bounds vertices SRV.
        size_t offset_to_bounds = srv_bounds_buffer * csu_table_alloc.m_descriptor_size;
        cpu_srv_bounds_vertices_handle[i].ptr = csu_heap_gpu->GetCPUDescriptorHandleForHeapStart().ptr + offset_to_bounds;
        m_gpu.device->CreateShaderResourceView(bounds_vertices_resource.Get(), &srv_bounds_vertices_desc, cpu_srv_bounds_vertices_handle[i]);

        // Bounds vertices UAV.
        offset_to_bounds = uav_bounds_buffer * csu_table_alloc.m_descriptor_size;
        cpu_uav_bounds_vertices_handle[i].ptr = csu_heap_gpu->GetCPUDescriptorHandleForHeapStart().ptr + offset_to_bounds;
        gpu_uav_bounds_vertices_handle[i].ptr = csu_heap_gpu->GetGPUDescriptorHandleForHeapStart().ptr + offset_to_bounds;
        m_gpu.device->CreateUnorderedAccessView(bounds_vertices_resource.Get(), nullptr, &uav_bounds_vertices_desc, cpu_uav_bounds_vertices_handle[i]);
    }
}

void particles_graphics::create_gbuffers()
{
    m_gbuffer0 = m_gpu.create_gbuffer(gbuffer0_format);
    NAME_D3D12_OBJECT(m_gbuffer0.rt_default_resource);

    m_gbuffer1 = m_gpu.create_gbuffer(gbuffer1_format);
    NAME_D3D12_OBJECT(m_gbuffer1.rt_default_resource);

    m_gbuffer2 = m_gpu.create_gbuffer(gbuffer2_format);
    NAME_D3D12_OBJECT(m_gbuffer2.rt_default_resource);
}

void particles_graphics::create_depth_target()
{
    // Create depth target
    D3D12_RESOURCE_DESC dt_tex_desc = {};
    dt_tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    dt_tex_desc.Format = depthtarget_tex_format;
    dt_tex_desc.Width = g_hwnd_width;
    dt_tex_desc.Height = g_hwnd_height;
    dt_tex_desc.DepthOrArraySize = 1;
    dt_tex_desc.Alignment = 0;
    dt_tex_desc.MipLevels = 1;
    dt_tex_desc.SampleDesc.Count = 1;
    dt_tex_desc.SampleDesc.Quality = 0;
    dt_tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dt_tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_CLEAR_VALUE clear_value = {};
    clear_value.Format = depthstencil_format;
    clear_value.DepthStencil.Depth = 1.f;
    clear_value.DepthStencil.Stencil = 0;

    check_hr(m_gpu.device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &dt_tex_desc,
                                                   D3D12_RESOURCE_STATE_COMMON,
                                                   &clear_value,
                                                   IID_PPV_ARGS(depthtarget_default.GetAddressOf())));
    NAME_D3D12_OBJECT(depthtarget_default);

    // Create depth target DSV.
    depthtarget_dsv_handle.ptr = m_gpu.dsv_allocator.allocate();

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {};
    dsv_desc.Texture2D.MipSlice = 0;
    dsv_desc.Format = depthstencil_format;
    dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_gpu.device->CreateDepthStencilView(depthtarget_default.Get(), &dsv_desc, depthtarget_dsv_handle);

    // Create depth target SRV.
    depthtarget_srv_handle.ptr = m_gpu.csu_allocator.allocate();

    D3D12_SHADER_RESOURCE_VIEW_DESC dt_srv_desc;
    dt_srv_desc.Format = depthtarget_srv_format;
    dt_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    dt_srv_desc.Texture2D.MipLevels = 1;
    dt_srv_desc.Texture2D.MostDetailedMip = 0;
    dt_srv_desc.Texture2D.PlaneSlice = 0;
    dt_srv_desc.Texture2D.ResourceMinLODClamp = 0.f;
    dt_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    m_gpu.device->CreateShaderResourceView(depthtarget_default.Get(),
                                           &dt_srv_desc, depthtarget_srv_handle);
}

void particles_graphics::create_spotlight_shadowmaps()
{
    // Shadow map texture description.
    D3D12_RESOURCE_DESC shadow_tex_desc = {};
    shadow_tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    shadow_tex_desc.DepthOrArraySize = G_NUM_SHADOW_MAPS;
    shadow_tex_desc.MipLevels = 1;
    shadow_tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    shadow_tex_desc.Format = shadow_texture_format_alias; // Typeless format will get cast to D32 and R32.
    shadow_tex_desc.SampleDesc.Count = 1;
    shadow_tex_desc.SampleDesc.Quality = 0;
    shadow_tex_desc.Width = shadowmap_width;
    shadow_tex_desc.Height = shadowmap_height;

    D3D12_RESOURCE_ALLOCATION_INFO shadowmap_alloc_info;
    shadowmap_alloc_info = m_gpu.device->GetResourceAllocationInfo(0, 1, &shadow_tex_desc);
    shadow_tex_desc.Alignment = shadowmap_alloc_info.Alignment;

    // Create the heap that will hold the shadow map default resources.
    D3D12_HEAP_PROPERTIES heap_props = {};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_DESC shadows_heap_desc = {};
    shadows_heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    shadows_heap_desc.Flags = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED; // We always ClearDepthStencil() before writing to resources in this heap.
    shadows_heap_desc.Properties = heap_props;
    shadows_heap_desc.SizeInBytes = shadowmap_alloc_info.SizeInBytes * G_NUM_SHADOW_MAPS;

    D3D12_CLEAR_VALUE clear_value;
    clear_value.Format = shadow_texture_format_dsv;
    clear_value.DepthStencil.Depth = 1.0f;
    clear_value.DepthStencil.Stencil = 0;

    // Create the shadow maps resources.
    check_hr(m_gpu.device->CreateCommittedResource(&heap_props,
                                                   shadows_heap_desc.Flags,
                                                   &shadow_tex_desc,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear_value,
                                                   IID_PPV_ARGS(m_spotlight_shadowmaps.default_resource.GetAddressOf())));
    NAME_D3D12_OBJECT(m_spotlight_shadowmaps.default_resource);

    // Create a view to the entire array of shadow maps.
    D3D12_DEPTH_STENCIL_VIEW_DESC shadow_tex_dsv_desc = {};
    shadow_tex_dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
    shadow_tex_dsv_desc.Format = shadow_texture_format_dsv;
    shadow_tex_dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
    shadow_tex_dsv_desc.Texture2DArray.MipSlice = 0;
    shadow_tex_dsv_desc.Texture2DArray.FirstArraySlice = 0;
    shadow_tex_dsv_desc.Texture2DArray.ArraySize = G_NUM_SHADOW_MAPS;
    m_spotlight_shadowmaps.dsv_handle.ptr = m_gpu.dsv_allocator.allocate();
    m_gpu.device->CreateDepthStencilView(m_spotlight_shadowmaps.default_resource.Get(),
                                         &shadow_tex_dsv_desc,
                                         m_spotlight_shadowmaps.dsv_handle);

    m_spotlight_shadowmaps.array_dsv_handles.resize(G_NUM_SHADOW_MAPS);
    for (UINT i = 0; i < G_NUM_SHADOW_MAPS; i++)
    {
        m_spotlight_shadowmaps.array_dsv_handles[i].ptr = m_gpu.dsv_allocator.allocate();
        shadow_tex_dsv_desc.Texture2DArray.ArraySize = 1;
        shadow_tex_dsv_desc.Texture2DArray.FirstArraySlice = i;
        m_gpu.device->CreateDepthStencilView(m_spotlight_shadowmaps.default_resource.Get(),
                                             &shadow_tex_dsv_desc,
                                             m_spotlight_shadowmaps.array_dsv_handles[i]);
    }

    // Create shadow map SRVs.
    D3D12_SHADER_RESOURCE_VIEW_DESC shadow_srv_desc = {};
    shadow_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    shadow_srv_desc.Format = shadow_texture_format_srv;
    shadow_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shadow_srv_desc.Texture2DArray.ArraySize = G_NUM_SHADOW_MAPS;
    shadow_srv_desc.Texture2DArray.FirstArraySlice = 0;
    shadow_srv_desc.Texture2DArray.MipLevels = 1;
    shadow_srv_desc.Texture2DArray.MostDetailedMip = 0;
    shadow_srv_desc.Texture2DArray.PlaneSlice = 0;
    shadow_srv_desc.Texture2DArray.ResourceMinLODClamp = 0.f;

    // Create shadow maps SRVs for each frame.
    m_spotlight_shadowmaps.srv_handle.ptr = m_gpu.csu_allocator.allocate();
    m_gpu.device->CreateShaderResourceView(m_spotlight_shadowmaps.default_resource.Get(),
                                           &shadow_srv_desc,
                                           m_spotlight_shadowmaps.srv_handle);
}

void particles_graphics::create_pointlight_shadowmaps()
{
    // Create shadow texture.
    size_t num_shadowmap_faces = num_pointlight_cube_shadowmaps * 6;
    D3D12_RESOURCE_DESC shadow_tex_desc = {};
    shadow_tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    shadow_tex_desc.DepthOrArraySize = (UINT16)num_shadowmap_faces;
    shadow_tex_desc.MipLevels = 1;
    shadow_tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    shadow_tex_desc.Format = shadow_texture_format_alias; // Typeless format will get cast to D32 and R32.
    shadow_tex_desc.SampleDesc.Count = 1;
    shadow_tex_desc.SampleDesc.Quality = 0;
    shadow_tex_desc.Width = shadowmap_width;
    shadow_tex_desc.Height = shadowmap_height;

    D3D12_RESOURCE_ALLOCATION_INFO shadowmap_alloc_info;
    shadowmap_alloc_info = m_gpu.device->GetResourceAllocationInfo(0, 1, &shadow_tex_desc);
    shadow_tex_desc.Alignment = shadowmap_alloc_info.Alignment;

    // Create the heap that will hold the shadow map default resources.
    D3D12_HEAP_PROPERTIES heap_props = {};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_DESC shadows_heap_desc = {};
    shadows_heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    shadows_heap_desc.Flags = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED; // We always ClearDepthStencil() before writing to resources in this heap.
    shadows_heap_desc.Properties = heap_props;
    shadows_heap_desc.SizeInBytes = shadowmap_alloc_info.SizeInBytes;

    D3D12_CLEAR_VALUE clear_value;
    clear_value.Format = shadow_texture_format_dsv;
    clear_value.DepthStencil.Depth = 1.0f;
    clear_value.DepthStencil.Stencil = 0;

    // Create the shadow maps resources.
    check_hr(m_gpu.device->CreateCommittedResource(&heap_props,
                                                   shadows_heap_desc.Flags,
                                                   &shadow_tex_desc,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear_value,
                                                   IID_PPV_ARGS(m_pointlight_shadowmaps.default_resource.GetAddressOf())));
    NAME_D3D12_OBJECT(m_pointlight_shadowmaps.default_resource);

    // Create a DSV that contains all of the shadow maps.
    m_pointlight_shadowmaps.dsv_handle.ptr = m_gpu.dsv_allocator.allocate();
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {};
    dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
    dsv_desc.Format = shadow_texture_format_dsv;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
    dsv_desc.Texture2DArray.ArraySize = (UINT)num_shadowmap_faces;
    dsv_desc.Texture2DArray.FirstArraySlice = 0;
    dsv_desc.Texture2DArray.MipSlice = 0;
    m_gpu.device->CreateDepthStencilView(m_pointlight_shadowmaps.default_resource.Get(), &dsv_desc, m_pointlight_shadowmaps.dsv_handle);

    // Create an SRV.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_cube_shadowmaps_desc = {};
    srv_cube_shadowmaps_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    srv_cube_shadowmaps_desc.Format = shadow_texture_format_srv;
    srv_cube_shadowmaps_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_cube_shadowmaps_desc.TextureCubeArray.MipLevels = 1;
    srv_cube_shadowmaps_desc.TextureCubeArray.MostDetailedMip = 0;
    srv_cube_shadowmaps_desc.TextureCubeArray.ResourceMinLODClamp = 0.f;
    srv_cube_shadowmaps_desc.TextureCubeArray.First2DArrayFace = 0;
    srv_cube_shadowmaps_desc.TextureCubeArray.NumCubes = num_pointlight_cube_shadowmaps;

    m_pointlight_shadowmaps.srv_handle.ptr = m_gpu.csu_allocator.allocate();
    m_gpu.device->CreateShaderResourceView(m_pointlight_shadowmaps.default_resource.Get(),
                                           &srv_cube_shadowmaps_desc,
                                           m_pointlight_shadowmaps.srv_handle);
}

void particles_graphics::shadowmap_worker(int thread_index, spot_light *spotlights, int num_spotlights)
{
    assert(thread_index >= 0);
    assert(thread_index < G_NUM_SHADOW_THREADS);

    while (thread_index >= 0 && thread_index < G_NUM_SHADOW_THREADS)
    {
        // Wait for main thread to tell us to begin.
        WaitForSingleObject(begin_shadowpass_events[thread_index], INFINITE);

        ComPtr<ID3D12CommandAllocator> shadow_cmdalloc = shadow_cmdallocs[m_gpu.frame_index][thread_index];
        ComPtr<ID3D12GraphicsCommandList> shadow_cmdlist = shadow_cmdlists[m_gpu.frame_index][thread_index];

        m_gpu.timer_start(shadow_cmdlist, "Shadow pass");
        m_gpu.set_staging_heaps(shadow_cmdlist);
        shadow_cmdlist->SetGraphicsRootSignature(m_graphics_rootsig.Get());
        shadow_cmdlist->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Set a square scissor rect and viewport.
        D3D12_RECT shadow_rect;
        shadow_rect.left = 0;
        shadow_rect.top = 0;
        shadow_rect.right = (LONG)shadowmap_width;
        shadow_rect.bottom = (LONG)shadowmap_height;

        D3D12_VIEWPORT shadow_vp;
        shadow_vp.TopLeftY = 0;
        shadow_vp.TopLeftX = 0;
        shadow_vp.Width = (float)shadowmap_width;
        shadow_vp.Height = (float)shadowmap_height;
        shadow_vp.MaxDepth = 1.0f;
        shadow_vp.MinDepth = 0.0f;

        shadow_cmdlist->RSSetViewports(1, &shadow_vp);
        shadow_cmdlist->RSSetScissorRects(1, &shadow_rect);

        shadow_cmdlist->SetGraphicsRootConstantBufferView(10, m_shadowmap_cb_vs.default_resource->GetGPUVirtualAddress());

        // Draw to the shadow maps for each light.
        for (size_t i = 0; i < num_spotlights; i++)
        {
            spot_light *light = &spotlights[i];

            // Set the next shadow map for depth writes.
            shadow_cmdlist->OMSetRenderTargets(0, nullptr, false, &m_spotlight_shadowmaps.array_dsv_handles[light->id]);

            // Calculate the current spotlight's view-projection matrix.
            XMVECTOR light_up = XMVectorSet(0.f, 0.f, 1.f, 0.f); // All spotlights will just face downwards for now.
            XMVECTOR light_pos = XMVectorSet(light->position_ws.x, light->position_ws.y, light->position_ws.z, 1.f);
            XMVECTOR light_focus_pos = XMVectorAdd(light_pos, XMLoadFloat3(&light->direction));

            XMMATRIX light_view = XMMatrixLookAtLH(light_pos, light_focus_pos, light_up);
            XMMATRIX light_proj = XMMatrixPerspectiveFovLH(DirectX::XM_PI * 0.5f, 1.f, light->falloff_start, light->falloff_end);
            XMMATRIX light_viewproj = light_view * light_proj;

            XMStoreFloat4x4(&light->view_proj, XMMatrixTranspose(light_viewproj));

            // Draw shadow casters.
            for (auto &ro_pair : m_render_objects)
            {
                draw_render_objects(shadow_cmdlist, &m_shadowmap_cb_vs, nullptr, &ro_pair.second, 1, light_viewproj, false);
            }
        }

        // Update light data.
        mtx.lock();
        m_spotlights_sb.update(spotlights, num_spotlights, thread_index, shadow_cmdlist);
        mtx.unlock();

        // Execute shadow pass.
        m_gpu.timer_stop(shadow_cmdlist, "Shadow pass");
        check_hr(shadow_cmdlist->Close());
        m_gpu.graphics_cmd_queue->ExecuteCommandLists(1, (ID3D12CommandList *const *)shadow_cmdlist.GetAddressOf());
        SetEvent(end_shadowpass_events[thread_index]);
    }
}

DWORD __stdcall shadow_worker_thunk(void *param)
{
    particles_graphics::shadow_thunk_parameter *params = reinterpret_cast<particles_graphics::shadow_thunk_parameter *>(param);
    params->graphics->shadowmap_worker(params->thread_index, params->spotlights, params->num_spotlights);
    return 0;
}

void particles_graphics::create_shadowmap_thread_contexts()
{
    for (int thread_index = 0; thread_index < G_NUM_SHADOW_THREADS; thread_index++)
    {
        per_thread_sl[thread_index] = &spotlights[thread_index * G_NUM_LIGHTS_PER_THREAD];

        // Create events.
        begin_shadowpass_events[thread_index] = CreateEvent(nullptr, false, false, nullptr);
        end_shadowpass_events[thread_index] = CreateEvent(nullptr, false, false, nullptr);

        // Create threads.
        shadow_thunk_parameters[thread_index].graphics = this;
        shadow_thunk_parameters[thread_index].thread_index = thread_index;
        shadow_thunk_parameters[thread_index].spotlights = per_thread_sl[thread_index];
        shadow_thunk_parameters[thread_index].num_spotlights = G_NUM_LIGHTS_PER_THREAD;
        shadow_thread_handles[thread_index] = CreateThread(nullptr, 0,
                                                           shadow_worker_thunk, reinterpret_cast<void *>(&shadow_thunk_parameters[thread_index]),
                                                           0, nullptr);
        std::wstring thread_desc = L"shadow worker #" + std::to_wstring(thread_index);
        check_hr(SetThreadDescription(shadow_thread_handles[thread_index], thread_desc.c_str()));
    }
}

DWORD __stdcall compute_worker_thunk(void *param)
{
    particles_graphics::compute_thunk_parameter *params = reinterpret_cast<particles_graphics::compute_thunk_parameter *>(param);
    params->graphics->compute_worker(params->thread_index);
    return 0;
}

void particles_graphics::create_compute_thread_contexts()
{
    for (int thread_index = 0; thread_index < G_NUM_COMPUTE_THREADS; thread_index++)
    {
        // Create events.
        begin_compute_events[thread_index] = CreateEvent(nullptr, false, false, nullptr);
        end_compute_events[thread_index] = CreateEvent(nullptr, false, false, nullptr);

        // Create threads.
        compute_thunk_parameters[thread_index].graphics = this;
        compute_thunk_parameters[thread_index].thread_index = thread_index;
        compute_thread_handles[thread_index] = CreateThread(nullptr, 0,
                                                            compute_worker_thunk, reinterpret_cast<void *>(&compute_thunk_parameters[thread_index]),
                                                            0, nullptr);
        std::wstring thread_desc = L"compute worker #" + std::to_wstring(thread_index);
        check_hr(SetThreadDescription(compute_thread_handles[thread_index], thread_desc.c_str()));
    }
}

void particles_graphics::compute_worker(int thread_index)
{
    assert(thread_index >= 0);
    assert(thread_index < G_NUM_COMPUTE_THREADS);

    while (thread_index >= 0 && thread_index < G_NUM_COMPUTE_THREADS)
    {
        WaitForSingleObject(begin_compute_events[thread_index], INFINITE);
        ComPtr<ID3D12GraphicsCommandList> compute_cmdlist = compute_cmdlists[m_gpu.frame_index][thread_index];
        m_gpu.set_staging_heaps(compute_cmdlist);
        compute_cmdlist->SetComputeRootSignature(m_compute_rootsig.Get());

        particle_simcmds_default.Swap(particle_simcmds_swap_default);

        compute_cmdlist->SetComputeRootShaderResourceView(5, particle_simcmds_default->GetGPUVirtualAddress());
        compute_cmdlist->SetComputeRootConstantBufferView(3, m_pass_cb.default_resource->GetGPUVirtualAddress());
        compute_cmdlist->SetComputeRootDescriptorTable(2, compute_descriptors_base_gpu_handle[m_gpu.frame_index]);

        m_gpu.transition(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                         D3D12_RESOURCE_STATE_COPY_DEST,
                         {particle_simcmds_counter_default[m_gpu.frame_index],
                          particle_drawcmds_counter_default[m_gpu.frame_index],
                          m_particle_system_info.default_resource},
                         compute_cmdlist);
        m_gpu.transition(D3D12_RESOURCE_STATE_COPY_SOURCE,
                         D3D12_RESOURCE_STATE_COPY_DEST,
                         {m_attractors_sb.default_resource},
                         compute_cmdlist);

        // Reset counters.
        compute_cmdlist->CopyBufferRegion(m_particle_lights_counter.default_resource.Get(), 0,
                                          reset_counter_default.Get(), 0,
                                          sizeof(UINT));
        compute_cmdlist->CopyResource(particle_simcmds_counter_default[m_gpu.frame_index].Get(), reset_counter_default.Get());
        compute_cmdlist->CopyResource(particle_drawcmds_counter_default[m_gpu.frame_index].Get(), reset_counter_default.Get());

        // Update attractors data.
        for (size_t i = 0; i < num_particle_systems; i++)
        {
            attractor_point_light *attractor = &attractors[i];

            // Update the light position.

            float time = (float)g_cpu_timer.get_current_time();
            transform t;
            t.set_translation(attractor->light.position_ws.x,
                              attractor->light.position_ws.y + (sinf(time) * 0.0003f),
                              attractor->light.position_ws.z);
            attractor->world = t.m_transposed_world;
            attractor->light.position_ws = t.m_translation;

            XMVECTOR light_pos = XMLoadFloat3(&attractor->light.position_ws);
            XMMATRIX light_proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(90.f),
                                                           1.f,
                                                           attractor->light.falloff_start,
                                                           attractor->light.falloff_end);

            // +X
            XMVECTOR light_up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
            XMVECTOR face_dir = XMVectorSet(1.f, 0.f, 0.f, 0.f);
            XMVECTOR target_pos = XMVectorAdd(face_dir, light_pos);
            XMMATRIX light_view = XMMatrixLookAtLH(light_pos, target_pos, light_up);
            XMStoreFloat4x4(&attractor->view_proj[0], XMMatrixTranspose(light_view * light_proj));

            // -X
            face_dir = XMVectorSet(-1.f, 0.f, 0.f, 0.f);
            target_pos = XMVectorAdd(face_dir, light_pos);
            light_view = XMMatrixLookAtLH(light_pos, target_pos, light_up);
            XMStoreFloat4x4(&attractor->view_proj[1], XMMatrixTranspose(light_view * light_proj));

            // +Y
            light_up = XMVectorSet(0.f, 0.f, -1.f, 0.f);
            face_dir = XMVectorSet(0.f, 1.f, 0.f, 0.f);
            target_pos = XMVectorAdd(face_dir, light_pos);
            light_view = XMMatrixLookAtLH(light_pos, target_pos, light_up);
            XMStoreFloat4x4(&attractor->view_proj[2], XMMatrixTranspose(light_view * light_proj));

            // -Y
            light_up = XMVectorSet(0.f, 0.f, 1.f, 0.f);
            face_dir = XMVectorSet(0.f, -1.f, 0.f, 0.f);
            target_pos = XMVectorAdd(face_dir, light_pos);
            light_view = XMMatrixLookAtLH(light_pos, target_pos, light_up);
            XMStoreFloat4x4(&attractor->view_proj[3], XMMatrixTranspose(light_view * light_proj));

            // +Z
            light_up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
            face_dir = XMVectorSet(0.f, 0.f, 1.f, 0.f);
            target_pos = XMVectorAdd(face_dir, light_pos);
            light_view = XMMatrixLookAtLH(light_pos, target_pos, light_up);
            XMStoreFloat4x4(&attractor->view_proj[4], XMMatrixTranspose(light_view * light_proj));

            // -Z
            face_dir = XMVectorSet(0.f, 0.f, -1.f, 0.f);
            target_pos = XMVectorAdd(face_dir, light_pos);
            light_view = XMMatrixLookAtLH(light_pos, target_pos, light_up);
            XMStoreFloat4x4(&attractor->view_proj[5], XMMatrixTranspose(light_view * light_proj));

            // Update volume lights.
            volume_light *vl = &volume_lights[i];
            vl->m_transform = t;
        }
        m_attractors_sb.update(attractors, compute_cmdlist);

        // Update particle systems data.
        m_particle_system_info.update(particle_systems_infos, compute_cmdlist);

        m_gpu.transition(D3D12_RESOURCE_STATE_COPY_DEST,
                         D3D12_RESOURCE_STATE_COPY_SOURCE,
                         {m_attractors_sb.default_resource},
                         compute_cmdlist);

        // Update attractor world matrix.
        for (size_t i = 0; i < num_particle_systems; i++)
        {
            // Assign the world transform of each attractor to the world transform of each particle system.
            size_t ps_offset = (i * sizeof(particle_system_info));
            size_t dst_offset = ps_offset + offsetof(particle_system_info, world);
            size_t pl_offset = (i * sizeof(attractor_point_light));
            size_t src_offset = pl_offset + offsetof(attractor_point_light, world);
            compute_cmdlist->CopyBufferRegion(m_particle_system_info.default_resource.Get(), dst_offset,
                                              m_attractors_sb.default_resource.Get(), src_offset,
                                              sizeof(particle_system_info::world));
        }

        m_gpu.transition(D3D12_RESOURCE_STATE_COPY_DEST,
                         D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                         {particle_simcmds_counter_default[m_gpu.frame_index],
                          particle_drawcmds_counter_default[m_gpu.frame_index],
                          m_particle_system_info.default_resource},
                         compute_cmdlist);
        m_gpu.transition(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                         {bounds_vertices_resource},
                         compute_cmdlist);
        m_gpu.transition(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         {particle_simcmds_filtered_default,
                          particle_drawcmds_filtered_default},
                         compute_cmdlist);

        // Frustum culling of commands.
        m_gpu.timer_start(compute_cmdlist, "Frustum culling of commands");
        compute_cmdlist->SetPipelineState(m_PSOs[commands_culling_PSO]);
        compute_cmdlist->Dispatch(1, 1, 1);
        m_gpu.timer_stop(compute_cmdlist, "Frustum culling of commands");

        m_gpu.transition(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                         {bounds_vertices_resource},
                         compute_cmdlist);
        m_gpu.transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                         {particle_simcmds_filtered_default,
                          particle_drawcmds_filtered_default},
                         compute_cmdlist);

        // Particle simulation.
        m_gpu.timer_start(compute_cmdlist, "Particle simulation");
        compute_cmdlist->SetPipelineState(m_PSOs[particle_sim_PSO]);
        compute_cmdlist->ExecuteIndirect(particle_sim_cmdsig.Get(), num_particle_systems,
                                         particle_simcmds_filtered_default.Get(), 0,
                                         particle_simcmds_counter_default[m_gpu.frame_index].Get(), 0);
        m_gpu.timer_stop(compute_cmdlist, "Particle simulation");

        m_gpu.transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                         {particle_output_default},
                         compute_cmdlist);

        // Update particle bounds.
        m_gpu.timer_start(compute_cmdlist, "Update particle bounds");
        compute_cmdlist->SetPipelineState(m_PSOs[calculate_bounds_PSO]);
        compute_cmdlist->ExecuteIndirect(bounds_calc_cmdsig.Get(), num_particle_systems,
                                         bounds_calc_cmds_default.Get(), 0,
                                         nullptr, 0);
        m_gpu.timer_stop(compute_cmdlist, "Update particle bounds");

        SetEvent(end_compute_events[thread_index]);
    }
}

void particles_graphics::create_bounds_calculations_commands(ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    // Create the bounds calculation commands.
    struct calc_bounds_command
    {
        D3D12_GPU_VIRTUAL_ADDRESS cbv_particle_system_info;
        D3D12_DISPATCH_ARGUMENTS dispatch_args;
    };

    // Create the bounds calculation command signature.
    D3D12_INDIRECT_ARGUMENT_DESC bounds_calc_indirect_args[2] = {};
    bounds_calc_indirect_args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
    bounds_calc_indirect_args[0].ConstantBufferView.RootParameterIndex = 4;
    bounds_calc_indirect_args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

    D3D12_COMMAND_SIGNATURE_DESC bounds_calc_cmdsig_desc = {};
    bounds_calc_cmdsig_desc.NumArgumentDescs = _countof(bounds_calc_indirect_args);
    bounds_calc_cmdsig_desc.pArgumentDescs = bounds_calc_indirect_args;
    bounds_calc_cmdsig_desc.ByteStride = (UINT)sizeof(calc_bounds_command);
    check_hr(m_gpu.device->CreateCommandSignature(&bounds_calc_cmdsig_desc, m_compute_rootsig.Get(),
                                                  IID_PPV_ARGS(bounds_calc_cmdsig.GetAddressOf())));
    NAME_D3D12_OBJECT(bounds_calc_cmdsig);

    // Create bounds calculations commands.
    D3D12_DISPATCH_ARGUMENTS dispatch_args = {num_sim_threadgroups, 1, 1};
    calc_bounds_command calc_bounds_cmds[num_particle_systems];
    for (int i = 0; i < num_particle_systems; i++)
    {
        // SetGraphicsRootConstantBufferView.
        calc_bounds_cmds[i].cbv_particle_system_info = m_particle_system_info.default_resource->GetGPUVirtualAddress() +
                                                       (sizeof(particle_system_info) * i);

        // Dispatch.
        calc_bounds_cmds[i].dispatch_args = dispatch_args;
    }

    // Create the resource to hold the bounds calculation commands.
    m_gpu.default_resource_from_uploader(cmd_list, bounds_calc_cmds_default.GetAddressOf(),
                                         calc_bounds_cmds, num_particle_systems * sizeof(calc_bounds_command),
                                         sizeof(calc_bounds_command),
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    NAME_D3D12_OBJECT(bounds_calc_cmds_default);
}

void particles_graphics::create_bounds_draw_commands(ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    struct draw_bounds_command
    {
        D3D12_GPU_VIRTUAL_ADDRESS particle_draw_info;
        D3D12_INDEX_BUFFER_VIEW ibv;
        D3D12_VERTEX_BUFFER_VIEW vbv;
        D3D12_DRAW_INDEXED_ARGUMENTS draw_indexed_args;
    };

    // Create the bounds drawing command signature.
    D3D12_INDIRECT_ARGUMENT_DESC bounds_draw_indirect_args[4] = {};
    bounds_draw_indirect_args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
    bounds_draw_indirect_args[0].ConstantBufferView.RootParameterIndex = 13;
    bounds_draw_indirect_args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
    bounds_draw_indirect_args[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
    bounds_draw_indirect_args[2].VertexBuffer.Slot = 0;
    bounds_draw_indirect_args[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    D3D12_COMMAND_SIGNATURE_DESC bounds_draw_cmdsig_desc = {};
    bounds_draw_cmdsig_desc.NumArgumentDescs = _countof(bounds_draw_indirect_args);
    bounds_draw_cmdsig_desc.pArgumentDescs = bounds_draw_indirect_args;
    bounds_draw_cmdsig_desc.ByteStride = (UINT)sizeof(draw_bounds_command);
    check_hr(m_gpu.device->CreateCommandSignature(&bounds_draw_cmdsig_desc, m_graphics_rootsig.Get(),
                                                  IID_PPV_ARGS(bounds_draw_cmdsig.GetAddressOf())));
    NAME_D3D12_OBJECT(bounds_draw_cmdsig);

    // Create bounds drawing commands.
    draw_bounds_command draw_bounds_cmds[num_particle_systems];
    D3D12_DRAW_INDEXED_ARGUMENTS bounds_draw_indexed_args = {};
    bounds_draw_indexed_args.InstanceCount = 1;
    bounds_draw_indexed_args.IndexCountPerInstance = (UINT)num_bb_indices;

    size_t positions_size = sizeof(XMFLOAT3);
    size_t all_positions_size = positions_size * 8;
    size_t bounding_box_size = sizeof(bounding_box);
    size_t offset_to_bb_positions = offsetof(bounding_box, positions);
    size_t draw_bounds_cmd_size = sizeof(draw_bounds_command);
    size_t num_draw_bounds_cmds = num_particle_systems * draw_bounds_cmd_size;

    for (int i = 0; i < num_particle_systems; i++)
    {
        // SetGraphicsRootConstantBufferView.
        draw_bounds_cmds[i].particle_draw_info = m_particle_system_info.default_resource->GetGPUVirtualAddress() +
                                                 (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT * i);

        // IASetIndexBuffer.
        draw_bounds_cmds[i].ibv = bb_ibv;

        // IASetVertexBuffer.
        draw_bounds_cmds[i].vbv.BufferLocation = bounds_vertices_resource->GetGPUVirtualAddress() +
                                                 (bounding_box_size * i) +
                                                 offset_to_bb_positions;
        draw_bounds_cmds[i].vbv.StrideInBytes = (UINT)positions_size;
        draw_bounds_cmds[i].vbv.SizeInBytes = (UINT)all_positions_size;

        // DrawIndexedInstanced.
        draw_bounds_cmds[i].draw_indexed_args = bounds_draw_indexed_args;
    }
    // Create the resource to hold the bounds drawing commands.
    m_gpu.default_resource_from_uploader(cmd_list, bounds_drawcmds_default.GetAddressOf(),
                                         draw_bounds_cmds, num_draw_bounds_cmds,
                                         draw_bounds_cmd_size,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    NAME_D3D12_OBJECT(bounds_drawcmds_default);
}

void particles_graphics::create_PSOs()
{
    // Alpha transparency blending (blend)
    D3D12_RENDER_TARGET_BLEND_DESC transparency_rtv_blend_desc = {};
    transparency_rtv_blend_desc.BlendEnable = true;
    transparency_rtv_blend_desc.LogicOpEnable = false;
    transparency_rtv_blend_desc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Enable conventional alpha blending.
    // C = (C_src * C_src_alpha) + (C_dst * (1 - C_src_alpha))
    transparency_rtv_blend_desc.BlendOp = D3D12_BLEND_OP_ADD;
    transparency_rtv_blend_desc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    transparency_rtv_blend_desc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;

    // Don't blend alpha values.
    transparency_rtv_blend_desc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    transparency_rtv_blend_desc.DestBlendAlpha = D3D12_BLEND_ZERO;
    transparency_rtv_blend_desc.SrcBlendAlpha = D3D12_BLEND_ONE;

    // Additive blending.
    D3D12_RENDER_TARGET_BLEND_DESC additive_rtv_blend_desc = transparency_rtv_blend_desc;
    // C = (C_src * (1,1,1,1)) + (C_dst * (1,1,1,1))
    additive_rtv_blend_desc.BlendOp = D3D12_BLEND_OP_ADD;
    additive_rtv_blend_desc.SrcBlend = D3D12_BLEND_ONE;
    additive_rtv_blend_desc.DestBlend = D3D12_BLEND_ONE;

    // Additive blending with alpha.
    D3D12_RENDER_TARGET_BLEND_DESC additive_transparency_rtv_blend_desc = additive_rtv_blend_desc;
    additive_transparency_rtv_blend_desc.BlendOp = D3D12_BLEND_OP_ADD;
    additive_transparency_rtv_blend_desc.DestBlend = D3D12_BLEND_ONE;
    additive_transparency_rtv_blend_desc.SrcBlend = D3D12_BLEND_SRC_ALPHA;

    D3D12_BLEND_DESC transparency_blend_desc = {};
    transparency_blend_desc.AlphaToCoverageEnable = false;
    transparency_blend_desc.IndependentBlendEnable = true;
    transparency_blend_desc.RenderTarget[0] = transparency_rtv_blend_desc;

    // Disable blending for gbuffer1 and gbuffer2.
    D3D12_RENDER_TARGET_BLEND_DESC disabled_blend_rt_blend_desc = transparency_rtv_blend_desc;
    disabled_blend_rt_blend_desc.BlendEnable = false;
    transparency_blend_desc.RenderTarget[1] = disabled_blend_rt_blend_desc;
    transparency_blend_desc.RenderTarget[2] = disabled_blend_rt_blend_desc;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC default_pso_desc = m_gpu.create_default_pso_desc();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC simplepbr_pso_desc = default_pso_desc;
    ID3D10Blob *shader_blob_vs = nullptr;
    ID3D10Blob *shader_blob_gs = nullptr;
    ID3D10Blob *shader_blob_ps = nullptr;
    ID3D10Blob *shader_blob_cs = nullptr;

    D3D_SHADER_MACRO gs_macros[2] = {"GEOMETRY_SHADER", "0"};
    D3D_SHADER_MACRO vs_macros[2] = {"VERTEX_SHADER", "1"};
    D3D_SHADER_MACRO ps_macros[2] = {"PIXEL_SHADER", "1"};

    // Input layouts.
    D3D12_INPUT_ELEMENT_DESC particle_input_layout[4] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"SIZE", 0, DXGI_FORMAT_R32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"VELOCITY", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"AGE", 0, DXGI_FORMAT_R32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    D3D12_INPUT_ELEMENT_DESC standard_inputlayout[5] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BITANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    D3D12_INPUT_ELEMENT_DESC debug_inputlayout[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    // Simple pbr PSO.
    m_gpu.compile_shader(L"..\\particles\\shaders\\pbr_simple.hlsl", L"vs_main", VS, &shader_blob_vs, vs_macros);
    m_gpu.compile_shader(L"..\\particles\\shaders\\pbr_simple.hlsl", L"ps_main", PS, &shader_blob_ps, ps_macros);
    simplepbr_pso_desc.VS = {shader_blob_vs->GetBufferPointer(), shader_blob_vs->GetBufferSize()};
    simplepbr_pso_desc.PS = {shader_blob_ps->GetBufferPointer(), shader_blob_ps->GetBufferSize()};
    simplepbr_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    simplepbr_pso_desc.NumRenderTargets = 3;
    simplepbr_pso_desc.BlendState = transparency_blend_desc;
    simplepbr_pso_desc.RTVFormats[0] = gbuffer0_format;
    simplepbr_pso_desc.RTVFormats[1] = gbuffer1_format;
    simplepbr_pso_desc.RTVFormats[2] = gbuffer2_format;
    simplepbr_pso_desc.DSVFormat = depthstencil_format;
    simplepbr_pso_desc.InputLayout = {standard_inputlayout, _countof(standard_inputlayout)};
    simplepbr_pso_desc.pRootSignature = m_graphics_rootsig.Get();
    ID3D12PipelineState *pbr_simple_pso = nullptr;
    check_hr(m_gpu.device->CreateGraphicsPipelineState(&simplepbr_pso_desc, IID_PPV_ARGS(&pbr_simple_pso)));
    NAME_D3D12_OBJECT(pbr_simple_pso);
    m_PSOs[pbr_simple_PSO] = pbr_simple_pso;

    // Lighting pass PSO.
    m_gpu.compile_shader(L"..\\particles\\shaders\\lighting_pass.hlsl", L"vs_main", VS, &shader_blob_vs, vs_macros);
    m_gpu.compile_shader(L"..\\particles\\shaders\\lighting_pass.hlsl", L"ps_main", PS, &shader_blob_ps, ps_macros);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC lighting_pass_pso_desc = simplepbr_pso_desc;
    lighting_pass_pso_desc.VS = {shader_blob_vs->GetBufferPointer(), shader_blob_vs->GetBufferSize()};
    lighting_pass_pso_desc.PS = {shader_blob_ps->GetBufferPointer(), shader_blob_ps->GetBufferSize()};
    lighting_pass_pso_desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    lighting_pass_pso_desc.RasterizerState.FrontCounterClockwise = true;
    lighting_pass_pso_desc.DepthStencilState.DepthEnable = false;
    lighting_pass_pso_desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    lighting_pass_pso_desc.NumRenderTargets = 1;
    lighting_pass_pso_desc.RTVFormats[0] = hdr_buffer_format;
    lighting_pass_pso_desc.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
    lighting_pass_pso_desc.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
    lighting_pass_pso_desc.InputLayout = {};
    ID3D12PipelineState *lighting_pass_pso = nullptr;
    check_hr(m_gpu.device->CreateGraphicsPipelineState(&lighting_pass_pso_desc, IID_PPV_ARGS(&lighting_pass_pso)));
    NAME_D3D12_OBJECT(lighting_pass_pso);
    m_PSOs[lighting_pass_PSO] = lighting_pass_pso;

    // Shadow pass PSO.
    m_gpu.compile_shader(L"..\\particles\\shaders\\shadow_pass.hlsl", L"vs_main", VS, &shader_blob_vs, vs_macros);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadow_pass_pso_desc = default_pso_desc;
    shadow_pass_pso_desc.InputLayout = {standard_inputlayout, _countof(standard_inputlayout)};
    shadow_pass_pso_desc.pRootSignature = m_graphics_rootsig.Get();
    shadow_pass_pso_desc.VS = {shader_blob_vs->GetBufferPointer(), shader_blob_vs->GetBufferSize()};
    shadow_pass_pso_desc.DSVFormat = shadow_texture_format_dsv;
    shadow_pass_pso_desc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    shadow_pass_pso_desc.RasterizerState.DepthBias = 100000;
    shadow_pass_pso_desc.RasterizerState.DepthBiasClamp = 0.0f;
    shadow_pass_pso_desc.RasterizerState.SlopeScaledDepthBias = 1.0f;
    shadow_pass_pso_desc.NumRenderTargets = 0;
    ID3D12PipelineState *shadow_pass_pso = nullptr;
    check_hr(m_gpu.device->CreateGraphicsPipelineState(&shadow_pass_pso_desc, IID_PPV_ARGS(&shadow_pass_pso)));
    NAME_D3D12_OBJECT(shadow_pass_pso);
    m_PSOs[shadow_pass_PSO] = shadow_pass_pso;

    // Cube shadow pass PSO.
    m_gpu.compile_shader(L"..\\particles\\shaders\\pointlight_shadow.hlsl", L"vs_main", VS, &shader_blob_vs, vs_macros);
    m_gpu.compile_shader(L"..\\particles\\shaders\\pointlight_shadow.hlsl", L"gs_main", GS, &shader_blob_gs, gs_macros);
    m_gpu.compile_shader(L"..\\particles\\shaders\\pointlight_shadow.hlsl", L"ps_main", PS, &shader_blob_ps, ps_macros);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC indirect_shadow_pass_pso_desc = default_pso_desc;
    indirect_shadow_pass_pso_desc.VS = {shader_blob_vs->GetBufferPointer(), shader_blob_vs->GetBufferSize()};
    indirect_shadow_pass_pso_desc.GS = {shader_blob_gs->GetBufferPointer(), shader_blob_gs->GetBufferSize()};
    indirect_shadow_pass_pso_desc.PS = {shader_blob_ps->GetBufferPointer(), shader_blob_ps->GetBufferSize()};
    indirect_shadow_pass_pso_desc.InputLayout = {standard_inputlayout, _countof(standard_inputlayout)};
    indirect_shadow_pass_pso_desc.pRootSignature = m_graphics_rootsig.Get();
    indirect_shadow_pass_pso_desc.DSVFormat = shadow_texture_format_dsv;
    indirect_shadow_pass_pso_desc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    indirect_shadow_pass_pso_desc.NumRenderTargets = 0;
    indirect_shadow_pass_pso_desc.DepthStencilState.DepthEnable = true;
    indirect_shadow_pass_pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    indirect_shadow_pass_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    ID3D12PipelineState *indirect_shadow_pass_pso = nullptr;
    check_hr(m_gpu.device->CreateGraphicsPipelineState(&indirect_shadow_pass_pso_desc, IID_PPV_ARGS(&indirect_shadow_pass_pso)));
    NAME_D3D12_OBJECT(indirect_shadow_pass_pso);
    m_PSOs[cube_shadow_pass_PSO] = indirect_shadow_pass_pso;

    // Solid color PSO.
    m_gpu.compile_shader(L"..\\particles\\shaders\\solid_color.hlsl", L"vs_main", VS, &shader_blob_vs, vs_macros);
    m_gpu.compile_shader(L"..\\particles\\shaders\\solid_color.hlsl", L"ps_main", PS, &shader_blob_ps, ps_macros);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC solid_color_pso_desc = simplepbr_pso_desc;
    solid_color_pso_desc.VS = {shader_blob_vs->GetBufferPointer(), shader_blob_vs->GetBufferSize()};
    solid_color_pso_desc.PS = {shader_blob_ps->GetBufferPointer(), shader_blob_ps->GetBufferSize()};
    solid_color_pso_desc.NumRenderTargets = 1;
    solid_color_pso_desc.RTVFormats[0] = hdr_buffer_format;
    solid_color_pso_desc.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
    solid_color_pso_desc.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
    ID3D12PipelineState *solid_color_pso = nullptr;
    check_hr(m_gpu.device->CreateGraphicsPipelineState(&solid_color_pso_desc, IID_PPV_ARGS(&solid_color_pso)));
    NAME_D3D12_OBJECT(solid_color_pso);
    m_PSOs[solid_color_PSO] = solid_color_pso;

    // Debug line PSO.
    m_gpu.compile_shader(L"..\\particles\\shaders\\debug_line.hlsl", L"vs_main", VS, &shader_blob_vs, vs_macros);
    m_gpu.compile_shader(L"..\\particles\\shaders\\debug_line.hlsl", L"ps_main", PS, &shader_blob_ps, ps_macros);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC debug_line_pso_desc = solid_color_pso_desc;
    debug_line_pso_desc.VS = {shader_blob_vs->GetBufferPointer(), shader_blob_vs->GetBufferSize()};
    debug_line_pso_desc.PS = {shader_blob_ps->GetBufferPointer(), shader_blob_ps->GetBufferSize()};
    debug_line_pso_desc.InputLayout = {debug_inputlayout, _countof(debug_inputlayout)};
    debug_line_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    debug_line_pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // Turn off writes to the depth buffer.
    ID3D12PipelineState *debug_line_pso = nullptr;
    check_hr(m_gpu.device->CreateGraphicsPipelineState(&debug_line_pso_desc, IID_PPV_ARGS(&debug_line_pso)));
    NAME_D3D12_OBJECT(debug_line_pso);
    m_PSOs[debug_line_PSO] = debug_line_pso;

    // Debug plane PSO.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC debug_plane_pso_desc = debug_line_pso_desc;
    debug_plane_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    debug_plane_pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    debug_plane_pso_desc.BlendState = transparency_blend_desc;
    ID3D12PipelineState *debug_plane_pso = nullptr;
    check_hr(m_gpu.device->CreateGraphicsPipelineState(&debug_plane_pso_desc, IID_PPV_ARGS(&debug_plane_pso)));
    NAME_D3D12_OBJECT(debug_plane_pso);
    m_PSOs[debug_plane_PSO] = debug_plane_pso;

    // Particle simulation PSO.
    D3D_SHADER_MACRO cs_macros[2] = {"COMPUTE", "1"};
    m_gpu.compile_shader(L"..\\particles\\shaders\\particle_sim.hlsl", L"cs_main", CS, &shader_blob_cs, cs_macros);
    D3D12_COMPUTE_PIPELINE_STATE_DESC particle_sim_pso_desc = {};
    particle_sim_pso_desc.CS = {shader_blob_cs->GetBufferPointer(), shader_blob_cs->GetBufferSize()};
    particle_sim_pso_desc.pRootSignature = m_compute_rootsig.Get();
    particle_sim_pso_desc.NodeMask = 0;
    particle_sim_pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    ID3D12PipelineState *particle_sim_pso = nullptr;
    check_hr(m_gpu.device->CreateComputePipelineState(&particle_sim_pso_desc, IID_PPV_ARGS(&particle_sim_pso)));
    NAME_D3D12_OBJECT(particle_sim_pso);
    m_PSOs[particle_sim_PSO] = particle_sim_pso;

    // Calculate particle/light bounds and update light matrices.
    m_gpu.compile_shader(L"..\\particles\\shaders\\update_particles_bounds.hlsl", L"cs_main", CS, &shader_blob_cs, cs_macros);
    D3D12_COMPUTE_PIPELINE_STATE_DESC calc_bounds_pso_desc = particle_sim_pso_desc;
    calc_bounds_pso_desc.CS = {shader_blob_cs->GetBufferPointer(), shader_blob_cs->GetBufferSize()};
    ID3D12PipelineState *calc_bounds_pso = nullptr;
    check_hr(m_gpu.device->CreateComputePipelineState(&calc_bounds_pso_desc, IID_PPV_ARGS(&calc_bounds_pso)));
    NAME_D3D12_OBJECT(calc_bounds_pso);
    m_PSOs[calculate_bounds_PSO] = calc_bounds_pso;

    // Draw particles as points.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC particle_point_draw_pso_desc = default_pso_desc;
    m_gpu.compile_shader(L"..\\particles\\shaders\\particle_point.hlsl", L"vs_main", VS, &shader_blob_vs, vs_macros);
    m_gpu.compile_shader(L"..\\particles\\shaders\\particle_point.hlsl", L"ps_main", PS, &shader_blob_ps, ps_macros);
    particle_point_draw_pso_desc.VS = {shader_blob_vs->GetBufferPointer(), shader_blob_vs->GetBufferSize()};
    particle_point_draw_pso_desc.PS = {shader_blob_ps->GetBufferPointer(), shader_blob_ps->GetBufferSize()};
    particle_point_draw_pso_desc.InputLayout = {particle_input_layout, _countof(particle_input_layout)};
    particle_point_draw_pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // Turn off writes to the depth buffer.
    particle_point_draw_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    particle_point_draw_pso_desc.pRootSignature = m_graphics_rootsig.Get();
    particle_point_draw_pso_desc.NumRenderTargets = 1;
    particle_point_draw_pso_desc.RTVFormats[0] = hdr_buffer_format;
    ID3D12PipelineState *particle_point_draw_pso = nullptr;
    check_hr(m_gpu.device->CreateGraphicsPipelineState(&particle_point_draw_pso_desc, IID_PPV_ARGS(&particle_point_draw_pso)));
    NAME_D3D12_OBJECT(particle_point_draw_pso);
    m_PSOs[particle_point_draw_PSO] = particle_point_draw_pso;

    // Draw bounding box bounds.
    m_gpu.compile_shader(L"..\\particles\\shaders\\bounds_draw.hlsl", L"vs_main", VS, &shader_blob_vs, vs_macros);
    m_gpu.compile_shader(L"..\\particles\\shaders\\bounds_draw.hlsl", L"ps_main", PS, &shader_blob_ps, ps_macros);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC bounds_draw_pso_desc = default_pso_desc;
    bounds_draw_pso_desc.VS = {shader_blob_vs->GetBufferPointer(), shader_blob_vs->GetBufferSize()};
    bounds_draw_pso_desc.PS = {shader_blob_ps->GetBufferPointer(), shader_blob_ps->GetBufferSize()};
    bounds_draw_pso_desc.InputLayout = {debug_inputlayout, _countof(debug_inputlayout)};
    bounds_draw_pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // Turn off writes to the depth buffer.
    bounds_draw_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    bounds_draw_pso_desc.pRootSignature = m_graphics_rootsig.Get();
    bounds_draw_pso_desc.NumRenderTargets = 1;
    bounds_draw_pso_desc.RTVFormats[0] = hdr_buffer_format;
    ID3D12PipelineState *bounds_draw_pso = nullptr;
    check_hr(m_gpu.device->CreateGraphicsPipelineState(&bounds_draw_pso_desc, IID_PPV_ARGS(&bounds_draw_pso)));
    NAME_D3D12_OBJECT(bounds_draw_pso);
    m_PSOs[draw_bounds_PSO] = bounds_draw_pso;

    // Reinhard tonemapping.
    m_gpu.compile_shader(L"..\\particles\\shaders\\reinhard_tonemapping.hlsl", L"vs_main", VS, &shader_blob_vs, vs_macros);
    m_gpu.compile_shader(L"..\\particles\\shaders\\reinhard_tonemapping.hlsl", L"ps_main", PS, &shader_blob_ps, ps_macros);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC tonemapping_pso_desc = default_pso_desc;
    tonemapping_pso_desc.VS = {shader_blob_vs->GetBufferPointer(), shader_blob_vs->GetBufferSize()};
    tonemapping_pso_desc.PS = {shader_blob_ps->GetBufferPointer(), shader_blob_ps->GetBufferSize()};
    tonemapping_pso_desc.RTVFormats[0] = back_buffer_format;
    tonemapping_pso_desc.NumRenderTargets = 1;
    tonemapping_pso_desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    tonemapping_pso_desc.DepthStencilState.DepthEnable = false;
    tonemapping_pso_desc.DepthStencilState.StencilEnable = false;
    tonemapping_pso_desc.pRootSignature = m_graphics_rootsig.Get();
    tonemapping_pso_desc.RasterizerState.FrontCounterClockwise = true;
    ID3D12PipelineState *tonemapping_pso = nullptr;
    check_hr(m_gpu.device->CreateGraphicsPipelineState(&tonemapping_pso_desc, IID_PPV_ARGS(&tonemapping_pso)));
    NAME_D3D12_OBJECT(tonemapping_pso);
    m_PSOs[tonemapping_PSO] = tonemapping_pso;

    // Frustum culling of commands.
    m_gpu.compile_shader(L"..\\particles\\shaders\\filter_commands.hlsl", L"cs_main", CS, &shader_blob_cs, cs_macros);
    D3D12_COMPUTE_PIPELINE_STATE_DESC commands_culling_pso_desc = {};
    commands_culling_pso_desc.CS = {shader_blob_cs->GetBufferPointer(), shader_blob_cs->GetBufferSize()};
    commands_culling_pso_desc.pRootSignature = m_compute_rootsig.Get();
    ID3D12PipelineState *commands_culling_pso = nullptr;
    check_hr(m_gpu.device->CreateComputePipelineState(&commands_culling_pso_desc, IID_PPV_ARGS(&commands_culling_pso)));
    NAME_D3D12_OBJECT(commands_culling_pso);
    m_PSOs[commands_culling_PSO] = commands_culling_pso;

    // Billboarding.
    D3D12_BLEND_DESC additive_blend_desc = {};
    additive_blend_desc.AlphaToCoverageEnable = false;
    additive_blend_desc.IndependentBlendEnable = false;
    additive_blend_desc.RenderTarget[0] = additive_rtv_blend_desc;

    m_gpu.compile_shader(L"..\\particles\\shaders\\billboards.hlsl", L"vs_main", VS, &shader_blob_vs, vs_macros);
    m_gpu.compile_shader(L"..\\particles\\shaders\\billboards.hlsl", L"gs_main", GS, &shader_blob_gs, gs_macros);
    m_gpu.compile_shader(L"..\\particles\\shaders\\billboards.hlsl", L"ps_main", PS, &shader_blob_ps, ps_macros);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC billboards_pso_desc = default_pso_desc;
    billboards_pso_desc.VS = {shader_blob_vs->GetBufferPointer(), shader_blob_vs->GetBufferSize()};
    billboards_pso_desc.GS = {shader_blob_gs->GetBufferPointer(), shader_blob_gs->GetBufferSize()};
    billboards_pso_desc.PS = {shader_blob_ps->GetBufferPointer(), shader_blob_ps->GetBufferSize()};
    billboards_pso_desc.InputLayout = {particle_input_layout, _countof(particle_input_layout)};
    billboards_pso_desc.pRootSignature = m_graphics_rootsig.Get();
    billboards_pso_desc.BlendState = additive_blend_desc;
    billboards_pso_desc.DepthStencilState.DepthEnable = true;
    billboards_pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // Turn off writes to the depth buffer.
    billboards_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    billboards_pso_desc.NumRenderTargets = 1;
    billboards_pso_desc.RTVFormats[0] = hdr_buffer_format;
    billboards_pso_desc.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
    billboards_pso_desc.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
    ID3D12PipelineState *billboards_pso = nullptr;
    check_hr(m_gpu.device->CreateGraphicsPipelineState(&billboards_pso_desc, IID_PPV_ARGS(&billboards_pso)));
    NAME_D3D12_OBJECT(billboards_pso);
    m_PSOs[billboards_PSO] = billboards_pso;

    // Volumetric point lights.
    D3D12_BLEND_DESC additive_transparency_blend_desc = {};
    additive_transparency_blend_desc.AlphaToCoverageEnable = false;
    additive_transparency_blend_desc.IndependentBlendEnable = false;
    additive_transparency_blend_desc.RenderTarget[0] = additive_transparency_rtv_blend_desc;

    m_gpu.compile_shader(L"..\\particles\\shaders\\volume_light.hlsl", L"vs_main", VS, &shader_blob_vs, vs_macros);
    m_gpu.compile_shader(L"..\\particles\\shaders\\volume_light.hlsl", L"ps_main", PS, &shader_blob_ps, ps_macros);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC volume_pl_desc = default_pso_desc;
    volume_pl_desc.VS = {shader_blob_vs->GetBufferPointer(), shader_blob_vs->GetBufferSize()};
    volume_pl_desc.PS = {shader_blob_ps->GetBufferPointer(), shader_blob_ps->GetBufferSize()};
    volume_pl_desc.pRootSignature = m_graphics_rootsig.Get();
    volume_pl_desc.BlendState = additive_transparency_blend_desc;
    volume_pl_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // Turn off writes to the depth buffer.
    volume_pl_desc.DepthStencilState.DepthEnable = false;
    volume_pl_desc.RasterizerState.FrontCounterClockwise = true;
    volume_pl_desc.NumRenderTargets = 1;
    volume_pl_desc.RTVFormats[0] = hdr_buffer_format;
    volume_pl_desc.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
    volume_pl_desc.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
    volume_pl_desc.InputLayout = {standard_inputlayout, _countof(standard_inputlayout)};
    ID3D12PipelineState *volume_pl_pso = nullptr;
    check_hr(m_gpu.device->CreateGraphicsPipelineState(&volume_pl_desc, IID_PPV_ARGS(&volume_pl_pso)));
    NAME_D3D12_OBJECT(volume_pl_pso);
    m_PSOs[volume_light_additive_transparency_PSO] = volume_pl_pso;

    D3D12_BLEND_DESC alpha_transparency_blend_desc = {};
    alpha_transparency_blend_desc.AlphaToCoverageEnable = false;
    alpha_transparency_blend_desc.IndependentBlendEnable = false;
    alpha_transparency_blend_desc.RenderTarget[0] = transparency_rtv_blend_desc;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC volume_pl_desc3 = volume_pl_desc;
    volume_pl_desc3.BlendState = alpha_transparency_blend_desc;
    ID3D12PipelineState *volume_pl_pso3 = nullptr;
    check_hr(m_gpu.device->CreateGraphicsPipelineState(&volume_pl_desc3, IID_PPV_ARGS(&volume_pl_pso3)));
    NAME_D3D12_OBJECT(volume_pl_pso3);
    m_PSOs[volume_light_alpha_transparency_PSO] = volume_pl_pso3;

    // Conversion from equirectangular textures to cube maps.
    m_gpu.compile_shader(L"..\\particles\\shaders\\equirect_to_cube.hlsl", L"cs_main", CS, &shader_blob_cs, cs_macros);
    D3D12_COMPUTE_PIPELINE_STATE_DESC equirect_to_cube_desc = {};
    equirect_to_cube_desc.CS = {shader_blob_cs->GetBufferPointer(), shader_blob_cs->GetBufferSize()};
    equirect_to_cube_desc.pRootSignature = m_compute_rootsig.Get();
    equirect_to_cube_desc.NodeMask = 0;
    equirect_to_cube_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    ID3D12PipelineState *equirect_to_cube_pso = nullptr;
    check_hr(m_gpu.device->CreateComputePipelineState(&equirect_to_cube_desc, IID_PPV_ARGS(&equirect_to_cube_pso)));
    NAME_D3D12_OBJECT(equirect_to_cube_pso);
    m_PSOs[equirect_to_cube_PSO] = equirect_to_cube_pso;

    // Draw sky.
    m_gpu.compile_shader(L"..\\particles\\shaders\\draw_sky.hlsl", L"vs_main", VS, &shader_blob_vs, vs_macros);
    m_gpu.compile_shader(L"..\\particles\\shaders\\draw_sky.hlsl", L"ps_main", PS, &shader_blob_ps, ps_macros);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC draw_sky_desc = default_pso_desc;
    draw_sky_desc.VS = {shader_blob_vs->GetBufferPointer(), shader_blob_vs->GetBufferSize()};
    draw_sky_desc.PS = {shader_blob_ps->GetBufferPointer(), shader_blob_ps->GetBufferSize()};
    draw_sky_desc.pRootSignature = m_graphics_rootsig.Get();
    draw_sky_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // Turn off writes to the depth buffer.
    draw_sky_desc.DepthStencilState.DepthEnable = false;
    draw_sky_desc.RasterizerState.FrontCounterClockwise = true;
    draw_sky_desc.NumRenderTargets = 1;
    draw_sky_desc.RTVFormats[0] = hdr_buffer_format;
    draw_sky_desc.InputLayout = {};
    ID3D12PipelineState *draw_sky_pso = nullptr;
    check_hr(m_gpu.device->CreateGraphicsPipelineState(&draw_sky_desc, IID_PPV_ARGS(&draw_sky_pso)));
    NAME_D3D12_OBJECT(draw_sky_pso);
    m_PSOs[draw_sky_PSO] = draw_sky_pso;

    // Precompute the diffuse irradiance map.
    m_gpu.compile_shader(L"..\\particles\\shaders\\diffuse_irradiance_map.hlsl", L"cs_main", CS, &shader_blob_cs, cs_macros);
    D3D12_COMPUTE_PIPELINE_STATE_DESC diffuse_irrmap_pso_desc = {};
    diffuse_irrmap_pso_desc.CS = {shader_blob_cs->GetBufferPointer(), shader_blob_cs->GetBufferSize()};
    diffuse_irrmap_pso_desc.pRootSignature = m_compute_rootsig.Get();
    diffuse_irrmap_pso_desc.NodeMask = 0;
    diffuse_irrmap_pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    ID3D12PipelineState *diffuse_irrmap_pso = nullptr;
    check_hr(m_gpu.device->CreateComputePipelineState(&diffuse_irrmap_pso_desc, IID_PPV_ARGS(&diffuse_irrmap_pso)));
    NAME_D3D12_OBJECT(diffuse_irrmap_pso);
    m_PSOs[filter_diffuse_irradiance_map_PSO] = diffuse_irrmap_pso;

    // Precompute the specular irradiance map.
    m_gpu.compile_shader(L"..\\particles\\shaders\\specular_irradiance_map.hlsl", L"cs_main", CS, &shader_blob_cs, cs_macros);
    D3D12_COMPUTE_PIPELINE_STATE_DESC specular_irrmap_pso_desc = {};
    specular_irrmap_pso_desc.CS = {shader_blob_cs->GetBufferPointer(), shader_blob_cs->GetBufferSize()};
    specular_irrmap_pso_desc.pRootSignature = m_compute_rootsig.Get();
    specular_irrmap_pso_desc.NodeMask = 0;
    specular_irrmap_pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    ID3D12PipelineState *specular_irrmap_pso = nullptr;
    check_hr(m_gpu.device->CreateComputePipelineState(&specular_irrmap_pso_desc, IID_PPV_ARGS(&specular_irrmap_pso)));
    NAME_D3D12_OBJECT(specular_irrmap_pso);
    m_PSOs[filter_specular_irradiance_map_PSO] = specular_irrmap_pso;

    // Generate mipmap.
    m_gpu.compile_shader(L"..\\particles\\shaders\\generate_cube_mip_linear.hlsl", L"cs_main", CS, &shader_blob_cs, cs_macros);
    D3D12_COMPUTE_PIPELINE_STATE_DESC generate_mipmap_pso_desc = {};
    generate_mipmap_pso_desc.CS = {shader_blob_cs->GetBufferPointer(), shader_blob_cs->GetBufferSize()};
    generate_mipmap_pso_desc.pRootSignature = m_compute_rootsig.Get();
    generate_mipmap_pso_desc.NodeMask = 0;
    generate_mipmap_pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    ID3D12PipelineState *generate_mipmap_pso = nullptr;
    check_hr(m_gpu.device->CreateComputePipelineState(&generate_mipmap_pso_desc, IID_PPV_ARGS(&generate_mipmap_pso)));
    NAME_D3D12_OBJECT(generate_mipmap_pso);
    m_PSOs[generate_mipmap_PSO] = generate_mipmap_pso;

    // Pre-compute the specular BRDF lut.
    m_gpu.compile_shader(L"..\\particles\\shaders\\specular_brdf_lut.hlsl", L"cs_main", CS, &shader_blob_cs, cs_macros);
    D3D12_COMPUTE_PIPELINE_STATE_DESC specular_brdf_lut_pso_desc = {};
    specular_brdf_lut_pso_desc.CS = {shader_blob_cs->GetBufferPointer(), shader_blob_cs->GetBufferSize()};
    specular_brdf_lut_pso_desc.pRootSignature = m_compute_rootsig.Get();
    specular_brdf_lut_pso_desc.NodeMask = 0;
    specular_brdf_lut_pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    ID3D12PipelineState *specular_brdf_lut_pso = nullptr;
    check_hr(m_gpu.device->CreateComputePipelineState(&specular_brdf_lut_pso_desc, IID_PPV_ARGS(&specular_brdf_lut_pso)));
    NAME_D3D12_OBJECT(specular_brdf_lut_pso);
    m_PSOs[pre_integrate_specular_brdf_PSO] = specular_brdf_lut_pso;
}

void particles_graphics::create_samplers()
{
    D3D12_SAMPLER_DESC linear_wrap_desc = {};
    linear_wrap_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linear_wrap_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    linear_wrap_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    linear_wrap_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    linear_wrap_desc.MipLODBias = 0.f;
    linear_wrap_desc.MaxAnisotropy = 16;
    linear_wrap_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    linear_wrap_desc.BorderColor[0] = 1.f;
    linear_wrap_desc.BorderColor[1] = 1.f;
    linear_wrap_desc.BorderColor[2] = 1.f;
    linear_wrap_desc.BorderColor[3] = 1.f;
    linear_wrap_desc.MinLOD = 0.f;
    linear_wrap_desc.MaxLOD = D3D12_FLOAT32_MAX;
    m_samplers[linear_wrap].ptr = m_gpu.sampler_allocator.allocate();
    m_gpu.device->CreateSampler(&linear_wrap_desc, m_samplers[linear_wrap]);

    D3D12_SAMPLER_DESC shadow_sampler_desc = {};
    shadow_sampler_desc.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadow_sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    shadow_sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    shadow_sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    shadow_sampler_desc.MipLODBias = 0.f;
    shadow_sampler_desc.MaxAnisotropy = 0;
    shadow_sampler_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadow_sampler_desc.BorderColor[0] = 0.f;
    shadow_sampler_desc.BorderColor[1] = 0.f;
    shadow_sampler_desc.BorderColor[2] = 0.f;
    shadow_sampler_desc.BorderColor[3] = 0.f;
    shadow_sampler_desc.MinLOD = 0.f;
    shadow_sampler_desc.MaxLOD = D3D12_FLOAT32_MAX;
    m_samplers[shadow_sampler].ptr = m_gpu.sampler_allocator.allocate();
    m_gpu.device->CreateSampler(&shadow_sampler_desc, m_samplers[shadow_sampler]);

    D3D12_SAMPLER_DESC point_sampler_desc = {};
    point_sampler_desc.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    point_sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    point_sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    point_sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    point_sampler_desc.MipLODBias = 0.f;
    point_sampler_desc.MaxAnisotropy = 16;
    point_sampler_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    point_sampler_desc.BorderColor[0] = 1.f;
    point_sampler_desc.BorderColor[1] = 1.f;
    point_sampler_desc.BorderColor[2] = 1.f;
    point_sampler_desc.BorderColor[3] = 1.f;
    point_sampler_desc.MinLOD = 0.f;
    point_sampler_desc.MaxLOD = D3D12_FLOAT32_MAX;
    m_samplers[point_clamp].ptr = m_gpu.sampler_allocator.allocate();
    m_gpu.device->CreateSampler(&point_sampler_desc, m_samplers[point_clamp]);

    D3D12_SAMPLER_DESC linear_clamp_sampler_desc = {};
    linear_clamp_sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linear_clamp_sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linear_clamp_sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linear_clamp_sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linear_clamp_sampler_desc.MipLODBias = 0.f;
    linear_clamp_sampler_desc.MaxAnisotropy = 16;
    linear_clamp_sampler_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    linear_clamp_sampler_desc.BorderColor[0] = 1.f;
    linear_clamp_sampler_desc.BorderColor[1] = 1.f;
    linear_clamp_sampler_desc.BorderColor[2] = 1.f;
    linear_clamp_sampler_desc.BorderColor[3] = 1.f;
    linear_clamp_sampler_desc.MinLOD = 0.f;
    linear_clamp_sampler_desc.MaxLOD = D3D12_FLOAT32_MAX;
    m_samplers[linear_clamp].ptr = m_gpu.sampler_allocator.allocate();
    m_gpu.device->CreateSampler(&linear_clamp_sampler_desc, m_samplers[linear_clamp]);
}

void particles_graphics::create_render_objects(ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    // Create the cube objects.
    float cube_size = 5.f;
    render_object &cube = m_render_objects["cube"];
    cube.m_mesh.from_generator(&m_gpu, cmd_list, &GeometryGenerator::CreateBox(1.f, 1.f, 1.f, 0));
    NAME_D3D12_OBJECT(cube.m_mesh.m_texture_heap);
    NAME_D3D12_OBJECT(cube.m_mesh.m_vertices_gpu);
    NAME_D3D12_OBJECT(cube.m_mesh.m_indices_gpu);
    cube.m_transform.set_translation(0.8f, 0.5f, 0.f);
    cube.m_transform.set_rotation(0.f, 1.1f, 0.f);
    cube.m_color = {1.f, 1.f, 1.f};
    cube.m_roughness_metalness = {0.5f, 0.5f};

    // Create sphere.
    render_object &sphere = m_render_objects["sphere"];
    sphere.m_mesh.from_generator(&m_gpu, cmd_list, &GeometryGenerator::CreateSphere(1.f, 20, 20));
    NAME_D3D12_OBJECT(sphere.m_mesh.m_texture_heap);
    NAME_D3D12_OBJECT(sphere.m_mesh.m_vertices_gpu);
    NAME_D3D12_OBJECT(sphere.m_mesh.m_indices_gpu);
    sphere.m_transform.set_translation(6.4f, 0.45f, 0.f);
    sphere.m_transform.set_scale(0.5f, 0.5f, 0.5f);
    sphere.m_color = {1.f, 1.f, 1.f};
    sphere.m_roughness_metalness = {0.1f, 0.f};

    // Create the Sponza scene data.
    render_object &sponza = m_render_objects["sponza"];
    sponza.m_mesh.from_asset(&m_gpu, "..\\particles\\models\\sponza_pbr\\sponza2.gltf", aiProcess_OptimizeGraph);
    std::reverse(sponza.m_mesh.m_submeshes.begin(), sponza.m_mesh.m_submeshes.end());
    sponza.m_color = {0.f, 0.f, 0.f};
    NAME_D3D12_OBJECT(sponza.m_mesh.m_texture_heap);
    NAME_D3D12_OBJECT(sponza.m_mesh.m_vertices_gpu);
    NAME_D3D12_OBJECT(sponza.m_mesh.m_indices_gpu);
    for (UINT i = 0; i < sponza.m_mesh.m_submeshes.size(); i++)
    {
        NAME_D3D12_OBJECT_INDEXED(sponza.m_mesh.m_submeshes[i].m_textures_gpu[diffuse], i);
        NAME_D3D12_OBJECT_INDEXED(sponza.m_mesh.m_submeshes[i].m_textures_gpu[normal], i);
        NAME_D3D12_OBJECT_INDEXED(sponza.m_mesh.m_submeshes[i].m_textures_gpu[metallic_roughness], i);
    }

    // Create the lucy statue data.
    render_object &lucy = m_render_objects["lucy"];
    lucy.m_mesh.from_asset(&m_gpu, "..\\particles\\models\\lucy.fbx", aiProcess_OptimizeGraph);
    lucy.m_color = {0.75f, 0.75f, 0.75f};
    lucy.m_roughness_metalness = {0.2f, 0.f};
    lucy.m_transform.set_scale(0.001f, 0.001f, 0.001f);
    lucy.m_transform.set_rotation(-XM_PIDIV2, 0.f, 0.f);
    lucy.m_transform.set_translation(2.4f, 0.58f, 1.3f);
    NAME_D3D12_OBJECT(lucy.m_mesh.m_texture_heap);
    NAME_D3D12_OBJECT(lucy.m_mesh.m_vertices_gpu);
    NAME_D3D12_OBJECT(lucy.m_mesh.m_indices_gpu);
    for (UINT i = 0; i < lucy.m_mesh.m_submeshes.size(); i++)
    {
        NAME_D3D12_OBJECT_INDEXED(lucy.m_mesh.m_submeshes[i].m_textures_gpu[diffuse], i);
        NAME_D3D12_OBJECT_INDEXED(lucy.m_mesh.m_submeshes[i].m_textures_gpu[normal], i);
        NAME_D3D12_OBJECT_INDEXED(lucy.m_mesh.m_submeshes[i].m_textures_gpu[metallic_roughness], i);
    }

    // Calculate total number of submeshes.
    for (auto &ro_pair : m_render_objects)
    {
        const render_object &ro = ro_pair.second;
        num_total_submeshes += ro.m_mesh.m_submeshes.size();
    }
}

void particles_graphics::create_buffers(ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    // Pass data.
    pass_data pass = {};
    m_pass_cb = m_gpu.create_constant_buffer<pass_data>(&pass);
    NAME_D3D12_OBJECT(m_pass_cb.default_resource);

    // Per object vertex shader data.
    object_data_vs obj_data_vs = {};
    m_object_cb_vs = m_gpu.create_constant_buffer<object_data_vs>(&obj_data_vs);
    NAME_D3D12_OBJECT(m_object_cb_vs.default_resource);

    m_shadowmap_cb_vs = m_gpu.create_constant_buffer<object_data_vs>(&obj_data_vs);
    NAME_D3D12_OBJECT(m_shadowmap_cb_vs.default_resource);

    // Per render object pixel shader data.
    render_object_data_ps ro_data_ps = {};
    m_render_object_cb_ps = m_gpu.create_constant_buffer<render_object_data_ps>(&ro_data_ps);
    NAME_D3D12_OBJECT(m_render_object_cb_ps.default_resource);

    // Per volume light pixel shader data.
    volume_light_data_ps vl_data_ps = {};
    m_volume_light_cb_ps = m_gpu.create_constant_buffer<volume_light_data_ps>(&vl_data_ps);
    NAME_D3D12_OBJECT(m_volume_light_cb_ps.default_resource);

    UINT particle_lights_count = 0;
    m_particle_lights_counter = m_gpu.create_constant_buffer<UINT>(&particle_lights_count, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    NAME_D3D12_OBJECT(m_particle_lights_counter.default_resource);

    m_particle_lights_sb = m_gpu.create_structured_buffer(total_particle_lights, num_particle_lights,
                                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    NAME_D3D12_OBJECT(m_particle_lights_sb.default_resource);

    // Create the UAVs and SRVs for point light data.
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    uav_desc.Buffer.NumElements = num_particles_total;
    uav_desc.Buffer.StructureByteStride = sizeof(point_light);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = DXGI_FORMAT_UNKNOWN;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    srv_desc.Buffer.NumElements = num_particles_total;
    srv_desc.Buffer.StructureByteStride = sizeof(point_light);

    D3D12_CPU_DESCRIPTOR_HANDLE pointlights_handle;
    for (UINT i = 0; i < m_gpu.NUM_BACK_BUFFERS; i++)
    {
        auto csu_table_alloc = m_gpu.frames[i].csu_table_allocator;
        ComPtr<ID3D12DescriptorHeap> csu_heap_gpu = csu_table_alloc.m_heap_gpu;

        // Create particle lights UAVs.
        uav_desc.Buffer.FirstElement = 0;
        uav_desc.Buffer.CounterOffsetInBytes = 0;
        size_t offset_in_bytes = uav_particle_lights * m_gpu.csu_descriptor_size;
        pointlights_handle.ptr = csu_heap_gpu->GetCPUDescriptorHandleForHeapStart().ptr + offset_in_bytes;
        m_gpu.device->CreateUnorderedAccessView(m_particle_lights_sb.default_resource.Get(),
                                                m_particle_lights_counter.default_resource.Get(),
                                                &uav_desc, pointlights_handle);

        // Create particle lights SRVs.
        srv_desc.Buffer.FirstElement = 0;
        offset_in_bytes = srv_particle_lights * m_gpu.csu_descriptor_size;
        pointlights_handle.ptr = csu_heap_gpu->GetCPUDescriptorHandleForHeapStart().ptr + offset_in_bytes;
        m_gpu.device->CreateShaderResourceView(m_particle_lights_sb.default_resource.Get(), &srv_desc, pointlights_handle);
    }

    m_attractors_sb = m_gpu.create_structured_buffer(attractors, _countof(attractors), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    NAME_D3D12_OBJECT(m_attractors_sb.default_resource);

    m_spotlights_sb = m_gpu.create_structured_buffer(spotlights, num_spotlights);
    NAME_D3D12_OBJECT(m_spotlights_sb.default_resource);

    // Initialize light data.
    for (UINT i = 0; i < _countof(total_particle_lights); i++)
    {
        point_light *particlelight = total_particle_lights;
        particlelight[i].id = i;
        particlelight[i].falloff_start = 0.01f;
        particlelight[i].falloff_end = 1.1f;
        particlelight[i].color = {1.f, 1.f, 1.f};
        particlelight[i].strenght = {0.6f, 0.6f, 0.6f};
    }

    for (UINT i = 0; i < _countof(spotlights); i++)
    {
        spot_light *spotlight = spotlights;
        spotlight[i].id = i;
        spotlight[i].falloff_start = 1.f;
        spotlight[i].falloff_end = 10.f;
        spotlight[i].direction = {0.f, -1.f, 0.f};
        spotlight[i].color = {0.275f, 0.45f, 1.f};
        spotlight[i].strenght = {1.f, 1.f, 1.f};
        spotlight[i].spot_power = 64.f;
    }
    spotlights[0].position_ws = {-12.f, 5.f, 4.f};
    spotlights[1].position_ws = {11.f, 5.f, 4.f};
    spotlights[2].position_ws = {11.f, 5.f, -4.f};
    spotlights[3].position_ws = {-12.f, 5.f, -4.f};

    // Create point and volume lights.
    float volume_size = 5.f;
    float radius = 1.3f;
    auto volume_data = GeometryGenerator::CreateBox(volume_size, volume_size, volume_size, 0);
    for (UINT i = 0; i < num_particle_systems; i++)
    {
        // Point lights.
        attractor_point_light *attractor = attractors;
        attractor[i].light.id = i;
        attractor[i].light.falloff_start = 0.01f;
        attractor[i].light.falloff_end = 5.f;
        attractor[i].light.color = {1.f, 0.7f, 0.2f};
        attractor[i].light.strenght = {1.f, 1.f, 1.f};

        // Volume lights.
        volume_light *volume_light = &volume_lights[i];
        volume_light->m_color = {1.f, 0.8f, 0.4f, 0.9f};
        volume_light->m_radius = radius;
        volume_light->m_mesh.from_generator(&m_gpu, cmd_list, &volume_data);
        volume_light->m_blend_mode = blend_modes::additive_transparency;
    }
    transform t;
    t.set_translation(2.9f, 1.5f, 1.3f);
    attractors[0].world = t.m_transposed_world;
    attractors[0].light.position_ws = t.m_translation;
    volume_lights[0].m_transform = t;

    t.set_translation(4.8f, 1.5f, -2.2f);
    attractors[1].world = t.m_transposed_world;
    attractors[1].light.position_ws = t.m_translation;
    volume_lights[1].m_transform = t;

    // Create a zero'd counter used to reset other counters.
    check_hr(m_gpu.device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT)),
                                                   D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                   nullptr,
                                                   IID_PPV_ARGS(&reset_counter_default)));
    NAME_D3D12_OBJECT(reset_counter_default);

    // Create the resource that holds the shadow casters transforms data.
    int ro_id = 0;
    for (auto &ro_pair : m_render_objects)
    {
        const render_object &ro = ro_pair.second;

        shadow_casters_transforms[ro_id].world = ro.m_transform.m_world;
        XMMATRIX world;
        world = XMLoadFloat4x4(&ro.m_transform.m_world);
        XMStoreFloat4x4(&shadow_casters_transforms[ro_id].world, XMMatrixTranspose(world));
        XMMATRIX view_proj = XMLoadFloat4x4(&m_cameras[main_camera].m_view_proj);
        XMStoreFloat4x4(&shadow_casters_transforms[ro_id].world_view_proj, XMMatrixTranspose(world * view_proj));
        ro_id++;
    }

    shadow_transforms_cbv_size = num_shadow_casters * align_up(sizeof(object_data_vs), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    m_gpu.default_resource_from_uploader(cmd_list, m_shadowcasters_transforms.GetAddressOf(),
                                         shadow_casters_transforms, shadow_transforms_cbv_size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    NAME_D3D12_OBJECT(m_shadowcasters_transforms);

    // Create the CBVs of the shadow casters transforms.
    for (size_t i = 0; i < m_gpu.NUM_BACK_BUFFERS; i++)
    {
        auto csu_table_alloc = m_gpu.frames[i].csu_table_allocator;
        ComPtr<ID3D12DescriptorHeap> csu_heap_gpu = csu_table_alloc.m_heap_gpu;
        cbv_gpu_shadowcasters_transforms_base[i].ptr = csu_heap_gpu->GetGPUDescriptorHandleForHeapStart().ptr +
                                                       (cbv_shadow_casters_transforms * csu_table_alloc.m_descriptor_size);

        for (size_t j = 0; j < num_shadow_casters; j++)
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_shadowcasters_transforms_desc = {};
            cbv_shadowcasters_transforms_desc.BufferLocation = m_shadowcasters_transforms->GetGPUVirtualAddress() +
                                                               (j * D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            cbv_shadowcasters_transforms_desc.SizeInBytes = (UINT)sizeof(object_data_vs);

            size_t offset_to_transforms = (cbv_shadow_casters_transforms + j) * csu_table_alloc.m_descriptor_size;
            cbv_cpu_shadowcasters_transforms[i].ptr = csu_heap_gpu->GetCPUDescriptorHandleForHeapStart().ptr + offset_to_transforms;
            m_gpu.device->CreateConstantBufferView(&cbv_shadowcasters_transforms_desc, cbv_cpu_shadowcasters_transforms[i]);
        }
    }
}

void particles_graphics::create_camera_debug_vertices(ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    std::vector<camera::vertex_debug> cam_debug_vertices = m_cameras[debug_camera].get_debug_frustum_vertices();

    m_debugcam_frustum_vertices = m_gpu.create_scratch_buffer<camera::vertex_debug>(cam_debug_vertices.data(),
                                                                                    cam_debug_vertices.size(),
                                                                                    "debugcam_frustum_vertices");
}

void particles_graphics::create_camera_debug_indices(ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    // Frustum indices.
    std::vector<UINT16> frustum_indices = m_cameras[debug_camera].get_frustum_indices();
    size_t index_stride = sizeof(UINT16);
    size_t num_indices = frustum_indices.size();
    size_t indices_size = index_stride * num_indices;
    m_gpu.default_resource_from_uploader(cmd_list, m_debug_cam_frustum.m_mesh.m_indices_gpu.GetAddressOf(),
                                         frustum_indices.data(), indices_size, index_stride);
    NAME_D3D12_OBJECT(m_debug_cam_frustum.m_mesh.m_indices_gpu);

    D3D12_INDEX_BUFFER_VIEW frustum_indices_ibv;
    frustum_indices_ibv.BufferLocation = m_debug_cam_frustum.m_mesh.m_indices_gpu->GetGPUVirtualAddress();
    frustum_indices_ibv.SizeInBytes = (UINT)indices_size;
    frustum_indices_ibv.Format = DXGI_FORMAT_R16_UINT;

    m_debug_cam_frustum.m_mesh.m_ibv = frustum_indices_ibv;

    mesh::submesh indices_submesh;
    indices_submesh.base_vertex_location = 0;
    indices_submesh.start_index_location = 0;
    indices_submesh.index_count = (UINT)num_indices;
    indices_submesh.bounds = {};

    m_debug_cam_frustum.m_mesh.m_submeshes.push_back(indices_submesh);

    // Frustum plane indices.
    std::vector<UINT16> frustum_plane_indices = m_cameras[debug_camera].get_frustum_planes_indices();
    num_indices = frustum_plane_indices.size();
    indices_size = index_stride * num_indices;
    m_gpu.default_resource_from_uploader(cmd_list, m_debug_cam_frustum_planes.m_mesh.m_indices_gpu.GetAddressOf(),
                                         frustum_plane_indices.data(), indices_size, index_stride);
    NAME_D3D12_OBJECT(m_debug_cam_frustum_planes.m_mesh.m_indices_gpu);

    frustum_indices_ibv.BufferLocation = m_debug_cam_frustum_planes.m_mesh.m_indices_gpu->GetGPUVirtualAddress();
    frustum_indices_ibv.SizeInBytes = (UINT)indices_size;
    frustum_indices_ibv.Format = DXGI_FORMAT_R16_UINT;
    m_debug_cam_frustum_planes.m_mesh.m_ibv = frustum_indices_ibv;

    indices_submesh;
    indices_submesh.base_vertex_location = 0;
    indices_submesh.start_index_location = 0;
    indices_submesh.index_count = (UINT)num_indices;
    indices_submesh.bounds = {};

    m_debug_cam_frustum_planes.m_mesh.m_submeshes.push_back(indices_submesh);
}

void particles_graphics::update_current_camera()
{
    m_cameras[selected_cam].update_position();
    m_cameras[selected_cam].update_view_proj();
}

void particles_graphics::update_camera_yaw_pitch(ImVec2 last_mouse_pos)
{
    m_cameras[selected_cam].update_yaw_pitch(XMFLOAT2(ImGui::GetMousePos().x, ImGui::GetMousePos().y),
                                             XMFLOAT2(last_mouse_pos.x, last_mouse_pos.y));
}

ComPtr<ID3D12RootSignature> particles_graphics::create_graphics_rootsig()
{
    // Note1: the descriptor staging system needs to support multithreading before we can use it everywhere.
    // In the meantime worker threads need to bind to the root signature directly.

    // Note2: the descriptor staging system needs to support descriptor arrays before we can use it everywhere.
    // In the meantime we bind descriptor arrays to the root signature directly.
    // Some indirect execution commands make use of descriptor arrays.

    // Spot light shadow caster transform.
    std::vector<D3D12_ROOT_PARAMETER1> params;
    D3D12_ROOT_PARAMETER1 sparam;
    sparam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    sparam.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    sparam.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    sparam.Descriptor.RegisterSpace = 1;
    sparam.Descriptor.ShaderRegister = 1;
    params.push_back(sparam);

    // Point light shadow caster info.
    sparam = {};
    sparam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    sparam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    sparam.Constants.Num32BitValues = 2;
    sparam.Constants.RegisterSpace = 2;
    sparam.Constants.ShaderRegister = 0;
    params.push_back(sparam);

    // Point light shadow caster transforms.
    D3D12_DESCRIPTOR_RANGE1 shadow_transforms_ranges[1];
    shadow_transforms_ranges[0].BaseShaderRegister = 1;
    shadow_transforms_ranges[0].RegisterSpace = 2;
    shadow_transforms_ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    shadow_transforms_ranges[0].NumDescriptors = num_shadow_casters;
    shadow_transforms_ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    shadow_transforms_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;

    sparam = {};
    sparam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    sparam.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    sparam.DescriptorTable.NumDescriptorRanges = _countof(shadow_transforms_ranges);
    sparam.DescriptorTable.pDescriptorRanges = shadow_transforms_ranges;
    params.push_back(sparam);

    // Particle system info.
    sparam = {};
    sparam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    sparam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    sparam.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    sparam.Descriptor.RegisterSpace = 1;
    sparam.Descriptor.ShaderRegister = 0;
    params.push_back(sparam);

    // Attractor point lights.
    sparam = {};
    sparam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    sparam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    sparam.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    sparam.Descriptor.RegisterSpace = 1;
    sparam.Descriptor.ShaderRegister = 0;
    params.push_back(sparam);

    return m_gpu.create_graphics_staging_rootsig(params);
}

ComPtr<ID3D12RootSignature> particles_graphics::create_compute_rootsig()
{
    std::vector<D3D12_ROOT_PARAMETER1> params;

    // Particles.
    D3D12_DESCRIPTOR_RANGE1 compute_ranges[10];
    compute_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    compute_ranges[0].BaseShaderRegister = 0;
    compute_ranges[0].RegisterSpace = 5;
    compute_ranges[0].NumDescriptors = num_particle_systems;
    compute_ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    compute_ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    compute_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    compute_ranges[1].BaseShaderRegister = 0;
    compute_ranges[1].RegisterSpace = 0;
    compute_ranges[1].NumDescriptors = num_particle_systems;
    compute_ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    compute_ranges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    compute_ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    compute_ranges[2].BaseShaderRegister = 0;
    compute_ranges[2].RegisterSpace = 0;
    compute_ranges[2].NumDescriptors = num_particle_systems * 2;
    compute_ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    compute_ranges[2].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    // Particle lights.
    compute_ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    compute_ranges[3].BaseShaderRegister = 0;
    compute_ranges[3].RegisterSpace = 1;
    compute_ranges[3].NumDescriptors = 1;
    compute_ranges[3].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    compute_ranges[3].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    compute_ranges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    compute_ranges[4].BaseShaderRegister = 0;
    compute_ranges[4].RegisterSpace = 1;
    compute_ranges[4].NumDescriptors = 1;
    compute_ranges[4].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    compute_ranges[4].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    // Bounds.
    compute_ranges[5].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    compute_ranges[5].BaseShaderRegister = 0;
    compute_ranges[5].RegisterSpace = 2;
    compute_ranges[5].NumDescriptors = 1;
    compute_ranges[5].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    compute_ranges[5].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    compute_ranges[6].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    compute_ranges[6].BaseShaderRegister = 0;
    compute_ranges[6].RegisterSpace = 2;
    compute_ranges[6].NumDescriptors = 1;
    compute_ranges[6].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    compute_ranges[6].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    compute_ranges[7].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    compute_ranges[7].BaseShaderRegister = 0;
    compute_ranges[7].RegisterSpace = 3;
    compute_ranges[7].NumDescriptors = 1;
    compute_ranges[7].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    compute_ranges[7].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    // Draw commands.
    compute_ranges[8].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    compute_ranges[8].BaseShaderRegister = 0;
    compute_ranges[8].RegisterSpace = 4;
    compute_ranges[8].NumDescriptors = 1;
    compute_ranges[8].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    compute_ranges[8].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    compute_ranges[9].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    compute_ranges[9].BaseShaderRegister = 0;
    compute_ranges[9].RegisterSpace = 4;
    compute_ranges[9].NumDescriptors = 1;
    compute_ranges[9].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    compute_ranges[9].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    D3D12_ROOT_PARAMETER1 bindless_param;
    bindless_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    bindless_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    bindless_param.DescriptorTable.NumDescriptorRanges = _countof(compute_ranges);
    bindless_param.DescriptorTable.pDescriptorRanges = compute_ranges;
    params.push_back(bindless_param);

    // Pass data root CBV.
    D3D12_ROOT_PARAMETER1 pass_data_param;
    pass_data_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    pass_data_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    pass_data_param.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    pass_data_param.Descriptor.RegisterSpace = 0;
    pass_data_param.Descriptor.ShaderRegister = 0;
    params.push_back(pass_data_param);

    // Particle info root CBV.
    D3D12_ROOT_PARAMETER1 particle_info_param;
    particle_info_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    particle_info_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    particle_info_param.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
    particle_info_param.Descriptor.RegisterSpace = 0;
    particle_info_param.Descriptor.ShaderRegister = 1;
    params.push_back(particle_info_param);

    // Particle simulation command root srv.
    // It's bound as root so that we can advance the simulation by swapping pointers.
    D3D12_ROOT_PARAMETER1 simulation_commands;
    simulation_commands.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    simulation_commands.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    simulation_commands.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    simulation_commands.Descriptor.RegisterSpace = 3;
    simulation_commands.Descriptor.ShaderRegister = 0;
    params.push_back(simulation_commands);

    // Particle buffer info.
    D3D12_ROOT_PARAMETER1 buffer_indices_param;
    buffer_indices_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    buffer_indices_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    buffer_indices_param.Constants.Num32BitValues = 2;
    buffer_indices_param.Constants.ShaderRegister = 2;
    buffer_indices_param.Constants.RegisterSpace = 0;
    params.push_back(buffer_indices_param);

    // Texture preprocessing.
    D3D12_DESCRIPTOR_RANGE1 tex_preprocess_range[2];
    tex_preprocess_range[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    tex_preprocess_range[0].BaseShaderRegister = 0;
    tex_preprocess_range[0].RegisterSpace = 6;
    tex_preprocess_range[0].NumDescriptors = 1;
    tex_preprocess_range[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    tex_preprocess_range[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;

    tex_preprocess_range[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    tex_preprocess_range[1].BaseShaderRegister = 0;
    tex_preprocess_range[1].RegisterSpace = 6;
    tex_preprocess_range[1].NumDescriptors = 1;
    tex_preprocess_range[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    tex_preprocess_range[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    D3D12_ROOT_PARAMETER1 tex_preprocess_param;
    tex_preprocess_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    tex_preprocess_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    tex_preprocess_param.DescriptorTable.NumDescriptorRanges = _countof(tex_preprocess_range);
    tex_preprocess_param.DescriptorTable.pDescriptorRanges = tex_preprocess_range;
    params.push_back(tex_preprocess_param);

    // Delta roughness used during the specular irradiance map filtering.
    D3D12_ROOT_PARAMETER1 delta_roughness_param;
    delta_roughness_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    delta_roughness_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    delta_roughness_param.Constants.Num32BitValues = 1;
    delta_roughness_param.Constants.RegisterSpace = 0;
    delta_roughness_param.Constants.ShaderRegister = 4;
    params.push_back(delta_roughness_param);

    return m_gpu.create_compute_staging_rootsig(params, 20);
}
