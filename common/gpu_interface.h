#pragma once
#include "directx12_include.h"
#include "common.h"
#include <vector>
#include <atomic>
#include "gpu_timer.h"
#include <mutex>

enum shader_stages
{
    VS,
    HS,
    DS,
    GS,
    PS,
    CS,
    SHADERSTAGE_MAX
};

enum shader_descriptor_type
{
    CBV,
    SRV,
    UAV,
    sampler
};

#pragma warning(push)
#pragma warning(disable : 4251) // Safe to ignore because the users of this DLL will always be compiled together with the DLL

struct COMMON_API gpu_interface
{
    gpu_interface() = default;
    ~gpu_interface() = default;

    static const UINT32 DEFAULT_NODE = 0;
    static const UINT32 NUM_BACK_BUFFERS = 3;

    // Formats
    static const DXGI_FORMAT DEPTH_STENCIL_FORMAT = DXGI_FORMAT_D24_UNORM_S8_UINT;

    // Descriptor layout counts:
    static const UINT32 GPU_RESOURCE_HEAP_CBV_COUNT = 12;
    static const UINT32 GPU_RESOURCE_HEAP_SRV_COUNT = 64;
    static const UINT32 GPU_RESOURCE_HEAP_UAV_COUNT = 8;
    static const UINT32 GPU_SAMPLER_HEAP_COUNT = 16;
    static const UINT32 max_rename_count = 1024;
    static const UINT32 staging_descriptors_per_stage = GPU_RESOURCE_HEAP_CBV_COUNT +
                                                        GPU_RESOURCE_HEAP_SRV_COUNT +
                                                        GPU_RESOURCE_HEAP_UAV_COUNT;
    static const UINT32 num_csu_staging_descriptors = staging_descriptors_per_stage * SHADERSTAGE_MAX * max_rename_count;

    // Descriptor sizes
    UINT32 rtv_descriptor_size;
    UINT32 dsv_descriptor_size;
    UINT32 csu_descriptor_size;

    // Core utilities
    void init_core(DXGI_FORMAT back_buffer_format);
    void init_frame_resources(UINT additional_descriptors_count);
    bool compile_shader(const wchar_t *file,
                        const wchar_t *entry,
                        shader_stages stage,
                        ID3DBlob **blob,
                        D3D_SHADER_MACRO *defines);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC create_default_pso_desc();
    ComPtr<ID3D12RootSignature> create_rootsig(std::vector<D3D12_ROOT_PARAMETER1> *params,
                                               std::vector<CD3DX12_STATIC_SAMPLER_DESC> *samplers,
                                               D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    std::vector<D3D12_RESOURCE_BARRIER> transition(
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
        const std::vector<ComPtr<ID3D12Resource>> &resources,
        ComPtr<ID3D12GraphicsCommandList> cmd_list = nullptr,
        UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_BARRIER_FLAGS flags = D3D12_RESOURCE_BARRIER_FLAG_NONE);

    void default_resource_from_uploader(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                                        ID3D12Resource **default_resource,
                                        const void *data,
                                        size_t byte_size,
                                        size_t alignment,
                                        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);

    void create_dsv(UINT64 width, UINT height);
    void set_descriptor_tables(ComPtr<ID3D12GraphicsCommandList> cmd_list);

    template <typename T>
    struct constant_buffer
    {
        constant_buffer() = default;
        void update(T *data, ComPtr<ID3D12GraphicsCommandList> cmd_list)
        {
            gpu_interface::frame_resource *frame = m_gpu->get_frame_resource();
            UINT8 *dest = frame->m_resources_buffer.allocate(data_size, m_alignment);
            memcpy(dest, data, data_size);
            size_t offset = dest - frame->m_resources_buffer.m_begin;
            cmd_list->CopyBufferRegion(default_resource.Get(), 0,
                                       frame->m_resources_buffer.m_upload_resource.Get(), offset,
                                       data_size);
            return;
        }
        size_t data_size;
        size_t m_alignment;
        ComPtr<ID3D12Resource> default_resource;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle;
        gpu_interface *m_gpu;
    };
    template <typename T>
    gpu_interface::constant_buffer<T> create_constant_buffer(T *data,
                                                             size_t num_elements = 1,
                                                             D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);

    // EXPERIMENTAL: Reuse the temporary uploader as an upload buffer.
    template <typename T>
    struct scratch_buffer
    {
        scratch_buffer() = default;
        void update(T *data)
        {
            // Find the next available memory inside the current frame's resource buffer.
            gpu_interface::frame_resource *frame = m_gpu->get_frame_resource();
            UINT8 *upload_resource_alloc = m_gpu->get_frame_resource()->m_resources_buffer.allocate(m_unaligned_size, m_alignment);

            // Update the data.
            memcpy(upload_resource_alloc, data, m_unaligned_size);

            // We calculate what the offset is using the CPU pointers, but we are updating the GPU pointer.
            size_t data_offset_size = upload_resource_alloc - frame->m_resources_buffer.m_begin;

            // Update the *GPU* virtual address to the beginning of the new data.
            m_gpu_va = frame->m_resources_buffer.m_upload_resource->GetGPUVirtualAddress() + data_offset_size;
        }

        gpu_interface *m_gpu;
        D3D12_GPU_VIRTUAL_ADDRESS m_gpu_va;
        size_t m_unaligned_size;
        size_t m_num_elements;
        size_t m_alignment;
        size_t m_datum_size;
        size_t m_aligned_size;
    };
    template <typename T>
    gpu_interface::scratch_buffer<T> create_scratch_buffer(T *data, size_t num_elements, bool is_constant_buffer = false);

    template <typename T>
    struct upload_buffer
    {
        upload_buffer() = default;
        void update(T *data, size_t element_index)
        {
            memcpy(&m_mapped_data[element_index * m_datum_size], data, m_datum_size);
        }

        gpu_interface *m_gpu;
        ComPtr<ID3D12Resource> m_upload_resource;
        uint8_t *m_mapped_data;
        size_t m_unaligned_size;
        size_t m_num_elements;
        size_t m_alignment;
        size_t m_datum_size;
        size_t m_aligned_size;
    };
    template <typename T>
    gpu_interface::upload_buffer<T> create_upload_buffer(T *data, size_t num_elements, bool is_constant_buffer = false);

    template <typename T>
    struct structured_buffer
    {
        structured_buffer() = default;
        void update(T *data, size_t count, size_t index, ComPtr<ID3D12GraphicsCommandList> cmd_list)
        {
            size_t data_size = count * m_datum_size;
            gpu_interface::frame_resource *frame = m_gpu->get_frame_resource();
            UINT8 *dest = frame->m_resources_buffer.allocate(data_size, m_alignment);
            memcpy(dest, data, data_size);
            size_t src_offset = dest - frame->m_resources_buffer.m_begin;
            cmd_list->CopyBufferRegion(default_resource.Get(), index * data_size,
                                       frame->m_resources_buffer.m_upload_resource.Get(), src_offset,
                                       data_size);
        }

        void update(T *data, ComPtr<ID3D12GraphicsCommandList> cmd_list)
        {
            size_t data_size = m_num_elements * m_datum_size;
            gpu_interface::frame_resource *frame = m_gpu->get_frame_resource();
            UINT8 *dest = frame->m_resources_buffer.allocate(data_size, m_alignment);
            memcpy(dest, data, data_size);
            size_t src_offset = dest - frame->m_resources_buffer.m_begin;
            cmd_list->CopyBufferRegion(default_resource.Get(), 0,
                                       frame->m_resources_buffer.m_upload_resource.Get(), src_offset,
                                       data_size);
            return;
        }
        size_t m_datum_size;
        size_t m_num_elements;
        size_t m_alignment;
        size_t m_unaligned_size;
        ComPtr<ID3D12Resource> default_resource;
        D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_handle;
        D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu_handle;
        gpu_interface *m_gpu;
    };
    template <typename T>
    gpu_interface::structured_buffer<T> create_structured_buffer(T *data, size_t num_elements, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE, ComPtr<ID3D12Resource> counter = nullptr);

    struct gbuffer
    {
        DXGI_FORMAT format;
        ComPtr<ID3D12Resource> rt_default_resource;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
        D3D12_CPU_DESCRIPTOR_HANDLE srv_handle;
    };
    gpu_interface::gbuffer create_gbuffer(DXGI_FORMAT format, D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON);

    struct render_target
    {
        DXGI_FORMAT hdr_format;
        DXGI_FORMAT ldr_format;
        ComPtr<ID3D12Resource> rt_default_resource;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_hdr;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_backbuffer;
        D3D12_CPU_DESCRIPTOR_HANDLE srv_handle;
    };
    gpu_interface::render_target create_render_target(int index, DXGI_FORMAT format, D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON);
    void resize(int width, int height, render_target *render_targets, size_t num_render_targets);

    HANDLE cpu_wait_event;
    size_t minimum_fence;
    size_t completed_fence;
    void cpu_wait_for_fence(UINT64 fence_value);
    void flush_graphics_queue();
    void next_frame();

    // Timing.
    std::mutex mtx;
    gpu_timer m_gpu_timer;
    std::string get_full_event_name(std::string event_name);
    void timer_start(ComPtr<ID3D12GraphicsCommandList> cmd_list, std::string event_name);
    void timer_stop(ComPtr<ID3D12GraphicsCommandList> cmd_list, std::string event_name);
    double result_cpu(std::string event_name);
    double result_gpu(std::string event_name);

    struct timer_entry
    {
        std::vector<std::string> cpu_event_names;
        ID3D12GraphicsCommandList *cmd_list[NUM_BACK_BUFFERS];
    };
    std::unordered_map<std::string, timer_entry> timers;

    // Core device objects.
    ComPtr<IDXGIFactory6> m_dxgi_factory;
    ComPtr<IDXGIAdapter> m_adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12CommandQueue> graphics_cmd_queue;
    ComPtr<ID3D12CommandQueue> compute_cmd_queue;
    ComPtr<ID3D12Resource> depth_stencil_default_resource;
    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor_rect;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> dsv_heap;

    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event;

    // Texture helpers.
    D3D12_RESOURCE_ALLOCATION_INFO upload_dds(const std::wstring &file,
                                              ComPtr<ID3D12GraphicsCommandList> cmd_list,
                                              ID3D12Resource **texture_resource,
                                              bool generate_mips = false,
                                              ComPtr<ID3D12Heap> texture_heap = nullptr, size_t heap_offset = 0);

    // Resource uploaders.
    struct COMMON_API resource_uploader
    {
        UINT8 *allocate(UINT64 data_size, UINT64 alignment);
        ComPtr<ID3D12Resource> m_upload_resource;
        UINT8 *m_begin;
        UINT8 *m_end;
        UINT8 *m_current;
    };
    resource_uploader m_buffer_uploader;
    ComPtr<ID3D12Resource> m_texture_uploader;
    size_t m_tex_uploader_offset;
    void *m_tex_uploader_data;

    // Descriptor staging infrastructure
    void reset_staging_descriptors();
    void set_staging_heaps(ComPtr<ID3D12GraphicsCommandList> cmd_list);
    void create_null_descriptors();
    D3D12_CPU_DESCRIPTOR_HANDLE m_null_sampler;
    D3D12_CPU_DESCRIPTOR_HANDLE m_null_cbv;
    D3D12_CPU_DESCRIPTOR_HANDLE m_null_srv;
    D3D12_CPU_DESCRIPTOR_HANDLE m_null_uav;

    void create_descriptor_allocators();
    struct COMMON_API descriptor_allocator
    {
        size_t allocate();
        ComPtr<ID3D12DescriptorHeap> m_staging_heap;
        std::atomic<UINT32> descriptor_count;
        UINT max_descriptor_count;
        UINT descriptor_size;
    };
    descriptor_allocator rtv_allocator;     // Render target view allocator
    descriptor_allocator dsv_allocator;     // Depth-stencil view allocator
    descriptor_allocator csu_allocator;     // Cbv Srv Uav allocator
    descriptor_allocator sampler_allocator; // Sampler allocator

    ComPtr<ID3D12RootSignature> create_graphics_staging_rootsig(std::vector<D3D12_ROOT_PARAMETER1> additional_parameters = {}, UINT space = 0);
    ComPtr<ID3D12RootSignature> create_compute_staging_rootsig(std::vector<D3D12_ROOT_PARAMETER1> additional_parameters = {}, UINT space = 0);

    // Per-frame data
    UINT32 frame_index;
    UINT64 previous_frame_index;
    struct frame_resource
    {
        frame_resource() = default;
        ~frame_resource() = default;

        UINT64 fence_value;
        ComPtr<ID3D12CommandAllocator> cmd_alloc;
        ComPtr<ID3D12GraphicsCommandList> cmd_list;
        ComPtr<ID3D12Resource> back_buffer;

        // Per-frame resource allocator
        struct COMMON_API frame_resources_allocator
        {
            ~frame_resources_allocator() = default;
            frame_resources_allocator() = default;
            frame_resources_allocator(
                ComPtr<ID3D12Device> device,
                size_t size);
            UINT8 *allocate(size_t size, size_t alignment);
            ComPtr<ID3D12Resource> m_upload_resource;
            UINT8 *m_begin;
            UINT8 *m_current;
            UINT8 *m_end;
        };
        frame_resources_allocator m_resources_buffer;

        // Per-frame descriptor table allocator
        struct COMMON_API descriptor_table_frame_allocator
        {
            ~descriptor_table_frame_allocator() = default;
            descriptor_table_frame_allocator() = default;
            descriptor_table_frame_allocator(ComPtr<ID3D12Device> device,
                                             D3D12_DESCRIPTOR_HEAP_TYPE descriptor_type,
                                             UINT descriptor_count,
                                             UINT max_rename_count,
                                             UINT additional_descriptors_count = 0);
            ComPtr<ID3D12DescriptorHeap> m_heap_cpu;
            ComPtr<ID3D12DescriptorHeap> m_heap_gpu;
            D3D12_DESCRIPTOR_HEAP_TYPE m_descriptor_type;
            UINT m_descriptor_size;
            UINT m_descriptor_count;
            UINT m_ring_offset;
            bool m_is_stage_dirty[SHADERSTAGE_MAX];
            std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_bound_descriptors;

            // Reset.
            void reset_staging_heap(ComPtr<ID3D12Device> device,
                                    D3D12_CPU_DESCRIPTOR_HANDLE *null_descriptors_sampler_csu);

            // Update.
            void stage_to_cpu_heap(
                ComPtr<ID3D12Device> device,
                shader_stages stage,
                shader_descriptor_type type,
                UINT bind_slot,
                D3D12_CPU_DESCRIPTOR_HANDLE descriptor);

            // Validate.
            void set_tables(ComPtr<ID3D12Device> device, ComPtr<ID3D12GraphicsCommandList> cmd_list);
        };
        descriptor_table_frame_allocator csu_table_allocator;
        descriptor_table_frame_allocator sampler_table_allocator;
    };
    frame_resource frames[NUM_BACK_BUFFERS];
    frame_resource *get_frame_resource() { return &frames[frame_index]; };
};

template <typename T>
gpu_interface::scratch_buffer<T>
gpu_interface::create_scratch_buffer(T *data, size_t num_elements, bool is_constant_buffer)
{
    gpu_interface::scratch_buffer<T> sb = gpu_interface::scratch_buffer<T>();
    sb.m_gpu = this;
    sb.m_num_elements = num_elements;
    sb.m_datum_size = sizeof(T);
    sb.m_unaligned_size = sb.m_datum_size * sb.m_num_elements;

    if (is_constant_buffer)
    {
        sb.m_alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    }
    else
    {
        sb.m_alignment = D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT;
    }
    sb.m_aligned_size = align_up(sb.m_unaligned_size, sb.m_alignment);
    return sb;
}

template <typename T>
gpu_interface::upload_buffer<T>
gpu_interface::create_upload_buffer(T *data, size_t num_elements, bool is_constant_buffer)
{
    gpu_interface::upload_buffer<T> ub = gpu_interface::upload_buffer<T>();
    ub.m_gpu = this;
    ub.m_num_elements = num_elements;
    ub.m_datum_size = sizeof(T);
    ub.m_unaligned_size = ub.m_datum_size * ub.m_num_elements;

    if (is_constant_buffer)
    {
        ub.m_alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    }
    else
    {
        ub.m_alignment = D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT;
    }
    ub.m_aligned_size = align_up(ub.m_unaligned_size, ub.m_alignment);

    check_hr(device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
                                             D3D12_HEAP_FLAG_NONE,
                                             &CD3DX12_RESOURCE_DESC::Buffer(ub.m_aligned_size),
                                             D3D12_RESOURCE_STATE_GENERIC_READ,
                                             nullptr,
                                             IID_PPV_ARGS(ub.m_upload_resource.GetAddressOf())));

    check_hr(ub.m_upload_resource->Map(0, nullptr, (void **)&ub.m_mapped_data));
    return ub;
}

template <typename T>
gpu_interface::constant_buffer<T>
gpu_interface::create_constant_buffer(T *data, size_t num_elements, D3D12_RESOURCE_FLAGS flags)
{
    gpu_interface::constant_buffer<T> cb = constant_buffer<T>();
    cb.m_gpu = this;
    size_t data_size = sizeof(T) * num_elements;
    cb.data_size = data_size;
    cb.m_alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    size_t aligned_size = align_up(data_size, cb.m_alignment);

    default_resource_from_uploader(get_frame_resource()->cmd_list, cb.default_resource.GetAddressOf(),
                                   data, data_size, cb.m_alignment, flags);

    cb.cpu_handle.ptr = csu_allocator.allocate();

    D3D12_CONSTANT_BUFFER_VIEW_DESC object_cb_desc = {};
    object_cb_desc.BufferLocation = cb.default_resource->GetGPUVirtualAddress();
    object_cb_desc.SizeInBytes = (UINT)aligned_size;
    device->CreateConstantBufferView(&object_cb_desc, cb.cpu_handle);

    return cb;
}

template <typename T>
gpu_interface::structured_buffer<T>
gpu_interface::create_structured_buffer(T *data, size_t num_elements, D3D12_RESOURCE_FLAGS flags, ComPtr<ID3D12Resource> counter)
{
    gpu_interface::structured_buffer<T> db = structured_buffer<T>();
    db.m_gpu = this;
    db.m_datum_size = sizeof(T);
    db.m_num_elements = num_elements;
    db.m_alignment = sizeof(T);
    db.m_unaligned_size = num_elements * db.m_datum_size;

    default_resource_from_uploader(get_frame_resource()->cmd_list, db.default_resource.GetAddressOf(),
                                   data, db.m_unaligned_size, db.m_alignment, flags);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = DXGI_FORMAT_UNKNOWN;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv_desc.Buffer.FirstElement = 0;
    srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    srv_desc.Buffer.NumElements = (UINT)num_elements;
    srv_desc.Buffer.StructureByteStride = (UINT)db.m_datum_size;

    db.srv_cpu_handle.ptr = csu_allocator.allocate();
    device->CreateShaderResourceView(db.default_resource.Get(), &srv_desc, db.srv_cpu_handle);

    if (flags == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
        uav_desc.Format = DXGI_FORMAT_UNKNOWN;
        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav_desc.Buffer.CounterOffsetInBytes = 0;
        uav_desc.Buffer.FirstElement = 0;
        uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        uav_desc.Buffer.NumElements = (UINT)num_elements;
        uav_desc.Buffer.StructureByteStride = (UINT)db.m_datum_size;

        db.uav_cpu_handle.ptr = csu_allocator.allocate();
        device->CreateUnorderedAccessView(db.default_resource.Get(), counter.Get(), &uav_desc, db.uav_cpu_handle);
    }
    return db;
}
#pragma warning(pop)

#if defined(_DEBUG) || defined(DBG)
inline void set_name(ID3D12Object *object, const char *name)
{
    if (object != nullptr)
    {
        wchar_t obj_name[MAX_PATH] = {};
        size_t chars_converted = 0;
        mbstowcs_s(&chars_converted,
                   obj_name, MAX_PATH,
                   name, strlen(name));
        object->SetName(obj_name);
    }
}
inline void set_name(ComPtr<ID3D12Object> object, const char *name)
{
    if (object.Get() != nullptr)
    {
        wchar_t obj_name[MAX_PATH] = {};
        size_t chars_converted = 0;
        mbstowcs_s(&chars_converted,
                   obj_name, MAX_PATH,
                   name, strlen(name));
        object->SetName(obj_name);
    }
}
inline void set_name_indexed(ID3D12Object *object, const wchar_t *name, UINT index)
{
    WCHAR fullname[MAX_PATH];
    if (swprintf_s(fullname, L"%s[%u]", name, index) > 0 && object != nullptr)
    {
        object->SetName(fullname);
    }
}
inline void set_name_indexed(ComPtr<ID3D12Object> object, const wchar_t *name, UINT index)
{
    WCHAR fullname[MAX_PATH];
    if (swprintf_s(fullname, L"%s[%u]", name, index) > 0 && object.Get() != nullptr)
    {
        object->SetName(fullname);
    }
}

#else
inline void set_name(ID3D12Object *, const char *)
{
}
inline void set_name_indexed(ID3D12Object *, const wchar_t *, UINT)
{
}
inline void set_name(ComPtr<ID3D12Object>, const char *)
{
}
inline void set_name_indexed(ComPtr<ID3D12Object>, const wchar_t *, UINT)
{
}
#endif

#define NAME_D3D12_OBJECT(x) set_name((x), #x)
#define NAME_D3D12_OBJECT_INDEXED(x, n) set_name_indexed((x), L#x, n)
