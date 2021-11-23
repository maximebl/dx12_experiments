#include "common.hlsl"

ConstantBuffer<pass_data> cb_pass : register(b0);

// Bounds of each particle system.
StructuredBuffer<bounding_box> bounds : register(t0, space2);

// Particle simulation commands.
struct particle_simulation_command
{
    GPU_VIRTUAL_ADDRESS cbv_particle_system_info;
    uint input_buffer_index;
    uint output_buffer_index;
    D3D12_DISPATCH_ARGUMENTS dispatch_args;
};
StructuredBuffer<particle_simulation_command> in_sim_cmds : register(t0, space3);
AppendStructuredBuffer<particle_simulation_command> out_sim_cmds : register(u0, space3);

// Particle draw commands.
struct particle_draw_command
{
    VERTEX_BUFFER_VIEW vbv;
    GPU_VIRTUAL_ADDRESS cbv_particle_system_info;
    D3D12_DRAW_ARGUMENTS draw_args;
};
StructuredBuffer<particle_draw_command> in_draw_cmds : register(t0, space4);
AppendStructuredBuffer<particle_draw_command> out_draw_cmds : register(u0, space4);

bool is_aabb_visible(float4 planes[6], float4 center, float4 extents)
{
    for (int i = 0; i < 6; i++)
    {
        float4 plane = planes[i];
        plane = mul(plane, transpose(cb_pass.inv_view));

        float r = abs(plane.x * extents.x) + abs(plane.y * extents.y) + abs(plane.z * extents.z);
        float c = dot(plane.xyz, center.xyz) + plane.w;
        if (c <= -r)
        {
            return false;
        }
    }
    return true;
}

[numthreads(num_particle_systems, 1, 1)]
void cs_main(uint3 thread_id : SV_DispatchThreadID)
{
    uint index = thread_id.x;
    // filter commands
    bounding_box current_bounds = bounds[index];
    float4 center = current_bounds.center;
    float4 extents = float4(current_bounds.extents, 0.f);

    if (is_aabb_visible(cb_pass.frustum_planes, center, extents))
    {
        out_sim_cmds.Append(in_sim_cmds[index]);
        out_draw_cmds.Append(in_draw_cmds[index]);
    }
}