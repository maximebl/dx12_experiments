#pragma once
#include "gpu_interface.h"
#include "directx12_include.h"
#include <vector>
#include "imgui.h"
#include "render_object.h"
#include "volume_light.h"
#include "camera.h"
#include "shader_data.h"
#include "shader_shared_constants.h"
#include <mutex>
#include "gpu_timer.h"

namespace particle
{
// Allow for 2 particles per cache line.
static constexpr int half_cache_line = DirectX::XM_CACHE_LINE_SIZE / 2;
struct alignas(half_cache_line) aligned_aos
{
    DirectX::XMFLOAT3 position; // 12 bytes
    float size;                 // 4 bytes -- 16 bytes alignment
    DirectX::XMFLOAT3 velocity; // 12 bytes
    float age;                  // 4 bytes -- 16 bytes alignment
};
static_assert(std::alignment_of<aligned_aos>::value == half_cache_line, "aligned_particle_aos must be 32 bytes aligned.");
static constexpr size_t byte_size = sizeof(aligned_aos);
static_assert(byte_size == half_cache_line, "aligned_particle_aos must be 32 bytes wide.");
} // namespace particle

enum PSOs
{
    pbr_simple_PSO,
    solid_color_PSO,
    debug_line_PSO,
    debug_plane_PSO,
    shadow_pass_PSO,
    cube_shadow_pass_PSO,
    lighting_pass_PSO,
    particle_sim_PSO,
    calculate_bounds_PSO,
    particle_point_draw_PSO,
    billboards_PSO,
    volume_light_additive_transparency_PSO,
    volume_light_alpha_transparency_PSO,
    draw_sky_PSO,
    draw_bounds_PSO,
    tonemapping_PSO,
    commands_culling_PSO,
    filter_diffuse_irradiance_map_PSO,
    filter_specular_irradiance_map_PSO,
    pre_integrate_specular_brdf_PSO,
    equirect_to_cube_PSO,
    generate_mipmap_PSO,
    PSOs_MAX
};

enum samplers
{
    linear_wrap,
    shadow_sampler,
    point_clamp,
    linear_clamp,
    samplers_MAX
};

enum camera_selection
{
    main_camera,
    debug_camera,
    cameras_MAX
};

// Diffuse lighting options.
enum indirect_diffuse_brdf
{
    lambertian_pdf_ibl,
    lambertian_constant
};

enum direct_diffuse_brdf
{
    lambertian,
};

// Specular lighting options.
enum indirect_specular_brdf
{
    ggx_pdf_ibl,
    simple_reflection
};

enum direct_specular_brdf
{
    cook_torrance,
    blinn_phong
};

// Descriptor heap offsets to descriptors.
enum heap_descriptor_offsets
{
    // Staging descriptors.
    staging_descriptors = 0,

    // Shadow map related descriptors.
    cbv_shadow_casters_transforms = staging_descriptors + gpu_interface::num_csu_staging_descriptors,

    // Particle related descriptors.
    srv_particle_initial = cbv_shadow_casters_transforms + num_shadow_casters,
    srv_particle_outputs = srv_particle_initial + num_particle_systems,
    uav_particle_inputs = srv_particle_outputs + num_particle_systems,
    uav_particle_outputs = uav_particle_inputs + num_particle_systems,

    // Particle light related descriptors.
    srv_particle_lights = uav_particle_outputs + num_particle_systems,
    uav_particle_lights = srv_particle_lights + 1,

    // Bounds related descriptors.
    srv_bounds_buffer = uav_particle_lights + 1,
    uav_bounds_buffer = srv_bounds_buffer + 1,

    // Simulation commands descriptors.
    uav_simulation_commands_buffer = uav_bounds_buffer + 1,

    // Draw commands descriptors.
    srv_draw_commands_buffer = uav_simulation_commands_buffer + 1,
    uav_draw_commands_buffer = srv_draw_commands_buffer + 1,

    descriptors_MAX = uav_draw_commands_buffer + 1
};

struct particles_graphics
{
    particles_graphics() = default;
    ~particles_graphics();

    // Descriptors.
    static const int shadow_maps_descriptors = num_shadow_casters;
    static const int particle_buffers_descriptors = num_particle_systems * 4; // 4: [srv_particle_initial, srv_particle_outputs, uav_particle_inputs, uav_particle_outputs].
    static const int particle_lights_buffers_descriptors = 2;                 // 2: [srv_particle_lights, uav_particle_lights].
    static const int bounds_buffer_descriptors = 2;                           // 2: [srv_bounds_buffer, uav_bounds_buffer].
    static const int simulation_commands_buffer_descriptors = 1;              // 1: [uav_simulation_commands_buffer].
    static const int draw_commands_buffer_descriptors = 2;                    // 2: [srv_draw_commands_buffer, uav_draw_commands_buffer].

    gpu_interface m_gpu;
    std::mutex mtx;
    float m_deltatime;

    // Buffer formats.
    static const DXGI_FORMAT hdr_buffer_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static const DXGI_FORMAT back_buffer_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    static const DXGI_FORMAT gbuffer0_format = DXGI_FORMAT_R11G11B10_FLOAT;
    static const DXGI_FORMAT gbuffer1_format = DXGI_FORMAT_R10G10B10A2_UNORM;
    static const DXGI_FORMAT gbuffer2_format = hdr_buffer_format;

    // Depth targets formats.
    static const DXGI_FORMAT depthstencil_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    static const DXGI_FORMAT depthtarget_tex_format = DXGI_FORMAT_R24G8_TYPELESS;
    static const DXGI_FORMAT depthtarget_srv_format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    static const DXGI_FORMAT shadow_texture_format_dsv = DXGI_FORMAT_D32_FLOAT;
    static const DXGI_FORMAT shadow_texture_format_srv = DXGI_FORMAT_R32_FLOAT;
    static const DXGI_FORMAT shadow_texture_format_alias = DXGI_FORMAT_R32_TYPELESS;

    void initialize();
    void resize(int width, int height);
    void render();
    void staging_pass(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                      camera *cam);
    void point_shadows_pass(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                            gpu_interface::frame_resource *frame);
    void draw_geometry_pass(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                            gpu_interface::frame_resource *frame,
                            camera *cam);
    void draw_lighting_pass(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                            gpu_interface::frame_resource *frame);
    void draw_sky(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                  gpu_interface::frame_resource *frame);
    void draw_volume_lights(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                            gpu_interface::constant_buffer<object_data_vs> vs_cb,
                            gpu_interface::constant_buffer<volume_light_data_ps> ps_cb,
                            const volume_light *volume_lights, size_t count,
                            const camera *current_cam);
    void draw_particle_systems(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                               gpu_interface::frame_resource *frame);
    void draw_bounding_boxes(ComPtr<ID3D12GraphicsCommandList> cmd_list);
    void draw_debug_objects(ComPtr<ID3D12GraphicsCommandList> cmd_list);
    void post_process(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                      gpu_interface::frame_resource *frame);

    void draw_render_objects(ComPtr<ID3D12GraphicsCommandList> cmd_list,
                             gpu_interface::constant_buffer<object_data_vs>* vs_cb,
                             gpu_interface::constant_buffer<render_object_data_ps>* ps_cb,
                             const render_object *render_objects, size_t count, DirectX::XMMATRIX view_proj, bool is_scene_pass);

    void create_particle_systems_data(ComPtr<ID3D12GraphicsCommandList> cmd_list);
    void create_particle_simulation_commands(ComPtr<ID3D12GraphicsCommandList> cmd_list);
    void create_particle_draw_commands(ComPtr<ID3D12GraphicsCommandList> cmd_list);
    void create_ibl_textures(ComPtr<ID3D12GraphicsCommandList> cmd_list);

    void create_bounds(ComPtr<ID3D12GraphicsCommandList> cmd_list);
    void create_bounds_calculations_commands(ComPtr<ID3D12GraphicsCommandList> cmd_list);
    void create_bounds_draw_commands(ComPtr<ID3D12GraphicsCommandList> cmd_list);

    void create_point_shadows_draw_commands(ComPtr<ID3D12GraphicsCommandList> cmd_list);

    void create_render_targets();
    void create_gbuffers();
    void create_depth_target();
    void create_PSOs();
    void create_samplers();
    void create_render_objects(ComPtr<ID3D12GraphicsCommandList> cmd_list);
    void create_buffers(ComPtr<ID3D12GraphicsCommandList> cmd_list);
    void create_camera_debug_vertices(ComPtr<ID3D12GraphicsCommandList> cmd_list);

    camera *get_camera() { return &m_cameras[selected_cam]; };
    void create_camera_debug_indices(ComPtr<ID3D12GraphicsCommandList> cmd_list);
    void update_current_camera();
    void update_camera_yaw_pitch(ImVec2 last_mouse_pos);

    ImGuiContext *m_ctx;

    camera m_cameras[cameras_MAX];
    camera_selection selected_cam;
    bool show_debug_camera;

    // Root signatures.
    ComPtr<ID3D12RootSignature> create_compute_rootsig();
    ComPtr<ID3D12RootSignature> create_graphics_rootsig();
    ComPtr<ID3D12RootSignature> m_compute_rootsig;
    ComPtr<ID3D12RootSignature> m_graphics_rootsig;

    ID3D12PipelineState *m_PSOs[PSOs_MAX];
    D3D12_CPU_DESCRIPTOR_HANDLE m_samplers[samplers_MAX];

    // Render objects.
    typedef std::unordered_map<std::string, render_object> render_object_map;
    render_object_map m_render_objects;
    volume_light m_volume_light;
    size_t num_total_submeshes;

    // Debug render objects.
    render_object m_debug_cam_frustum;
    render_object m_debug_cam_frustum_planes;

    // Debug camera frustum vertices.
    gpu_interface::scratch_buffer<camera::vertex_debug> m_debugcam_frustum_vertices;

    // Render pass constant data.
    gpu_interface::constant_buffer<pass_data> m_pass_cb;

    // Per object constant data.
    gpu_interface::constant_buffer<object_data_vs> m_object_cb_vs;
    gpu_interface::constant_buffer<object_data_vs> m_shadowmap_cb_vs;
    gpu_interface::constant_buffer<render_object_data_ps> m_render_object_cb_ps;

    // App state.
    float clip_delta;
    float exposure;
    int specular_shading;
    bool do_tonemapping;
    bool use_pcf_point_shadows;
    bool use_pcf_max_quality;
    bool is_drawing_bounds;
    bool draw_billboards;
    int brdf_id;
    indirect_diffuse_brdf m_indirect_diffuse_brdf;
    indirect_specular_brdf m_indirect_specular_brdf;
    direct_diffuse_brdf m_direct_diffuse_brdf;
    direct_specular_brdf m_direct_specular_brdf;

    point_light total_particle_lights[num_particle_lights];
    gpu_interface::structured_buffer<point_light> m_particle_lights_sb;
    ComPtr<ID3D12Resource> particle_lights_counter_default[gpu_interface::NUM_BACK_BUFFERS];
    gpu_interface::constant_buffer<UINT> m_particle_lights_counter;

    attractor_point_light attractors[num_particle_systems];
    gpu_interface::structured_buffer<attractor_point_light> m_attractors_sb;

    volume_light volume_lights[num_particle_systems];
    gpu_interface::constant_buffer<volume_light_data_ps> m_volume_light_cb_ps;

    spot_light spotlights[num_spotlights];
    gpu_interface::structured_buffer<spot_light> m_spotlights_sb;

    // Render targets.
    gpu_interface::render_target m_render_targets[gpu_interface::NUM_BACK_BUFFERS];

    // Depth target.
    ComPtr<ID3D12Resource> depthtarget_default;
    D3D12_CPU_DESCRIPTOR_HANDLE depthtarget_dsv_handle;
    D3D12_CPU_DESCRIPTOR_HANDLE depthtarget_srv_handle;

    // Gbuffers.
    gpu_interface::gbuffer m_gbuffer0;
    gpu_interface::gbuffer m_gbuffer1;
    gpu_interface::gbuffer m_gbuffer2;

    // Staging command lists.
    ComPtr<ID3D12GraphicsCommandList> staging_cmdlists[gpu_interface::NUM_BACK_BUFFERS];
    ComPtr<ID3D12CommandAllocator> staging_cmdallocs[gpu_interface::NUM_BACK_BUFFERS];

    // Shadow maps related data.
    void create_shadowmap_thread_contexts();
    void shadowmap_worker(int thread_index, spot_light *spotlights, int num_spotlights);
    void create_spotlight_shadowmaps();
    void create_pointlight_shadowmaps();
    D3D12_GPU_DESCRIPTOR_HANDLE shadowmaps_base_gpu[gpu_interface::NUM_BACK_BUFFERS];

    // Shadow casters transforms data.
    ComPtr<ID3D12Resource> m_shadowcasters_transforms;
    object_data_vs shadow_casters_transforms[num_shadow_casters];
    size_t shadow_transforms_cbv_size;
    D3D12_GPU_DESCRIPTOR_HANDLE cbv_gpu_shadowcasters_transforms_base[gpu_interface::NUM_BACK_BUFFERS];
    D3D12_CPU_DESCRIPTOR_HANDLE cbv_cpu_shadowcasters_transforms[gpu_interface::NUM_BACK_BUFFERS];

    spot_light per_thread_spotlights[G_NUM_SHADOW_THREADS][G_NUM_LIGHTS_PER_THREAD];
    spot_light *per_thread_sl[G_NUM_SHADOW_THREADS];

    HANDLE shadow_thread_handles[G_NUM_SHADOW_THREADS];
    HANDLE begin_shadowpass_events[G_NUM_SHADOW_THREADS];
    HANDLE end_shadowpass_events[G_NUM_SHADOW_THREADS];

    struct shadow_thunk_parameter
    {
        particles_graphics *graphics;
        int thread_index;
        spot_light *spotlights;
        int num_spotlights;
    };
    shadow_thunk_parameter shadow_thunk_parameters[G_NUM_SHADOW_THREADS];

    ComPtr<ID3D12GraphicsCommandList> shadow_cmdlists[gpu_interface::NUM_BACK_BUFFERS][G_NUM_SHADOW_THREADS];
    ComPtr<ID3D12CommandAllocator> shadow_cmdallocs[gpu_interface::NUM_BACK_BUFFERS][G_NUM_SHADOW_THREADS];

    // Particles related data.
    void create_indirect_commands(ComPtr<ID3D12GraphicsCommandList> cmd_list);
    particle::aligned_aos particles[num_particle_systems][num_particles_per_system];
    transform particle_system_transforms[num_particle_systems];
    particle_system_info particle_systems_infos[num_particle_systems];

    gpu_interface::constant_buffer<particle_system_info> m_particle_system_info;
    ComPtr<ID3D12Resource> particle_initial_default;
    ComPtr<ID3D12Resource> particle_input_default;
    ComPtr<ID3D12Resource> particle_output_default;
    ComPtr<ID3D12Resource> particle_infos_default;

    D3D12_GPU_DESCRIPTOR_HANDLE compute_descriptors_base_gpu_handle[gpu_interface::NUM_BACK_BUFFERS];
    D3D12_CPU_DESCRIPTOR_HANDLE particles_base_cpu[gpu_interface::NUM_BACK_BUFFERS];

    // Texture processing data.
    struct texture
    {
        D3D12_CPU_DESCRIPTOR_HANDLE srv_handle;
        D3D12_CPU_DESCRIPTOR_HANDLE uav_handle;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> mips_uav_handles;
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> mips_srv_handles;
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> array_dsv_handles;
        std::unordered_map<std::string, D3D12_CPU_DESCRIPTOR_HANDLE> additional_SRVs;
        std::unordered_map<std::string, D3D12_CPU_DESCRIPTOR_HANDLE> additional_UAVs;
        std::unordered_map<std::string, D3D12_CPU_DESCRIPTOR_HANDLE> additional_DSVs;
        ComPtr<ID3D12Resource> default_resource;
    };
    ComPtr<ID3D12Heap> m_sprite_textures_heap;
    texture m_fire_sprite;

    // Shadow maps.
    texture m_spotlight_shadowmaps;
    texture m_pointlight_shadowmaps;

    // IBL.
    texture m_equirect_tex;
    texture m_unfiltered_tex;
    texture m_diffuse_irradiance_tex;
    texture m_specular_irradiance_tex;
    texture m_specular_brdf_lut;

    // Bounds related data.
    struct bounding_box
    {
        DirectX::XMFLOAT3 extents;
        DirectX::XMFLOAT4 center;
        DirectX::XMFLOAT3 positions[8];
    };
    D3D12_VERTEX_BUFFER_VIEW bb_vbv;
    D3D12_INDEX_BUFFER_VIEW bb_ibv;
    size_t num_bb_indices;
    ComPtr<ID3D12Resource> bounds_indices_resource;
    ComPtr<ID3D12Resource> bounds_vertices_resource;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_srv_bounds_vertices_handle[gpu_interface::NUM_BACK_BUFFERS];
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav_bounds_vertices_handle[gpu_interface::NUM_BACK_BUFFERS];
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav_bounds_vertices_handle[gpu_interface::NUM_BACK_BUFFERS];

    // Compute queue related data.
    void create_compute_thread_contexts();
    void compute_worker(int thread_index);

    ComPtr<ID3D12Fence> compute_fence;

    ComPtr<ID3D12GraphicsCommandList> compute_cmdlists[gpu_interface::NUM_BACK_BUFFERS][G_NUM_COMPUTE_THREADS];
    ComPtr<ID3D12CommandAllocator> compute_cmdallocs[gpu_interface::NUM_BACK_BUFFERS][G_NUM_COMPUTE_THREADS];

    HANDLE compute_thread_handles[G_NUM_COMPUTE_THREADS];
    HANDLE begin_compute_events[G_NUM_COMPUTE_THREADS];
    HANDLE end_compute_events[G_NUM_COMPUTE_THREADS];

    struct compute_thunk_parameter
    {
        particles_graphics *graphics;
        int thread_index;
    };
    compute_thunk_parameter compute_thunk_parameters[G_NUM_COMPUTE_THREADS];

    // Indirect execution data.
    ComPtr<ID3D12Resource> reset_counter_default; // Zero'd counter used to reset other counters.
    ComPtr<ID3D12Resource> particle_simcmds_counter_default[gpu_interface::NUM_BACK_BUFFERS];
    ComPtr<ID3D12Resource> particle_drawcmds_counter_default[gpu_interface::NUM_BACK_BUFFERS];

    ComPtr<ID3D12CommandSignature> render_point_shadows_cmdsig;
    ComPtr<ID3D12Resource> render_point_shadows_cmds_default;
    size_t num_point_shadow_cmds;

    ComPtr<ID3D12CommandSignature> particle_sim_cmdsig;
    ComPtr<ID3D12Resource> particle_simcmds_default;
    ComPtr<ID3D12Resource> particle_simcmds_swap_default;
    ComPtr<ID3D12Resource> particle_simcmds_filtered_default;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_srv_particle_simcmds_handle[gpu_interface::NUM_BACK_BUFFERS];
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav_particle_simcmds_filtered_handle[gpu_interface::NUM_BACK_BUFFERS];

    ComPtr<ID3D12CommandSignature> particle_draw_cmdsig;
    ComPtr<ID3D12Resource> particle_drawcmds_default;
    ComPtr<ID3D12Resource> particle_drawcmds_filtered_default;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_srv_particle_drawcmds_filtered_handle[gpu_interface::NUM_BACK_BUFFERS];
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav_particle_drawcmds_filtered_handle[gpu_interface::NUM_BACK_BUFFERS];

    ComPtr<ID3D12Resource> bounds_calc_cmds_default;
    ComPtr<ID3D12CommandSignature> bounds_calc_cmdsig;

    ComPtr<ID3D12Resource> bounds_drawcmds_default;
    ComPtr<ID3D12CommandSignature> bounds_draw_cmdsig;
};
