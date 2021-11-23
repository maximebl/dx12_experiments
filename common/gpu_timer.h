#pragma once
#include "common.h"
#include "directx12_include.h"
#include <unordered_map>
#include <vector>
#include <string>

#pragma warning(push)
#pragma warning(disable : 4251) // Safe to ignore because the users of this DLL will always be compiled together with the DLL
class COMMON_API gpu_timer
{
public:
    gpu_timer() = default;
    ~gpu_timer() = default;
    gpu_timer(ComPtr<ID3D12Device> device,
              ComPtr<ID3D12CommandQueue> cmd_queue,
              UINT *backbuffer_index,
              UINT num_backbuffers,
              UINT max_events_per_list = 100, UINT max_cmdlists_per_frame = 100, UINT max_samples_per_event = 2);

    void start(std::string event_name, ComPtr<ID3D12GraphicsCommandList> cmd_list);
    void stop(std::string event_name, ComPtr<ID3D12GraphicsCommandList> cmd_list);
    double result(std::string event_name, ComPtr<ID3D12GraphicsCommandList> cmd_list);
    void resolve_frame();
    void resolve(std::string event_name, ComPtr<ID3D12GraphicsCommandList> cmd_list);

private:
    ComPtr<ID3D12QueryHeap> m_query_heap;
    ComPtr<ID3D12Resource> m_query_rb_buffer;

    UINT m_samples_per_event;
    UINT max_num_entries;

    UINT event_stride;
    UINT frame_stride;
    UINT list_stride;

    UINT64 *m_timestamp_buffer;
    double m_gpu_frequency;
    UINT *m_backbuffer_index;

    using local_id = UINT;
    struct list_info
    {
        local_id id;
        bool is_resolved;
    };
    using events_table = std::unordered_map<std::string, local_id>;
    using lists_table = std::unordered_map<ID3D12GraphicsCommandList *, list_info>;
    //using lists_table = std::unordered_map<ID3D12GraphicsCommandList *, local_id>;
    using entries_table = std::unordered_map<ID3D12GraphicsCommandList *, events_table>;

    struct frame
    {
        lists_table lists;
        entries_table list_to_events;
    };
    std::vector<frame> frames;
    UINT calc_offset(local_id frame_id, local_id event_id, local_id list_id);
};
#pragma warning(pop)
