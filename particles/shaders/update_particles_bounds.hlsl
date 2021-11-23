#include "common.hlsl"

// Particles.
ConstantBuffer<particle_system_info> particle_system_cb : register(b1);
ConstantBuffer<particle_buffer_info> particle_buffer_indices : register(b2);
StructuredBuffer<particle> output_particles[num_particle_systems] : register(t0, space0);

// Bounds.
RWStructuredBuffer<bounding_box> bounds : register(u0, space2);

groupshared float3 max_positions[num_particles_per_system];
groupshared float3 min_positions[num_particles_per_system];

[numthreads(num_particles_per_system, 1, 1)]
void cs_main(uint3 thread_id : SV_DispatchThreadID, uint3 group_thread_id : SV_GroupThreadID)
{
    uint particle_index = thread_id.x;
    uint group_tid = group_thread_id.x;

    // Calculate particle bounds.
    max_positions[group_tid] = output_particles[particle_system_cb.particle_system_index][group_tid].position;
    min_positions[group_tid] = output_particles[particle_system_cb.particle_system_index][group_tid].position;
    GroupMemoryBarrierWithGroupSync();

    int step_size = 1;
    uint num_operating_threads = num_particles_per_system;

    while (num_operating_threads > 0)
    {
        if (group_tid < num_operating_threads)
        {
            int fst = group_tid * step_size * 2;
            int snd = fst + step_size;

            if (fst < num_particles_per_system && snd < num_particles_per_system)
            {
                max_positions[fst] = max(max_positions[fst], max_positions[snd]);
                min_positions[fst] = min(min_positions[fst], min_positions[snd]);
            }
        }

        step_size <<= 1; // Double the step size.
        num_operating_threads >>= 1; // Halve the amount of operating threads.
    }

    GroupMemoryBarrierWithGroupSync();

    if (group_tid == 0)
    {
        // The result of the reduction is stored at index 0.
        float3 v_max = max_positions[0];
        float3 v_min = min_positions[0];
        bounding_box new_bounds;

        float3 center = (v_max + v_min) * 0.5f;
        float3 extents = v_max - center;

        new_bounds.center = mul(float4(center, 1.f), particle_system_cb.world);
        new_bounds.extents = extents;

        new_bounds.position[0] = v_min;
        new_bounds.position[1] = float3(v_min.x, v_max.y, v_min.z);
        new_bounds.position[2] = float3(v_min.x, v_min.y, v_max.z);
        new_bounds.position[3] = float3(v_min.x, v_max.y, v_max.z);
        new_bounds.position[4] = float3(v_max.x, v_min.y, v_min.z);
        new_bounds.position[5] = float3(v_max.x, v_max.y, v_min.z);
        new_bounds.position[6] = float3(v_max.x, v_min.y, v_max.z);
        new_bounds.position[7] = v_max;

        bounds[particle_system_cb.particle_system_index] = new_bounds;
    }
    return;
}