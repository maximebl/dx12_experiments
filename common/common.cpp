#include "common.h"
#include <sstream>
#include <system_error>

std::wstring g_current_dll_name = L"";
HWND g_hwnd = NULL;
UINT64 g_hwnd_width = 0;
UINT g_hwnd_height = 0;
float g_aspect_ratio = 0.f;
cpu_timer g_cpu_timer = {};

std::string hr_msg(HRESULT hr)
{
    return std::system_category().message(hr);
}

inline void check_hr(HRESULT hr)
{
    ASSERT(SUCCEEDED(hr), hr_msg(hr));
}

void wait_duration(DWORD duration)
{
    HANDLE event_handle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
    WaitForSingleObject(event_handle, duration);
    CloseHandle(event_handle);
}

void failed_assert(const char *file, int line, std::string statement, std::string message)
{
    static bool debug = true;

    if (debug)
    {
        std::stringstream stream;
        stream << "Failed statement: " << statement << std::endl
               << "File: " << file << std::endl
               << "Line: " << line << std::endl
               << "Message: " << message;

        if (IsDebuggerPresent())
        {
            int res = MessageBoxA(NULL, stream.str().c_str(), "Assertion failed", MB_YESNOCANCEL | MB_ICONERROR);
            if (res == IDYES)
            {
                __debugbreak();
            }
            else if (res == IDCANCEL)
            {
                debug = false;
            }
        }
    }
}

std::string remove_extension(const std::string &filePath)
{
    return filePath.substr(0, filePath.rfind("."));
}

std::wstring remove_extension(const std::wstring &filePath)
{
    return filePath.substr(0, filePath.rfind(L"."));
}

uint32_t int_log2(uint32_t v)
{
    uint32_t r = 0;
    while (v >>= 1)
    {
        r++;
    }
    return r;
}

bool is_power_of2(uint64_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

uint64_t num_mipmap_levels(uint64_t width, uint64_t height)
{
    uint64_t levels = 1;
    while ((width | height) >> levels)
    {
        ++levels;
    }
    return levels;
}
