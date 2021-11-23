#pragma once
#include "common.h"
#include "directx12_include.h"
#include <unordered_map>
#include <vector>
#include <string>

#pragma warning(push)
#pragma warning(disable : 4251) // Safe to ignore because the users of this DLL will always be compiled together with the DLL
class COMMON_API gpu_query
{
public:
    gpu_query() = default;
    ~gpu_query() = default;
    gpu_query(ComPtr<ID3D12Device> device,
              ComPtr<ID3D12GraphicsCommandList> cmd_list,
              ComPtr<ID3D12CommandQueue> cmd_queue,
              UINT *backbuffer_index, UINT num_queries);

    void start(std::string query_name);
    void stop(std::string query_name);
    void resolve();
    double result(std::string query_name);

private:
    ComPtr<ID3D12GraphicsCommandList> m_cmd_list;
    ComPtr<ID3D12QueryHeap> m_query_heap;
    ComPtr<ID3D12Resource> m_query_rb_buffer;
    UINT m_timer_count;
    UINT64 *m_timestamp_buffer;
    double m_gpu_frequency;
    UINT *m_backbuffer_index;

    UINT m_num_queries = 0;
    UINT m_queries_stride;
    struct query_info
    {
        UINT buffer_start;
        UINT buffer_end;
        UINT index;
    };
    std::unordered_map<std::string, query_info> m_queries;
};
#pragma warning(pop)
