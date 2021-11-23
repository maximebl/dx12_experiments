#pragma once
#define WIN32_LEAN_AND_MEAN
#include "Windows.h"
#include "stdint.h"
#include <string>
#include "step_timer.h"

#ifdef COMMON_EXPORTS
#define COMMON_API __declspec(dllexport)
#else
#define COMMON_API __declspec(dllimport)
#endif

#define Kilobytes(Value) ((Value)*1024LL)
#define Megabytes(Value) (Kilobytes(Value) * 1024LL)
#define Gigabytes(Value) (Megabytes(Value) * 1024LL)
#define Terabytes(Value) (Gigabytes(Value) * 1024LL)

COMMON_API void failed_assert(const char *file, int line, std::string statement, std::string message);
#define ASSERT(statement, message) \
    if (!(statement))              \
    failed_assert(__FILE__, __LINE__, #statement, message)

std::string hr_msg(HRESULT hr);
COMMON_API void check_hr(HRESULT hr);

#define safe_release(a) \
    if ((a) != nullptr) \
    {                   \
        (a)->Release(); \
        (a) = nullptr;  \
    }

inline size_t align_up(size_t value, size_t alignment)
{
    return ((value + (alignment - 1)) & ~(alignment - 1));
}

COMMON_API void wait_duration(DWORD duration);

COMMON_API std::string remove_extension(const std::string &filePath);
COMMON_API std::wstring remove_extension(const std::wstring &filePath);

COMMON_API uint32_t int_log2(uint32_t v);

COMMON_API uint64_t num_mipmap_levels(uint64_t width, uint64_t height);
COMMON_API bool is_power_of2(uint64_t value);

extern COMMON_API std::wstring g_current_dll_name;
extern COMMON_API HWND g_hwnd;
extern COMMON_API UINT64 g_hwnd_width;
extern COMMON_API UINT g_hwnd_height;
extern COMMON_API float g_aspect_ratio;
extern COMMON_API cpu_timer g_cpu_timer;

struct game_memory
{
    size_t page_size;
    size_t num_pages;
    size_t alloc_granularity;
    size_t total_size;
    void *min_address;
    void *memory;
};

struct memory_arena
{
    size_t size;
    size_t used;
    uint8_t *base;
};

inline void init_arena(memory_arena *arena, size_t size, void *base)
{
    arena->size = size;
    arena->base = (uint8_t *)base;
    arena->used = 0;
}

template <typename T>
inline T *push_struct(memory_arena *arena)
{
    size_t size = sizeof(T);
    ASSERT((arena->used + size) <= arena->size, "New allocation does not fit in this arena.");
    void *base_of_new_alloc = arena->base + arena->used;
    arena->used += size;
    base_of_new_alloc = new (base_of_new_alloc) T(); // Call the constructors
    return (T *)base_of_new_alloc;
}
