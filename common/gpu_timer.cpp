#include "gpu_timer.h"

gpu_timer::gpu_timer(ComPtr<ID3D12Device> device,
                     ComPtr<ID3D12CommandQueue> cmd_queue,
                     UINT *backbuffer_index,
                     UINT num_backbuffers,
                     UINT max_events_per_list, UINT max_cmdlists_per_frame, UINT max_samples_per_event)
    : m_samples_per_event(max_samples_per_event)
{
    UINT64 tmp_gpu_frequency;
    cmd_queue->GetTimestampFrequency(&tmp_gpu_frequency);
    m_gpu_frequency = (double)tmp_gpu_frequency;

    frames.resize(num_backbuffers);
    m_backbuffer_index = backbuffer_index;

    event_stride = max_samples_per_event;
    list_stride = event_stride * max_events_per_list;
    frame_stride = list_stride * max_cmdlists_per_frame;

    max_num_entries = m_samples_per_event * num_backbuffers * max_events_per_list * max_cmdlists_per_frame;

    D3D12_QUERY_HEAP_DESC query_heap_desc;
    query_heap_desc.Count = max_num_entries;
    query_heap_desc.NodeMask = 0;
    query_heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;

    check_hr(device->CreateQueryHeap(
        &query_heap_desc,
        IID_PPV_ARGS(&m_query_heap)));
    m_query_heap->SetName(L"m_query_heap");

    check_hr(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT64) * max_num_entries),
        D3D12_RESOURCE_STATE_COPY_DEST,
        NULL,
        IID_PPV_ARGS(m_query_rb_buffer.GetAddressOf())));
    m_query_rb_buffer->SetName(L"m_query_rb_buffer");
}

void gpu_timer::start(std::string event_name, ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    UINT frame_id = *m_backbuffer_index;
    frame &cur_frame = frames[frame_id];

    bool is_list_found = cur_frame.lists.find(cmd_list.Get()) != cur_frame.lists.end();
    if (!is_list_found)
    {
        // The command list was not found for this frame, add it.
        cur_frame.lists[cmd_list.Get()].id = (UINT)cur_frame.lists.size();
        cur_frame.list_to_events[cmd_list.Get()];
    }

    events_table &cur_list_events = cur_frame.list_to_events.at(cmd_list.Get());
    bool is_event_found = cur_list_events.find(event_name) != cur_list_events.end();
    if (!is_event_found)
    {
        // The event was not found for this command list, add it.
        cur_list_events[event_name] = (UINT)cur_list_events.size();
    }

    UINT list_id = cur_frame.lists.at(cmd_list.Get()).id;
    UINT event_id = cur_frame.list_to_events.at(cmd_list.Get()).at(event_name);
    UINT global_offset = calc_offset(frame_id, event_id, list_id);
    cmd_list->EndQuery(m_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, global_offset);

    cur_frame.lists.at(cmd_list.Get()).is_resolved = false;
}

void gpu_timer::stop(std::string event_name, ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    UINT frame_id = *m_backbuffer_index;
    frame &cur_frame = frames[frame_id];
    UINT list_id = cur_frame.lists.at(cmd_list.Get()).id;
    UINT event_id = cur_frame.list_to_events.at(cmd_list.Get()).at(event_name);
    UINT global_offset = calc_offset(frame_id, event_id, list_id) + 1;
    cmd_list->EndQuery(m_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, global_offset);
}

double gpu_timer::result(std::string event_name, ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    UINT frame_id = *m_backbuffer_index;
    frame &cur_frame = frames[frame_id];

    bool is_list_found = cur_frame.lists.find(cmd_list.Get()) != cur_frame.lists.end();
    if (!is_list_found)
    {
        // This list is not being tracked by the gpu timer. No calls to start() have been made yet.
        return 0.0;
    }

    events_table &list_events = cur_frame.list_to_events.at(cmd_list.Get());
    bool list_event_found = list_events.find(event_name) != list_events.end();
    if (!list_event_found)
    {
        // The event was not found for this command list.
        return 0.0;
    }

    UINT list_id = cur_frame.lists.at(cmd_list.Get()).id;
    UINT event_id = cur_frame.list_to_events.at(cmd_list.Get()).at(event_name);

    UINT global_offset = calc_offset(frame_id, event_id, list_id);
    UINT buffer_start = global_offset;
    UINT buffer_end = buffer_start + 1;

    D3D12_RANGE rb_range;
    rb_range.Begin = buffer_start * sizeof(UINT64);
    rb_range.End = buffer_end * sizeof(UINT64);
    m_query_rb_buffer->Map(0, &rb_range, (void **)&m_timestamp_buffer);

    UINT64 timestamp_tick_delta = m_timestamp_buffer[buffer_end] - m_timestamp_buffer[buffer_start];
    double frame_time = ((double)timestamp_tick_delta / m_gpu_frequency) * 1000.0; // convert from gpu ticks to milliseconds

    rb_range = {};
    m_query_rb_buffer->Unmap(0, &rb_range);
    m_timestamp_buffer = NULL;
    return frame_time;
}

void gpu_timer::resolve_frame()
{
    frame &frame = frames[*m_backbuffer_index];
    lists_table &lists = frame.lists;
    entries_table &entries = frame.list_to_events;

    for (auto &list : lists)
    {
        ID3D12GraphicsCommandList *cur_list = list.first;
        list_info &cur_list_info = list.second;

        if (!cur_list_info.is_resolved)
        {
            events_table &events = entries.at(cur_list);
            for (auto &event : events)
            {
                const std::string &cur_list_event_name = event.first;
                resolve(cur_list_event_name, cur_list);
            }
        }
    }
}

void gpu_timer::resolve(std::string event_name, ComPtr<ID3D12GraphicsCommandList> cmd_list)
{
    UINT frame_id = *m_backbuffer_index;
    frame &cur_frame = frames[frame_id];
    UINT list_id = cur_frame.lists.at(cmd_list.Get()).id;
    UINT event_id = cur_frame.list_to_events.at(cmd_list.Get()).at(event_name);

    UINT buffer_start = calc_offset(frame_id, event_id, list_id);
    UINT64 aligned_dest = buffer_start * sizeof(UINT64);

    cmd_list->ResolveQueryData(m_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, buffer_start, 2, m_query_rb_buffer.Get(), aligned_dest);

    cur_frame.lists.at(cmd_list.Get()).is_resolved = true;
}

UINT gpu_timer::calc_offset(local_id frame_id, local_id event_id, local_id list_id)
{
    UINT offset_to_frame = frame_stride * frame_id;
    UINT offset_to_event = event_stride * event_id;
    UINT offset_to_list = list_stride * list_id;
    return offset_to_frame + offset_to_event + offset_to_list;
}
