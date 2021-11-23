#pragma once
#include <unordered_map>

struct cpu_timer
{
    static constexpr double milliseconds = 1000.0;

    struct measurement
    {
        union
        {
            double start_time;
            UINT64 start_cycle;
        };
        union
        {
            double end_time;
            UINT64 end_cycle;
        };
    };

    struct sample
    {
        measurement cpu_time;
        measurement clock_cycles;
    };

    double cpu_frequency = 0.0;
    UINT frame_count = 0;
    UINT64 total_frame_count = 0;
    UINT fps = 0;
    double total_time = 0.0;
    double frame_time_ms = 0.0;
    double base_time = 0.0;
    UINT64 cycles_per_frame = 0;

    sample frame;
    std::unordered_map<std::string, sample> timers = {};

    cpu_timer()
    {
        LARGE_INTEGER tmp_cpu_frequency;
        QueryPerformanceFrequency(&tmp_cpu_frequency);
        cpu_frequency = (double)tmp_cpu_frequency.QuadPart;

        base_time = get_timestamp();
        frame.cpu_time.start_time = base_time;
        frame.clock_cycles.start_cycle = __rdtsc();
    }

    double get_timestamp()
    {
        LARGE_INTEGER current_time = {};
        QueryPerformanceCounter(&current_time);
        return (double)current_time.QuadPart;
    }

    double get_current_time()
    {
        return (get_timestamp() - base_time) / cpu_frequency;
    }

    void start(std::string name)
    {
        measurement cpu_time = {};
        cpu_time.start_time = get_timestamp();
        timers[name].cpu_time = cpu_time;

        measurement clock_cycles = {};
        clock_cycles.start_cycle = __rdtsc();
        timers[name].clock_cycles = clock_cycles;
    }

    void stop(std::string name)
    {
        timers[name].cpu_time.end_time = get_timestamp();
        timers[name].clock_cycles.end_cycle = __rdtsc();
    }

    double result_ms(std::string name)
    {
        measurement cpu_time = timers[name].cpu_time;
        double delta = cpu_time.end_time - cpu_time.start_time;
        return (delta / cpu_frequency) * milliseconds;
    }

    UINT64 result_cycles(std::string name)
    {
        measurement cpu_cycles = timers[name].clock_cycles;
        return cpu_cycles.end_cycle - cpu_cycles.start_cycle;
    }

    double tick()
    {
        // Cycles.
        frame.clock_cycles.end_cycle = __rdtsc();
        cycles_per_frame = frame.clock_cycles.end_cycle - frame.clock_cycles.start_cycle;
        frame.clock_cycles.start_cycle = frame.clock_cycles.end_cycle;

        // CPU time.
        frame.cpu_time.end_time = get_timestamp();
        double time_delta = frame.cpu_time.end_time - frame.cpu_time.start_time;
        frame.cpu_time.start_time = frame.cpu_time.end_time;

        double frame_time_s = time_delta / cpu_frequency;
        frame_time_ms = frame_time_s * milliseconds;

        total_time += time_delta;
        total_frame_count += frame_count++;

        if (total_time > cpu_frequency) // One second has elapsed.
        {
            fps = frame_count;

            frame_count = 0;
            total_time = 0;
        }
        return frame_time_s;
    }
};
