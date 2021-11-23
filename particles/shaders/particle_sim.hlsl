#include "common.hlsl"
#include "noise.hlsl"

ConstantBuffer<pass_data> cb_pass : register(b0);

// Particles.
ConstantBuffer<particle_system_info> particle_system_cb : register(b1);
ConstantBuffer<particle_buffer_info> particle_buffer_indices : register(b2);
StructuredBuffer<particle> initial_particles[num_particle_systems] : register(t0, space5);
RWStructuredBuffer<particle> particle_buffers[num_particle_systems * 2] : register(u0, space0);

// Particle lights.
AppendStructuredBuffer<point_light> particle_lights : register(u0, space1);

[numthreads(num_sim_threads, 1, 1)]
void cs_main(uint3 thread_id : SV_DispatchThreadID)
{
    int particle_index = thread_id.x;

    particle pi = initial_particles[particle_system_cb.particle_system_index][particle_index];
    particle p = particle_buffers[particle_buffer_indices.input_buffer_index][particle_index];

    float dt = cb_pass.delta_time;

    float amplitude = particle_system_cb.amplitude;
    float frequency = particle_system_cb.frequency;
    float speed = particle_system_cb.speed;

    float distortion = snoise(float4(pi.position.x * frequency,
                                     pi.position.y * frequency,
                                     pi.position.z * frequency,
                                     cb_pass.time * speed)) * amplitude;
    p.position = distortion * normalize(pi.position);

    // Output the particle with the simulation integrated to it.
    particle_buffers[particle_buffer_indices.output_buffer_index][particle_index] = p;

    if (particle_system_cb.particle_lights_enabled)
    {
        point_light light;
        light = particle_system_cb.light;
        light.position_ws = mul(float4(p.position, 1.f), particle_system_cb.world).xyz;
        particle_lights.Append(light);
    }
}