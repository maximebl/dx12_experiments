#include "gpu_query.h"
#include "d3dx12.h"

#define NUM_SAMPLES 2
#define NUM_BACK_BUFFERS 3

gpu_query::gpu_query(ComPtr<ID3D12Device> device, ComPtr<ID3D12GraphicsCommandList> cmd_list, ComPtr<ID3D12CommandQueue> cmd_queue, UINT *backbuffer_index, UINT num_queries)
    : m_cmd_list(cmd_list), m_backbuffer_index(backbuffer_index)
{

    UINT64 tmp_gpu_frequency;
    cmd_queue->GetTimestampFrequency(&tmp_gpu_frequency);
    m_gpu_frequency = (double)tmp_gpu_frequency;

    m_queries_stride = NUM_BACK_BUFFERS * NUM_SAMPLES;
    m_timer_count = m_queries_stride * num_queries;

    D3D12_QUERY_HEAP_DESC query_heap_desc;
    query_heap_desc.Count = m_timer_count;
    query_heap_desc.NodeMask = 0;
    query_heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;

    check_hr(device->CreateQueryHeap(
        &query_heap_desc,
        IID_PPV_ARGS(&m_query_heap)));

    check_hr(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT64) * m_timer_count),
        D3D12_RESOURCE_STATE_COPY_DEST,
        NULL,
        IID_PPV_ARGS(m_query_rb_buffer.GetAddressOf())));
}

void gpu_query::start(std::string query_name)
{
    if (m_queries.find(query_name) == m_queries.end())
        m_queries[query_name].index = m_num_queries++;

    UINT query_index = m_queries[query_name].index;
    UINT buffer_start = ((*m_backbuffer_index) * NUM_SAMPLES) + (query_index * m_queries_stride);

    m_queries[query_name].buffer_start = buffer_start;
    m_cmd_list->EndQuery(m_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, buffer_start);
}

void gpu_query::stop(std::string query_name)
{
    if (m_queries.empty())
        return;

    UINT buffer_start = m_queries[query_name].buffer_start;
    UINT buffer_end = buffer_start + 1;
    m_queries[query_name].buffer_end = buffer_end;
    m_cmd_list->EndQuery(m_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, buffer_end);
}

double gpu_query::result(std::string query_name)
{
    if (m_queries.empty())
        return 0.0;

    UINT buffer_start = m_queries[query_name].buffer_start;
    UINT buffer_end = m_queries[query_name].buffer_end;

    D3D12_RANGE rb_range;
    rb_range.Begin = buffer_start * sizeof(UINT64);
    rb_range.End = buffer_end * sizeof(UINT64);
    m_query_rb_buffer->Map(0, &rb_range, (void **)&m_timestamp_buffer);

    UINT64 time_delta = m_timestamp_buffer[buffer_end] - m_timestamp_buffer[buffer_start];
    double frame_time = ((double)time_delta / m_gpu_frequency) * 1000.0; // convert from gpu ticks to milliseconds

    rb_range = {};
    m_query_rb_buffer->Unmap(0, &rb_range);
    m_timestamp_buffer = NULL;
    return frame_time;
}

void gpu_query::resolve()
{
    if (m_queries.empty())
        return;

    m_cmd_list->ResolveQueryData(m_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, m_timer_count, m_query_rb_buffer.Get(), 0);
}
