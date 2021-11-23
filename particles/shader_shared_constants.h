static const int G_NUM_COMPUTE_THREADS = 1;

static const int num_cubes = 1;

static const int num_textures_per_particle_system = 2;

static const int num_particle_systems = 2;
static const int num_particles_per_system = 16;
static const int num_particles_total = num_particle_systems * num_particles_per_system;

static const int num_sim_threads = (num_particles_per_system % 32) + (32 * int(32 <= num_particles_per_system));
static const int num_sim_threadgroups = num_particles_per_system / num_sim_threads;

static const int shadowmap_width = 512;
static const int shadowmap_height = 512;

static const int num_particle_lights = num_particles_total;
static const int num_pointlight_cube_shadowmaps = num_particle_systems;
static const int num_spotlights = 4;

static const int G_NUM_SHADOW_THREADS = 4;
static const int G_NUM_LIGHTS_PER_THREAD = num_spotlights / G_NUM_SHADOW_THREADS;
static const int G_NUM_SHADOW_MAPS = G_NUM_SHADOW_THREADS * G_NUM_LIGHTS_PER_THREAD;

static const int num_shadow_casters = 4; // [Cube, Sphere, Lucy, Sponza scene].

static const int envmap_res = 1024;

#ifdef __cplusplus
static_assert(G_NUM_SHADOW_THREADS <= num_spotlights, "The number of shadow threads is less than or equal to the number of shadow casting lights.");
static_assert((envmap_res & (envmap_res - 1)) == 0, "envmap_res is a power of 2");
static_assert((num_particles_per_system & (num_particles_per_system - 1)) == 0, "num_particles_per_system is a power of 2");
static_assert(num_particles_per_system > 0, "num_particles_per_system is greater than zero");
#endif
