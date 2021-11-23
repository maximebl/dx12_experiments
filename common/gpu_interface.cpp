#include "gpu_interface.h"
#include <d3dcompiler.h>
#include "PathCch.h"
#include "common.h"
#include <pix3.h>
#include <DirectXTex.h>
#include "stb_image.h"

#include "../particles/shader_data.h"

using namespace DirectX;

void gpu_interface::init_core(DXGI_FORMAT back_buffer_format)
{
    UINT32 dxgi_factory_flags = 0;

    // Debug layer.
#ifdef _DEBUG

    ComPtr<ID3D12Debug3> debug;
    check_hr(D3D12GetDebugInterface(IID_PPV_ARGS(debug.GetAddressOf())));
    debug->EnableDebugLayer();

    // Turn on auto-breadcrumbs and page fault reporting.
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> pDredSettings;
    check_hr(D3D12GetDebugInterface(IID_PPV_ARGS(&pDredSettings)));
    pDredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    pDredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);

    debug->SetEnableGPUBasedValidation(false);
    debug->SetGPUBasedValidationFlags(D3D12_GPU_BASED_VALIDATION_FLAGS_NONE);
    debug->SetEnableSynchronizedCommandQueueValidation(true); // Turn off to improve performance when more than one queue is used.

    ComPtr<IDXGIInfoQueue> info_queue;
    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(info_queue.GetAddressOf()))))
    {
        UINT32 dxgi_factory_flags = DXGI_CREATE_FACTORY_DEBUG;
        info_queue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, true);
        info_queue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, true);
        info_queue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_WARNING, true);
    }

#endif

    // Adapter.
    check_hr(CreateDXGIFactory2(dxgi_factory_flags, IID_PPV_ARGS(m_dxgi_factory.GetAddressOf())));

    check_hr(m_dxgi_factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                        IID_PPV_ARGS(m_adapter.GetAddressOf())));

    // Device.
    check_hr(D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device)));
    NAME_D3D12_OBJECT(device);

    // D3D12 info queue.
    ComPtr<ID3D12InfoQueue> d3d12_info_queue;
    if (SUCCEEDED(device.As<ID3D12InfoQueue>(&d3d12_info_queue)))
    {
        d3d12_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
        d3d12_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
        d3d12_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);

        // Suppress messages based on their severity level
        D3D12_MESSAGE_SEVERITY severities[] = {D3D12_MESSAGE_SEVERITY_INFO};

        // Suppress individual messages by their ID
        D3D12_MESSAGE_ID deny_ids[] = {
            // This warning occurs when using capture frame while graphics debugging.
            D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,
            // This warning occurs when using capture frame while graphics debugging.
            D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE,
            // This warning occurs when a ExecuteIndirect command changes a root binding.
            D3D12_MESSAGE_ID_GPU_BASED_VALIDATION_RESOURCE_STATE_IMPRECISE,
        };

        D3D12_INFO_QUEUE_FILTER filter = {};
        filter.DenyList.NumSeverities = _countof(severities);
        filter.DenyList.pSeverityList = severities;
        filter.DenyList.NumIDs = _countof(deny_ids);
        filter.DenyList.pIDList = deny_ids;

        check_hr(d3d12_info_queue->PushStorageFilter(&filter));
    }

    // Command queues
    D3D12_COMMAND_QUEUE_DESC cmd_queue_desc;
    cmd_queue_desc.NodeMask = 0;
    cmd_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    cmd_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    cmd_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    check_hr(device->CreateCommandQueue(&cmd_queue_desc, IID_PPV_ARGS(graphics_cmd_queue.GetAddressOf())));
    NAME_D3D12_OBJECT(graphics_cmd_queue);

    cmd_queue_desc.NodeMask = 0;
    cmd_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    cmd_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    cmd_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    check_hr(device->CreateCommandQueue(&cmd_queue_desc, IID_PPV_ARGS(compute_cmd_queue.GetAddressOf())));
    NAME_D3D12_OBJECT(compute_cmd_queue);

    // Swap chain.
    DXGI_SWAP_CHAIN_DESC1 sd;
    {
        ZeroMemory(&sd, sizeof(sd));
        sd.BufferCount = NUM_BACK_BUFFERS;
        sd.Width = 0;
        sd.Height = 0;
        sd.Format = back_buffer_format;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.Stereo = FALSE;
    }

    IDXGISwapChain1 *swap_chain = NULL;
    check_hr(m_dxgi_factory->CreateSwapChainForHwnd(
        (IUnknown *)graphics_cmd_queue.Get(),
        g_hwnd,
        &sd,
        NULL,
        NULL,
        &swap_chain));

    check_hr(swap_chain->QueryInterface(IID_PPV_ARGS(swapchain.GetAddressOf())));
    safe_release(swap_chain);

    // RTV and DSV heap.
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
    rtv_desc.NumDescriptors = NUM_BACK_BUFFERS;
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    check_hr(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(rtv_heap.GetAddressOf())));
    NAME_D3D12_OBJECT(rtv_heap);

    D3D12_DESCRIPTOR_HEAP_DESC dsv_desc = {};
    dsv_desc.NumDescriptors = NUM_BACK_BUFFERS;
    dsv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    check_hr(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(dsv_heap.GetAddressOf())));
    NAME_D3D12_OBJECT(dsv_heap);

    // Descriptor sizes.
    rtv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dsv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    csu_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Depth stencil resource and view.
    create_dsv(g_hwnd_width, g_hwnd_height);

    // Viewport and scissor rect.
    D3D12_RECT rect;
    rect.left = 0;
    rect.top = 0;
    rect.right = (LONG)g_hwnd_width;
    rect.bottom = (LONG)g_hwnd_height;
    scissor_rect = rect;

    D3D12_VIEWPORT vp;
    vp.TopLeftY = 0;
    vp.TopLeftX = 0;
    vp.Width = (float)g_hwnd_width;
    vp.Height = (float)g_hwnd_height;
    vp.MaxDepth = 1.0f;
    vp.MinDepth = 0.0f;
    viewport = vp;

    // Fence.
    check_hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    NAME_D3D12_OBJECT(fence);
    fence_event = ::CreateEvent(NULL, FALSE, FALSE, NULL);

    // Buffer uploader.
    size_t buffer_size = 512 * 1024 * 1024;
    check_hr(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(buffer_size),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_buffer_uploader.m_upload_resource)));

    void *pdata = nullptr;
    CD3DX12_RANGE read_range(0, 0);
    m_buffer_uploader.m_upload_resource->Map(0, &read_range, &pdata);
    m_buffer_uploader.m_current = (UINT8 *)pdata;
    m_buffer_uploader.m_begin = (UINT8 *)pdata;
    m_buffer_uploader.m_end = m_buffer_uploader.m_begin + buffer_size;

    // Texture uploader.
    buffer_size = 2048 * 2048 * 256;
    check_hr(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(buffer_size),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_texture_uploader)));

    m_tex_uploader_data = nullptr;
    m_texture_uploader->Map(0, &read_range, &m_tex_uploader_data);
    NAME_D3D12_OBJECT(m_texture_uploader);

    frame_index = swapchain->GetCurrentBackBufferIndex();

    // GPU timer.
    m_gpu_timer = gpu_timer(device, graphics_cmd_queue,
                            &frame_index, NUM_BACK_BUFFERS);
}

void gpu_interface::init_frame_resources(UINT additional_descriptors_count)
{
    for (UINT32 i = 0; i < NUM_BACK_BUFFERS; ++i)
    {
        // Create the command list and command allocator for the current frame
        ComPtr<ID3D12CommandAllocator> cmd_alloc;
        check_hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(cmd_alloc.GetAddressOf())));
        NAME_D3D12_OBJECT_INDEXED(cmd_alloc, i);

        ComPtr<ID3D12GraphicsCommandList> cmd_list;
        check_hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_alloc.Get(), nullptr,
                                           IID_PPV_ARGS(cmd_list.GetAddressOf())));
        cmd_list->Close();
        NAME_D3D12_OBJECT_INDEXED(cmd_list, i);

        gpu_interface::frame_resource *frame = &frames[i];
        frame->cmd_alloc = cmd_alloc;
        frame->cmd_list = cmd_list;
        frame->fence_value = i;

        // Create resource allocator
        frame->m_resources_buffer = frame_resource::frame_resources_allocator::frame_resources_allocator(device, 1024 * 1024 * 128);
        NAME_D3D12_OBJECT_INDEXED(frame->m_resources_buffer.m_upload_resource, i);

        // Create descriptor table allocators
        frame->csu_table_allocator = frame_resource::descriptor_table_frame_allocator::descriptor_table_frame_allocator(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                                                                                        staging_descriptors_per_stage,
                                                                                                                        max_rename_count, additional_descriptors_count);
        NAME_D3D12_OBJECT_INDEXED(frame->csu_table_allocator.m_heap_cpu, i);
        NAME_D3D12_OBJECT_INDEXED(frame->csu_table_allocator.m_heap_gpu, i);

        frame->sampler_table_allocator = frame_resource::descriptor_table_frame_allocator::descriptor_table_frame_allocator(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                                                                                                            GPU_SAMPLER_HEAP_COUNT,
                                                                                                                            16);
        NAME_D3D12_OBJECT_INDEXED(frame->sampler_table_allocator.m_heap_cpu, i);
        NAME_D3D12_OBJECT_INDEXED(frame->sampler_table_allocator.m_heap_gpu, i);
    }

    // Create descriptor staging resources.
    create_descriptor_allocators();
    create_null_descriptors();
}

void gpu_interface::resize(int width, int height,
                           render_target *render_targets, size_t num_render_targets)
{
    // Clear back buffers
    for (UINT i = 0; i < num_render_targets; i++)
    {
        if (frames[i].back_buffer != nullptr)
        {
            frames[i].back_buffer.Reset();
        }
    }

    // Clear dsv.
    depth_stencil_default_resource.Reset();

    // Resize back buffers.
    DXGI_SWAP_CHAIN_DESC sc_desc;
    check_hr(swapchain->GetDesc(&sc_desc));
    check_hr(swapchain->ResizeBuffers((UINT)num_render_targets, (UINT)width, (UINT)height,
                                      sc_desc.BufferDesc.Format,
                                      (UINT)sc_desc.Flags));

    // Re-create depth-stencil resource and view.
    create_dsv(width, height);

    // Re-create render target resources and views.
    for (UINT i = 0; i < num_render_targets; i++)
    {
        ComPtr<ID3D12Resource> back_buffer = nullptr;
        check_hr(swapchain->GetBuffer(i, IID_PPV_ARGS(&back_buffer)));
        device->CreateRenderTargetView(back_buffer.Get(), nullptr, render_targets[i].rtv_backbuffer);
        frames[i].back_buffer = back_buffer;
        NAME_D3D12_OBJECT_INDEXED(frames[i].back_buffer, i);
    }
}

gpu_interface::frame_resource::frame_resources_allocator::frame_resources_allocator(
    ComPtr<ID3D12Device> device,
    size_t size)
{
    // Create the frame resource upload buffer.
    check_hr(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(size),
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_upload_resource)));

    void *pdata;
    CD3DX12_RANGE read_range(0, 0);
    m_upload_resource->Map(0, &read_range, &pdata);
    m_current = m_begin = reinterpret_cast<UINT8 *>(pdata);
    m_end = m_begin + size;
}

UINT8 *gpu_interface::frame_resource::frame_resources_allocator::allocate(
    size_t size,
    size_t alignment)
{
    m_current = reinterpret_cast<UINT8 *>(align_up(reinterpret_cast<size_t>(m_current), alignment));
    ASSERT(m_current + size <= m_end, "Buffer is not full");
    UINT8 *ret = m_current;
    m_current += size;
    return ret;
}

gpu_interface::frame_resource::descriptor_table_frame_allocator::descriptor_table_frame_allocator(
    ComPtr<ID3D12Device> device,
    D3D12_DESCRIPTOR_HEAP_TYPE descriptor_type,
    UINT descriptor_count,
    UINT max_rename_count,
    UINT additional_descriptors_count)
{
    // Create the resources descriptor table allocator
    m_descriptor_count = descriptor_count;

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.NodeMask = DEFAULT_NODE;
    heap_desc.Type = descriptor_type;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    heap_desc.NumDescriptors = m_descriptor_count * SHADERSTAGE_MAX;
    check_hr(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap_cpu)));

    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap_desc.NumDescriptors = (m_descriptor_count * SHADERSTAGE_MAX * max_rename_count) + additional_descriptors_count;
    check_hr(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap_gpu)));

    m_descriptor_type = descriptor_type;
    m_descriptor_size = device->GetDescriptorHandleIncrementSize(descriptor_type);
    m_bound_descriptors.resize(SHADERSTAGE_MAX * descriptor_count);
}

void gpu_interface::frame_resource::descriptor_table_frame_allocator::reset_staging_heap(
    ComPtr<ID3D12Device> device,
    D3D12_CPU_DESCRIPTOR_HANDLE *null_descriptors_sampler_csu)
{
    memset(m_bound_descriptors.data(), 0, m_bound_descriptors.size() * sizeof(D3D12_CPU_DESCRIPTOR_HANDLE));
    m_ring_offset = 0;

    for (int stage = 0; stage < SHADERSTAGE_MAX; ++stage)
    {
        m_is_stage_dirty[stage] = true;

        // Fill staging tables with null descriptors
        if (m_descriptor_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
        {
            for (int slot = 0; slot < GPU_RESOURCE_HEAP_CBV_COUNT; ++slot)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE dst_staging = m_heap_cpu->GetCPUDescriptorHandleForHeapStart();
                dst_staging.ptr += (stage * m_descriptor_count + slot) * m_descriptor_size;

                device->CopyDescriptorsSimple(1,
                                              dst_staging, null_descriptors_sampler_csu[1],
                                              m_descriptor_type);
            }
            for (int slot = 0; slot < GPU_RESOURCE_HEAP_SRV_COUNT; ++slot)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE dst_staging = m_heap_cpu->GetCPUDescriptorHandleForHeapStart();
                dst_staging.ptr += (stage * m_descriptor_count + GPU_RESOURCE_HEAP_CBV_COUNT + slot) * m_descriptor_size;

                device->CopyDescriptorsSimple(1,
                                              dst_staging, null_descriptors_sampler_csu[2],
                                              m_descriptor_type);
            }
            for (int slot = 0; slot < GPU_RESOURCE_HEAP_UAV_COUNT; ++slot)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE dst_staging = m_heap_cpu->GetCPUDescriptorHandleForHeapStart();
                dst_staging.ptr += (stage * m_descriptor_count + GPU_RESOURCE_HEAP_CBV_COUNT + GPU_RESOURCE_HEAP_SRV_COUNT + slot) * m_descriptor_size;

                device->CopyDescriptorsSimple(1,
                                              dst_staging, null_descriptors_sampler_csu[3],
                                              m_descriptor_type);
            }
        }
    }
}

void gpu_interface::frame_resource::descriptor_table_frame_allocator::stage_to_cpu_heap(ComPtr<ID3D12Device> device,
                                                                                        shader_stages stage,
                                                                                        shader_descriptor_type type,
                                                                                        UINT bind_slot,
                                                                                        D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
    if (descriptor.ptr == 0)
    {
        return;
    }

    UINT offset_to_range = 0;
    switch (type)
    {
    case CBV:
        offset_to_range = bind_slot;
        break;
    case SRV:
        offset_to_range = bind_slot + GPU_RESOURCE_HEAP_CBV_COUNT;
        break;
    case UAV:
        offset_to_range = bind_slot + GPU_RESOURCE_HEAP_CBV_COUNT + GPU_RESOURCE_HEAP_SRV_COUNT;
        break;
    case sampler:
        offset_to_range = bind_slot;
        break;
    default:
        return;
        break;
    }

    UINT offset_to_table = stage * m_descriptor_count;
    UINT offset_to_descriptor = offset_to_table + offset_to_range;

    m_is_stage_dirty[stage] = true;
    if (m_bound_descriptors[offset_to_descriptor].ptr == descriptor.ptr)
    {
        return;
    }

    m_bound_descriptors[offset_to_descriptor] = descriptor;

    D3D12_CPU_DESCRIPTOR_HANDLE dst_staging = m_heap_cpu->GetCPUDescriptorHandleForHeapStart();
    dst_staging.ptr += offset_to_descriptor * m_descriptor_size;

    device->CopyDescriptorsSimple(1, dst_staging, descriptor, m_descriptor_type);
}

void gpu_interface::frame_resource::descriptor_table_frame_allocator::set_tables(
    ComPtr<ID3D12Device> device,
    ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    if (cmd_list->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE)
    {
        if (m_is_stage_dirty[CS])
        {
            D3D12_CPU_DESCRIPTOR_HANDLE dst = m_heap_gpu->GetCPUDescriptorHandleForHeapStart();
            dst.ptr += m_ring_offset;

            D3D12_CPU_DESCRIPTOR_HANDLE src = m_heap_cpu->GetCPUDescriptorHandleForHeapStart();
            src.ptr += (CS * m_descriptor_count) * m_descriptor_size;

            device->CopyDescriptorsSimple(m_descriptor_count, dst, src, m_descriptor_type);

            D3D12_GPU_DESCRIPTOR_HANDLE table_base_descriptor = m_heap_gpu->GetGPUDescriptorHandleForHeapStart();
            table_base_descriptor.ptr += m_ring_offset;

            if (m_descriptor_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
            {
                cmd_list->SetComputeRootDescriptorTable(0, table_base_descriptor); // Resource table.
            }
            else
            {
                cmd_list->SetComputeRootDescriptorTable(1, table_base_descriptor); // Sampler table.
            }
            m_is_stage_dirty[CS] = false;
            m_ring_offset += m_descriptor_count * m_descriptor_size;
        }
        return;
    }

    for (int stage = VS; stage < SHADERSTAGE_MAX; ++stage)
    {
        if (stage != CS)
        {
            if (m_is_stage_dirty[stage])
            {
                D3D12_CPU_DESCRIPTOR_HANDLE dst = m_heap_gpu->GetCPUDescriptorHandleForHeapStart();
                dst.ptr += m_ring_offset;

                D3D12_CPU_DESCRIPTOR_HANDLE src = m_heap_cpu->GetCPUDescriptorHandleForHeapStart();
                src.ptr += (stage * m_descriptor_count) * m_descriptor_size;

                device->CopyDescriptorsSimple(m_descriptor_count, dst, src, m_descriptor_type);

                D3D12_GPU_DESCRIPTOR_HANDLE table_base_descriptor = m_heap_gpu->GetGPUDescriptorHandleForHeapStart();
                table_base_descriptor.ptr += m_ring_offset;

                if (m_descriptor_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
                {
                    cmd_list->SetGraphicsRootDescriptorTable(stage * 2 + 0, table_base_descriptor); // Resource table.
                }
                else
                {
                    cmd_list->SetGraphicsRootDescriptorTable(stage * 2 + 1, table_base_descriptor); // Sampler table.
                }

                m_is_stage_dirty[stage] = false;
                m_ring_offset += m_descriptor_count * m_descriptor_size;
            }
        }
    }
}

bool gpu_interface::compile_shader(const wchar_t *file,
                                   const wchar_t *entry,
                                   shader_stages stage,
                                   ID3DBlob **blob,
                                   D3D_SHADER_MACRO *defines)
{
    WIN32_FIND_DATAW found_file;
    if (FindFirstFileW(file, &found_file) == INVALID_HANDLE_VALUE)
    {
        wchar_t can_file[MAX_PATH];
        PathCchCanonicalize(can_file, MAX_PATH, file);

        wchar_t cur_dir[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, cur_dir);

        wchar_t err_msg[MAX_PATH];
        wcscpy(err_msg, L"Shader file not found:");
        wcscat(err_msg, L"\n");
        wcscat(err_msg, can_file);
        wcscat(err_msg, L"\n\n");

        wcscat(err_msg, L"Working directory:");
        wcscat(err_msg, L"\n");
        wcscat(err_msg, cur_dir);
        wcscat(err_msg, L"\n\n");

        wcscat(err_msg, L"File input:");
        wcscat(err_msg, L"\n");
        wcscat(err_msg, file);

        if (MessageBoxW(NULL, err_msg,
                        L"Could not find shader.",
                        MB_OK | MB_ICONERROR | MB_DEFBUTTON2) != IDYES)
        {
            return false;
        }
    }

    ID3DBlob *error_blob = NULL;

    char shader_target_str[10];
    switch (stage)
    {
    case VS:
        strcpy(shader_target_str, "vs_5_1");
        break;
    case DS:
        strcpy(shader_target_str, "ds_5_1");
        break;
    case HS:
        strcpy(shader_target_str, "hs_5_1");
        break;
    case GS:
        strcpy(shader_target_str, "gs_5_1");
        break;
    case PS:
        strcpy(shader_target_str, "ps_5_1");
        break;
    case CS:
        strcpy(shader_target_str, "cs_5_1");
    default:
        break;
    }

    char entry_str[20];
    wcstombs(entry_str, entry, 20);

    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#endif

#ifdef NDEBUG
    flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    HRESULT hr = D3DCompileFromFile(file,
                                    defines,
                                    D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    entry_str,
                                    shader_target_str,
                                    flags,
                                    0,
                                    blob,
                                    &error_blob);
    if (error_blob)
    {
        char *error_msg = (char *)error_blob->GetBufferPointer();
        OutputDebugStringA(error_msg);

        if (MessageBoxA(NULL, error_msg,
                        "Shader compilation error.",
                        MB_OK | MB_ICONERROR | MB_DEFBUTTON2) != IDYES)
        {
            return false;
        }
    }
    check_hr(hr);

    return true;
}

ComPtr<ID3D12RootSignature> gpu_interface::create_rootsig(std::vector<D3D12_ROOT_PARAMETER1> *params,
                                                          std::vector<CD3DX12_STATIC_SAMPLER_DESC> *samplers,
                                                          D3D12_ROOT_SIGNATURE_FLAGS flags)
{
    D3D12_FEATURE_DATA_ROOT_SIGNATURE feature_data = {};
    feature_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    check_hr(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, (void *)&feature_data, sizeof(feature_data)));

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned_rootsig_desc = {};
    versioned_rootsig_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versioned_rootsig_desc.Desc_1_1.Flags = flags;

    ASSERT(params->size() > 0, "At least 1 root signature parameter is required");
    versioned_rootsig_desc.Desc_1_1.NumParameters = (UINT)params->size();
    versioned_rootsig_desc.Desc_1_1.pParameters = params->data();

    if (samplers)
    {
        versioned_rootsig_desc.Desc_1_1.NumStaticSamplers = (UINT)samplers->size();
        versioned_rootsig_desc.Desc_1_1.pStaticSamplers = samplers->data();
    }

    ID3DBlob *rs_blob = nullptr;
    ID3DBlob *error_blob = nullptr;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&versioned_rootsig_desc, &rs_blob, &error_blob);

    if (error_blob)
    {
        char *error_msg = (char *)error_blob->GetBufferPointer();
        OutputDebugStringA(error_msg);
    }
    safe_release(error_blob);
    check_hr(hr);

    ComPtr<ID3D12RootSignature> rootsig;
    check_hr(device->CreateRootSignature(
        DEFAULT_NODE,
        rs_blob->GetBufferPointer(),
        rs_blob->GetBufferSize(),
        IID_PPV_ARGS(&rootsig)));
    return rootsig;
}

std::vector<D3D12_RESOURCE_BARRIER> gpu_interface::transition(
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
    const std::vector<ComPtr<ID3D12Resource>> &resources,
    ComPtr<ID3D12GraphicsCommandList> cmd_list,
    UINT subresource,
    D3D12_RESOURCE_BARRIER_FLAGS flags)
{
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    D3D12_RESOURCE_BARRIER transition;
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Flags = flags;
    transition.Transition.Subresource = subresource;
    transition.Transition.StateBefore = before;
    transition.Transition.StateAfter = after;

    for (auto &resource : resources)
    {
        transition.Transition.pResource = resource.Get();
        barriers.push_back(transition);
    }

    if (cmd_list)
    {
        cmd_list->ResourceBarrier((UINT)barriers.size(), barriers.data());
    }
    return barriers;
}

void gpu_interface::default_resource_from_uploader(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                                                   ID3D12Resource **default_resource,
                                                   const void *data,
                                                   size_t byte_size,
                                                   size_t alignment,
                                                   D3D12_RESOURCE_FLAGS flags)
{
    size_t aligned_byte_size = align_up(byte_size, alignment);
    check_hr(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(aligned_byte_size, flags),
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(default_resource)));
    ID3D12Resource *p_default_resource = (*default_resource);

    UINT8 *upload_dest = m_buffer_uploader.allocate(byte_size, aligned_byte_size);
    if (data != nullptr)
    {
        memcpy(upload_dest, data, byte_size);
    }

    size_t offset = upload_dest - m_buffer_uploader.m_begin;
    cmd_list->CopyBufferRegion(p_default_resource, 0,
                               m_buffer_uploader.m_upload_resource.Get(), offset,
                               byte_size);
}

UINT8 *gpu_interface::resource_uploader::allocate(UINT64 data_size, UINT64 alignment)
{
    data_size = align_up(data_size, alignment);
    ASSERT(m_current + data_size <= m_end, "Buffer is not full");

    UINT8 *ret = m_current;
    m_current += data_size;

    return ret;
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC gpu_interface::create_default_pso_desc()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC default_pso_desc = {};
    default_pso_desc.NodeMask = DEFAULT_NODE;
    default_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    default_pso_desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    default_pso_desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    default_pso_desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    default_pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    default_pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    default_pso_desc.SampleMask = UINT_MAX;
    default_pso_desc.SampleDesc.Count = 1;
    default_pso_desc.SampleDesc.Quality = 0;
    return default_pso_desc;
}

void gpu_interface::create_dsv(UINT64 width, UINT height)
{
    D3D12_RESOURCE_DESC ds_desc = CD3DX12_RESOURCE_DESC::Tex2D(DEPTH_STENCIL_FORMAT,
                                                               width,
                                                               height,
                                                               1, 1);

    ds_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    ds_desc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    ds_desc.Format = DXGI_FORMAT_R24G8_TYPELESS;

    D3D12_CLEAR_VALUE clear_value = {};
    clear_value.Format = DEPTH_STENCIL_FORMAT;
    clear_value.DepthStencil.Depth = 1.0f;
    clear_value.DepthStencil.Stencil = 0;

    check_hr(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &ds_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clear_value,
        IID_PPV_ARGS(&depth_stencil_default_resource)));
    NAME_D3D12_OBJECT(depth_stencil_default_resource);

    // Depth stencil view
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desv = {};
    dsv_desv.Flags = D3D12_DSV_FLAG_NONE;
    dsv_desv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsv_desv.Format = DEPTH_STENCIL_FORMAT;

    device->CreateDepthStencilView(depth_stencil_default_resource.Get(), &dsv_desv,
                                   dsv_heap->GetCPUDescriptorHandleForHeapStart());
}

void gpu_interface::set_descriptor_tables(ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    frame_resource *frame = get_frame_resource();
    frame->csu_table_allocator.set_tables(device, cmd_list);
    frame->sampler_table_allocator.set_tables(device, cmd_list);
}

void gpu_interface::flush_graphics_queue()
{
    // Mark commands up to this point.
    previous_frame_index = get_frame_resource()->fence_value;
    UINT64 fence_to_signal = previous_frame_index + 1;
    graphics_cmd_queue->Signal(fence.Get(), fence_to_signal);
    cpu_wait_for_fence(fence_to_signal);

    // Don't move to the next frame. No call to Present() is made.
    // Instead just increase the fence value of the current frame accordingly.
    frames[swapchain->GetCurrentBackBufferIndex()].fence_value = fence_to_signal + 1;
}

gpu_interface::gbuffer gpu_interface::create_gbuffer(DXGI_FORMAT format, D3D12_RESOURCE_STATES initial_state)
{
    gbuffer buffer = {};
    buffer.format = format;

    // Create gbuffer default texture resource.
    D3D12_RESOURCE_DESC gbuffer0_tex_desc = {};
    gbuffer0_tex_desc.Width = g_hwnd_width;
    gbuffer0_tex_desc.Height = g_hwnd_height;
    gbuffer0_tex_desc.DepthOrArraySize = 1;
    gbuffer0_tex_desc.Alignment = 0;
    gbuffer0_tex_desc.MipLevels = 1;
    gbuffer0_tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                              D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    gbuffer0_tex_desc.SampleDesc.Count = 1;
    gbuffer0_tex_desc.SampleDesc.Quality = 0;
    gbuffer0_tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    gbuffer0_tex_desc.Format = format;
    gbuffer0_tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_CLEAR_VALUE clear_value = {};
    clear_value.Format = format;
    clear_value.Color[0] = 0.f;
    clear_value.Color[1] = 0.f;
    clear_value.Color[2] = 0.f;
    clear_value.Color[3] = 0.f;
    check_hr(device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                                             D3D12_HEAP_FLAG_NONE,
                                             &gbuffer0_tex_desc,
                                             initial_state,
                                             &clear_value,
                                             IID_PPV_ARGS(&buffer.rt_default_resource)));

    // Create gbuffer RTV.
    buffer.rtv_handle.ptr = rtv_allocator.allocate();

    D3D12_RENDER_TARGET_VIEW_DESC gbuffer0_rtv_desc = {};
    gbuffer0_rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    gbuffer0_rtv_desc.Texture2D.MipSlice = 0;
    gbuffer0_rtv_desc.Texture2D.PlaneSlice = 0;
    gbuffer0_rtv_desc.Format = format;
    device->CreateRenderTargetView(buffer.rt_default_resource.Get(),
                                   &gbuffer0_rtv_desc, buffer.rtv_handle);

    // Create gbuffer SRV.
    buffer.srv_handle.ptr = csu_allocator.allocate();

    D3D12_SHADER_RESOURCE_VIEW_DESC gbuffer0_srv_desc = {};
    gbuffer0_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    gbuffer0_srv_desc.Texture2D.MipLevels = 1;
    gbuffer0_srv_desc.Texture2D.MostDetailedMip = 0;
    gbuffer0_srv_desc.Texture2D.PlaneSlice = 0;
    gbuffer0_srv_desc.Texture2D.ResourceMinLODClamp = 0.f;
    gbuffer0_srv_desc.Format = format;
    gbuffer0_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    device->CreateShaderResourceView(buffer.rt_default_resource.Get(),
                                     &gbuffer0_srv_desc, buffer.srv_handle);
    return buffer;
}

gpu_interface::render_target gpu_interface::create_render_target(int index, DXGI_FORMAT format, D3D12_RESOURCE_STATES initial_state)
{
    render_target rt = {};

    // Assign formats.
    frame_resource *frame = &frames[index];
    check_hr(swapchain->GetBuffer(index, IID_PPV_ARGS(&frame->back_buffer)));
    rt.ldr_format = frame->back_buffer->GetDesc().Format;
    rt.hdr_format = format;

    // Create the resource.
    D3D12_RESOURCE_DESC rt_resource_desc = {};
    rt_resource_desc.Format = rt.hdr_format;
    rt_resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_resource_desc.Alignment = 0;
    rt_resource_desc.DepthOrArraySize = 1;
    rt_resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    rt_resource_desc.Height = g_hwnd_height;
    rt_resource_desc.Width = g_hwnd_width;
    rt_resource_desc.SampleDesc.Count = 1;
    rt_resource_desc.SampleDesc.Quality = 0;
    rt_resource_desc.MipLevels = 1;
    rt_resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES rt_heap_desc = {};
    rt_heap_desc.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    rt_heap_desc.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    rt_heap_desc.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear_value = {};
    clear_value.Color[0] = 0.f;
    clear_value.Color[1] = 0.f;
    clear_value.Color[2] = 0.f;
    clear_value.Color[3] = 1.f;
    clear_value.Format = rt.hdr_format;
    check_hr(device->CreateCommittedResource(&rt_heap_desc, D3D12_HEAP_FLAG_NONE,
                                             &rt_resource_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                             &clear_value, IID_PPV_ARGS(rt.rt_default_resource.GetAddressOf())));
    NAME_D3D12_OBJECT_INDEXED(rt.rt_default_resource, index);

    // Create the RTVs.
    rt.rtv_hdr.ptr = rtv_allocator.allocate();
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = rt.hdr_format;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtv_desc.Texture2D.MipSlice = 0;
    rtv_desc.Texture2D.PlaneSlice = 0;
    device->CreateRenderTargetView(rt.rt_default_resource.Get(), &rtv_desc, rt.rtv_hdr);

    rt.rtv_backbuffer.ptr = rtv_allocator.allocate();
    device->CreateRenderTargetView(frame->back_buffer.Get(), nullptr, rt.rtv_backbuffer);

    // Create the SRV.
    rt.srv_handle.ptr = csu_allocator.allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC rt_srv_desc = {};
    rt_srv_desc.Format = rt.hdr_format;
    rt_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    rt_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    rt_srv_desc.Texture2D.MipLevels = 1;
    rt_srv_desc.Texture2D.MostDetailedMip = 0;
    rt_srv_desc.Texture2D.PlaneSlice = 0;
    rt_srv_desc.Texture2D.ResourceMinLODClamp = 0.f;
    device->CreateShaderResourceView(rt.rt_default_resource.Get(), &rt_srv_desc, rt.srv_handle);

    return rt;
}

void gpu_interface::cpu_wait_for_fence(UINT64 fence_value)
{
    if (fence_value == 0)
        return; // No fence was signaled
    if (fence->GetCompletedValue() >= fence_value)
        return; // We're already exactly at that fence value, or past that fence value

    fence->SetEventOnCompletion(fence_value, cpu_wait_event);
    WaitForSingleObject(cpu_wait_event, INFINITE);
}

void gpu_interface::next_frame()
{
    check_hr(swapchain->Present(0, 0));

    UINT64 current_frame_fence_value = get_frame_resource()->fence_value;
    UINT64 next_frame_fence_value = current_frame_fence_value + 1;

    check_hr(graphics_cmd_queue->Signal(fence.Get(), current_frame_fence_value));
    PIXSetMarker(graphics_cmd_queue.Get(), 0, "graphics_cmd_queue signal(%d)", current_frame_fence_value);

    // Update the frame_index. GetCurrentBackBufferIndex() gets incremented after swapchain->Present() calls.
    previous_frame_index = frame_index;
    frame_index = swapchain->GetCurrentBackBufferIndex();

    // The GPU must have reached at least up to the fence value of the frame we're about to render.
    minimum_fence = frames[frame_index].fence_value;
    completed_fence = fence->GetCompletedValue();

    if (completed_fence < minimum_fence)
    {
        PIXBeginEvent(0, "CPU Waiting for GPU to reach fence value: %d", minimum_fence);
        // Wait for the next frame resource to be ready.
        fence->SetEventOnCompletion(minimum_fence, fence_event);
        WaitForSingleObject(fence_event, INFINITE);
        PIXEndEvent();
    }

    frames[frame_index].fence_value = next_frame_fence_value;

    {
        // CPU and GPU frame-to-frame event.
        PIXEndEvent(graphics_cmd_queue.Get());
        PIXBeginEvent(graphics_cmd_queue.Get(), 0, "fence value: %d", next_frame_fence_value);
    }
}

std::string gpu_interface::get_full_event_name(std::string event_name)
{
    wchar_t *p = NULL;
    check_hr(GetThreadDescription(GetCurrentThread(), &p));
    std::wstring wthread(p);
    LocalFree(p);
    std::string full_event_name = event_name + " (" + std::string(wthread.begin(), wthread.end()) + ")";
    return full_event_name;
}

void gpu_interface::timer_start(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                                std::string event_name)
{
    mtx.lock();
    // GPU.
    timers[event_name].cmd_list[frame_index] = cmd_list.Get();
    m_gpu_timer.start(event_name, cmd_list);

    // CPU.
    std::string full_event_name = get_full_event_name(event_name);
    g_cpu_timer.start(full_event_name);

    std::vector<std::string> &cpu_events = timers.at(event_name).cpu_event_names;
    bool cpu_event_found = std::find(cpu_events.begin(), cpu_events.end(), full_event_name) != cpu_events.end();
    if (!cpu_event_found)
    {
        cpu_events.push_back(full_event_name);
    }

    // PIX.
    PIXBeginEvent(cmd_list.Get(), 0, full_event_name.c_str());
    mtx.unlock();
}

void gpu_interface::timer_stop(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                               std::string event_name)
{
    mtx.lock();
    // GPU.
    m_gpu_timer.stop(event_name, cmd_list);
    m_gpu_timer.resolve(event_name, cmd_list);

    // CPU.
    std::string full_event_name = get_full_event_name(event_name);
    g_cpu_timer.stop(full_event_name);

    // PIX.
    PIXEndEvent(cmd_list.Get());
    mtx.unlock();
}

double gpu_interface::result_cpu(std::string event_name)
{
    return g_cpu_timer.result_ms(event_name);
}

double gpu_interface::result_gpu(std::string event_name)
{
    return m_gpu_timer.result(event_name, timers[event_name].cmd_list[frame_index]);
}

ComPtr<ID3D12RootSignature> gpu_interface::create_compute_staging_rootsig(std::vector<D3D12_ROOT_PARAMETER1> additional_parameters, UINT space)
{
    std::vector<D3D12_ROOT_PARAMETER1> params;

    // Staging params.
    // Static ranges that never change, the size of each range is assumed to be enough for any shader.
    D3D12_DESCRIPTOR_RANGE1 sampler_range = {};
    sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    sampler_range.BaseShaderRegister = 0;
    sampler_range.RegisterSpace = space;
    sampler_range.NumDescriptors = GPU_SAMPLER_HEAP_COUNT;
    sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    sampler_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

    const UINT descriptor_range_count = 3;
    D3D12_DESCRIPTOR_RANGE1 descriptor_ranges[descriptor_range_count];

    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    descriptor_ranges[0].BaseShaderRegister = 0;
    descriptor_ranges[0].RegisterSpace = space;
    descriptor_ranges[0].NumDescriptors = GPU_RESOURCE_HEAP_CBV_COUNT;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptor_ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

    descriptor_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_ranges[1].BaseShaderRegister = 0;
    descriptor_ranges[1].RegisterSpace = space;
    descriptor_ranges[1].NumDescriptors = GPU_RESOURCE_HEAP_SRV_COUNT;
    descriptor_ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptor_ranges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

    descriptor_ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[2].BaseShaderRegister = 0;
    descriptor_ranges[2].RegisterSpace = space;
    descriptor_ranges[2].NumDescriptors = GPU_RESOURCE_HEAP_UAV_COUNT;
    descriptor_ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptor_ranges[2].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

    D3D12_ROOT_PARAMETER1 staging_param_csu = {};
    staging_param_csu.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    staging_param_csu.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    staging_param_csu.DescriptorTable.NumDescriptorRanges = descriptor_range_count;
    staging_param_csu.DescriptorTable.pDescriptorRanges = descriptor_ranges;
    params.push_back(staging_param_csu);

    D3D12_ROOT_PARAMETER1 staging_param_samplers = {};
    staging_param_samplers.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    staging_param_samplers.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    staging_param_samplers.DescriptorTable.NumDescriptorRanges = 1;
    staging_param_samplers.DescriptorTable.pDescriptorRanges = &sampler_range;
    params.push_back(staging_param_samplers);

    params.insert(params.end(), additional_parameters.begin(), additional_parameters.end());
    return create_rootsig(&params, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
}

ComPtr<ID3D12RootSignature> gpu_interface::create_graphics_staging_rootsig(std::vector<D3D12_ROOT_PARAMETER1> additional_parameters, UINT space)
{
    D3D12_DESCRIPTOR_RANGE1 sampler_range = {};
    sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    sampler_range.BaseShaderRegister = 0;
    sampler_range.RegisterSpace = space;
    sampler_range.NumDescriptors = GPU_SAMPLER_HEAP_COUNT;
    sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    sampler_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

    const UINT descriptor_range_count = 3;
    D3D12_DESCRIPTOR_RANGE1 descriptor_ranges[descriptor_range_count];

    // Static ranges that never change, the size of each range is assumed to be enough for any shader.
    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    descriptor_ranges[0].BaseShaderRegister = 0;
    descriptor_ranges[0].RegisterSpace = space;
    descriptor_ranges[0].NumDescriptors = GPU_RESOURCE_HEAP_CBV_COUNT;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptor_ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

    descriptor_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_ranges[1].BaseShaderRegister = 0;
    descriptor_ranges[1].RegisterSpace = space;
    descriptor_ranges[1].NumDescriptors = GPU_RESOURCE_HEAP_SRV_COUNT;
    descriptor_ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptor_ranges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

    descriptor_ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[2].BaseShaderRegister = 0;
    descriptor_ranges[2].RegisterSpace = space;
    descriptor_ranges[2].NumDescriptors = GPU_RESOURCE_HEAP_UAV_COUNT;
    descriptor_ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptor_ranges[2].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

    UINT param_count = 2 * (SHADERSTAGE_MAX - 1); // 2: resource,sampler;   5: vs,hs,ds,gs,ps;

    std::vector<D3D12_ROOT_PARAMETER1> params;
    params.resize(param_count);

    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[0].DescriptorTable.NumDescriptorRanges = descriptor_range_count;
    params[0].DescriptorTable.pDescriptorRanges = descriptor_ranges;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &sampler_range;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_HULL;
    params[2].DescriptorTable.NumDescriptorRanges = descriptor_range_count;
    params[2].DescriptorTable.pDescriptorRanges = descriptor_ranges;

    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_HULL;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &sampler_range;

    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_DOMAIN;
    params[4].DescriptorTable.NumDescriptorRanges = descriptor_range_count;
    params[4].DescriptorTable.pDescriptorRanges = descriptor_ranges;

    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_DOMAIN;
    params[5].DescriptorTable.NumDescriptorRanges = 1;
    params[5].DescriptorTable.pDescriptorRanges = &sampler_range;

    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_GEOMETRY;
    params[6].DescriptorTable.NumDescriptorRanges = descriptor_range_count;
    params[6].DescriptorTable.pDescriptorRanges = descriptor_ranges;

    params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_GEOMETRY;
    params[7].DescriptorTable.NumDescriptorRanges = 1;
    params[7].DescriptorTable.pDescriptorRanges = &sampler_range;

    params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[8].DescriptorTable.NumDescriptorRanges = descriptor_range_count;
    params[8].DescriptorTable.pDescriptorRanges = descriptor_ranges;

    params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[9].DescriptorTable.NumDescriptorRanges = 1;
    params[9].DescriptorTable.pDescriptorRanges = &sampler_range;

    params.insert(params.end(), additional_parameters.begin(), additional_parameters.end());

    return create_rootsig(&params, nullptr);
}

void gpu_interface::create_descriptor_allocators()
{
    UINT rtv_handle_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    UINT dsv_handle_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    UINT csu_handle_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    UINT sampler_handle_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    rtv_allocator.descriptor_count.store(0);
    dsv_allocator.descriptor_count.store(0);
    csu_allocator.descriptor_count.store(0);
    sampler_allocator.descriptor_count.store(0);

    rtv_allocator.descriptor_size = rtv_handle_size;
    dsv_allocator.descriptor_size = dsv_handle_size;
    csu_allocator.descriptor_size = csu_handle_size;
    sampler_allocator.descriptor_size = sampler_handle_size;

    rtv_allocator.max_descriptor_count = 128;
    dsv_allocator.max_descriptor_count = 1024;
    csu_allocator.max_descriptor_count = 4096;
    sampler_allocator.max_descriptor_count = 64;

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.NodeMask = 0;
    heap_desc.NumDescriptors = rtv_allocator.max_descriptor_count;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    check_hr(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_allocator.m_staging_heap)));

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heap_desc.NumDescriptors = dsv_allocator.max_descriptor_count;
    check_hr(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsv_allocator.m_staging_heap)));

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = csu_allocator.max_descriptor_count;
    check_hr(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&csu_allocator.m_staging_heap)));

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    heap_desc.NumDescriptors = sampler_allocator.max_descriptor_count;
    check_hr(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&sampler_allocator.m_staging_heap)));
}

D3D12_RESOURCE_ALLOCATION_INFO gpu_interface::upload_dds(const std::wstring &file,
                                                         ComPtr<ID3D12GraphicsCommandList> cmd_list,
                                                         ID3D12Resource **texture_resource,
                                                         bool generate_mips,
                                                         ComPtr<ID3D12Heap> texture_heap, size_t heap_offset)
{
    std::wstring file_name_dds = remove_extension(file) + L".dds";

    // Check if a .dds version of the file already exist.
    WIN32_FIND_DATA find_file_data;
    HANDLE handle = FindFirstFile(file_name_dds.c_str(), &find_file_data);
    bool dds_file_found = handle != INVALID_HANDLE_VALUE;
    FindClose(handle);

    // If the .dds version of the file doesn't exist, create it.
    if (!dds_file_found)
    {
        std::string file_narrow_str = std::string(file.begin(), file.end());

        // Load the pixels.
        int channels = 0;
        int width = 0;
        int height = 0;
        uint8_t *pixels;
        Image img;
        size_t row_pitch;
        size_t slice_pitch;

        if (stbi_is_hdr(file_narrow_str.c_str()))
        {
            img.format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            pixels = reinterpret_cast<uint8_t *>(stbi_loadf(file_narrow_str.c_str(),
                                                            &width, &height,
                                                            &channels, 4));
            check_hr(ComputePitch(DXGI_FORMAT_R32G32B32A32_FLOAT,
                                  width, height,
                                  row_pitch, slice_pitch));
        }
        else
        {
            img.format = DXGI_FORMAT_R8G8B8A8_UNORM;
            pixels = stbi_load(file_narrow_str.c_str(),
                               &width, &height,
                               &channels, 4);
            check_hr(ComputePitch(DXGI_FORMAT_R8G8B8A8_UNORM,
                                  width, height,
                                  row_pitch, slice_pitch));
        }

        img.pixels = pixels;
        img.width = width;
        img.height = height;
        img.rowPitch = row_pitch;
        img.slicePitch = slice_pitch;
        ScratchImage base_img;
        check_hr(base_img.InitializeFromImage(img));

        if (generate_mips)
        {
            ScratchImage mip_chain;
            check_hr(GenerateMipMaps(base_img.GetImages(), base_img.GetImageCount(),
                                     base_img.GetMetadata(), TEX_FILTER_DEFAULT,
                                     0, // Zero means creating a full mip chain down to 1x1.
                                     mip_chain));
            check_hr(SaveToDDSFile(mip_chain.GetImages(), mip_chain.GetImageCount(), mip_chain.GetMetadata(),
                                   DDS_FLAGS_NONE,
                                   file_name_dds.c_str()));
        }
        else
        {
            check_hr(SaveToDDSFile(base_img.GetImages(), base_img.GetImageCount(), base_img.GetMetadata(),
                                   DDS_FLAGS_NONE,
                                   file_name_dds.c_str()));
        }
    }

    // Load dds subresource data.
    ScratchImage dds_img;
    TexMetadata dds_md;
    check_hr(LoadFromDDSFile(file_name_dds.c_str(), DDS_FLAGS_NONE, &dds_md, dds_img));

    const Image *dds_images = dds_img.GetImages();
    std::vector<D3D12_SUBRESOURCE_DATA> subresources;
    subresources.reserve(dds_md.mipLevels);

    D3D12_RESOURCE_DESC tex_desc = {};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.DepthOrArraySize = (UINT16)dds_md.arraySize;
    tex_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    tex_desc.Format = dds_md.format;
    tex_desc.Width = dds_md.width;
    tex_desc.Height = (UINT)dds_md.height;
    tex_desc.MipLevels = (UINT16)dds_md.mipLevels;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.SampleDesc.Quality = 0;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_RESOURCE_ALLOCATION_INFO alloc_info = device->GetResourceAllocationInfo(0, 1, &tex_desc);
    tex_desc.Alignment = alloc_info.Alignment;

    for (size_t j = 0; j < dds_md.mipLevels; j++)
    {
        const Image *img = &dds_images[j];
        D3D12_SUBRESOURCE_DATA subr;
        subr.pData = img->pixels;
        subr.RowPitch = img->rowPitch;
        subr.SlicePitch = img->slicePitch;
        subresources.push_back(subr);
    }

    if (texture_heap)
    {
        // Create the default resource to hold the fire sprite texture.
        check_hr(device->CreatePlacedResource(texture_heap.Get(), heap_offset,
                                              &tex_desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(texture_resource)));
    }
    else
    {
        check_hr(device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                                                 D3D12_HEAP_FLAG_NONE,
                                                 &tex_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(texture_resource)));
    }

    // CopyTextureRegion() from upload resource to default resource.
    UpdateSubresources(cmd_list.Get(),
                       *texture_resource,
                       m_texture_uploader.Get(), heap_offset,
                       0, (UINT)subresources.size(), subresources.data());

    return alloc_info;
}

size_t gpu_interface::descriptor_allocator::allocate()
{
    return m_staging_heap->GetCPUDescriptorHandleForHeapStart().ptr + descriptor_count.fetch_add(1) * descriptor_size;
}

void gpu_interface::create_null_descriptors()
{
    D3D12_SAMPLER_DESC sampler_desc = {};
    sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    m_null_sampler = D3D12_CPU_DESCRIPTOR_HANDLE();
    m_null_sampler.ptr = sampler_allocator.allocate();
    device->CreateSampler(&sampler_desc, m_null_sampler);

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {};
    m_null_cbv = D3D12_CPU_DESCRIPTOR_HANDLE();
    m_null_cbv.ptr = csu_allocator.allocate();
    device->CreateConstantBufferView(&cbv_desc, m_null_cbv);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = DXGI_FORMAT_R32_UINT;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    m_null_srv = D3D12_CPU_DESCRIPTOR_HANDLE();
    m_null_srv.ptr = csu_allocator.allocate();
    device->CreateShaderResourceView(nullptr, &srv_desc, m_null_srv);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = DXGI_FORMAT_R32_UINT;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    m_null_uav = D3D12_CPU_DESCRIPTOR_HANDLE();
    m_null_uav.ptr = csu_allocator.allocate();
    device->CreateUnorderedAccessView(nullptr, nullptr, &uav_desc, m_null_uav);
}

void gpu_interface::reset_staging_descriptors()
{
    D3D12_CPU_DESCRIPTOR_HANDLE null_descriptors[4] = {
        m_null_sampler, m_null_cbv, m_null_srv, m_null_uav};

    gpu_interface::frame_resource *frame_resource = get_frame_resource();
    frame_resource->csu_table_allocator.reset_staging_heap(device, null_descriptors);
    frame_resource->sampler_table_allocator.reset_staging_heap(device, null_descriptors);
    frame_resource->m_resources_buffer.m_current = frame_resource->m_resources_buffer.m_begin;
}

void gpu_interface::set_staging_heaps(ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    gpu_interface::frame_resource *frame_resource = get_frame_resource();

    ID3D12DescriptorHeap *heaps[] = {
        frame_resource->csu_table_allocator.m_heap_gpu.Get(),
        frame_resource->sampler_table_allocator.m_heap_gpu.Get()};

    cmd_list->SetDescriptorHeaps(_countof(heaps), heaps);
}
