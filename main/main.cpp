#include "common.h"
#include "tchar.h"
#include "PathCch.h"

#include <Psapi.h>

typedef bool (*gamecode_initialize)(game_memory);
typedef void (*gamecode_resize)(int, int);
typedef void (*gamecode_on_mousemove)();
typedef void (*gamecode_wndproc)(UINT, WPARAM, LPARAM);
typedef bool (*gamecode_update_and_render)();
typedef void (*gamecode_cleanup)();
struct game_code
{
    HMODULE game_dll;
    gamecode_resize resize;
    gamecode_on_mousemove on_mousemove;
    gamecode_wndproc wndproc;
    gamecode_initialize initialize;
    gamecode_update_and_render update_and_render;
    gamecode_cleanup cleanup;
    FILETIME last_dll_write;
    FILETIME source_dll_write;
};

static game_memory game_mem;

static bool game_is_ready = false;
static game_code gamecode;
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static HMODULE win32code = NULL;

static void set_dll_paths(const wchar_t *path);
static bool load_gamecode();
static FILETIME file_last_write(const wchar_t *filename);
static bool hotreload();
static void update_window_titlebar();
static wchar_t window_text[MAX_PATH];
static wchar_t gamecodedll_path[MAX_PATH];
static wchar_t tempgamecodedll_path[MAX_PATH];
static wchar_t win32_exe_location[MAX_PATH];
static wchar_t gamecodedll_name[MAX_PATH];
static wchar_t temp_gamecodedll_name[MAX_PATH];

#include <vector>

struct app_memory
{
    size_t alloc_granularity;
    size_t total_size;
    void *min_address;
    void *memory;
};
app_memory app_mem;
memory_arena app_arena;

struct module_utils
{
    int val = 7;
    std::vector<int> ivec;
};

struct modulez
{
    module_utils utils;
};

static const size_t alloc = 268435456;
struct my_app
{
    modulez mod;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    set_dll_paths(L"particles.dll");
    if (!load_gamecode())
        return 1;

    WNDCLASSEX wc = {sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, win32code, NULL, NULL, NULL, NULL, _T("particles"), NULL};
    RegisterClassEx(&wc);
    g_hwnd = CreateWindow(wc.lpszClassName, _T("particles"), WS_OVERLAPPEDWINDOW, 100, 100, 1024, 1024, NULL, NULL, wc.hInstance, NULL);

    ShowWindow(g_hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(g_hwnd);
    GetWindowTextW(g_hwnd, window_text, 100);

    RECT rect;
    if (GetClientRect(g_hwnd, &rect))
    {
        g_hwnd_width = rect.right - rect.left;
        g_hwnd_height = rect.bottom - rect.top;
        g_aspect_ratio = (float)g_hwnd_width / g_hwnd_height;
    }

    // Allocate all memory for the app up front.
    SYSTEM_INFO sys_info = {};
    GetSystemInfo(&sys_info);
    game_mem.alloc_granularity = sys_info.dwAllocationGranularity; // Granularity for the starting address at which virtual memory can be reserved. Usually 65536.
    game_mem.page_size = sys_info.dwPageSize;                      // Size of the page file. Usually 4096.
    game_mem.min_address = sys_info.lpMinimumApplicationAddress;   // Lowest memory address in the process address space accessible to applications. Usually 0x0000000000010000.

    game_mem.total_size = align_up(Megabytes(1024), game_mem.alloc_granularity);
    game_mem.num_pages = game_mem.total_size / game_mem.page_size;

    game_mem.memory = VirtualAlloc(game_mem.min_address,
                                   game_mem.total_size,
                                   MEM_RESERVE | MEM_COMMIT,
                                   PAGE_READWRITE);

    // Initialize the app.
    if (!gamecode.initialize(game_mem))
    {
        gamecode.cleanup();
        return 1;
    }

    game_is_ready = true;

    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        if (hotreload())
        {
            update_window_titlebar();

            if (!load_gamecode())
            {
                wchar_t err_msg[MAX_PATH];
                wcscpy(err_msg, L"Game code dll error.");
                wcscat(err_msg, L"\n\n");

                wcscat(err_msg, L"The game code dll could not be loaded or did not export an implementation for necessary functions that the application needs.");

                if (MessageBoxW(NULL, err_msg,
                                L"Could not load game code.",
                                MB_OK | MB_ICONERROR | MB_DEFBUTTON2) != IDYES)
                {
                    break;
                }
            }

            if (!gamecode.initialize(game_mem))
            {
                wchar_t err_msg[MAX_PATH];
                wcscpy(err_msg, L"Game initialization error.");
                wcscat(err_msg, L"\n\n");

                wcscat(err_msg, L"The game failed to initialize.");
                if (MessageBoxW(NULL, err_msg,
                                L"Could initialize the game.",
                                MB_OK | MB_ICONERROR | MB_DEFBUTTON2) != IDYES)
                {
                    break;
                }
            }
            game_is_ready = true;
        }

        game_is_ready = gamecode.update_and_render();
    }

    if (gamecode.cleanup)
    {
        gamecode.cleanup();
    }
    if (gamecode.game_dll)
    {
        FreeLibrary(gamecode.game_dll);
    }
    DestroyWindow(g_hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    return 0;
}

void set_dll_paths(const wchar_t *path)
{
    g_current_dll_name = path;

    DWORD l_win32exe = GetModuleFileNameW(NULL, win32_exe_location, MAX_PATH);
    PathCchRemoveFileSpec(win32_exe_location, l_win32exe);
    wcscpy(gamecodedll_path, win32_exe_location);
    wcscpy(tempgamecodedll_path, win32_exe_location);

    wchar_t dll_name_with_slashes[MAX_PATH];
    wcscpy(dll_name_with_slashes, L"\\");
    wcscat(dll_name_with_slashes, path);
    wcscat(gamecodedll_path, dll_name_with_slashes);

    wchar_t temp_prefix[MAX_PATH];
    wcscpy(temp_prefix, L"\\temp_");
    wcscat(temp_prefix, path);
    wcscat(tempgamecodedll_path, temp_prefix);
}

FILETIME file_last_write(const wchar_t *filename)
{
    FILETIME last_write_time = {0};
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExW(filename, GetFileExInfoStandard, &data))
    {
        last_write_time = data.ftLastWriteTime;
    }
    return last_write_time;
}

bool load_gamecode()
{
    CopyFileW(gamecodedll_path, tempgamecodedll_path, 0);

    gamecode.game_dll = LoadLibraryW(tempgamecodedll_path);
    gamecode.last_dll_write = file_last_write(gamecodedll_path);

    if (!gamecode.game_dll)
        return false;

    gamecode.resize = (gamecode_resize)GetProcAddress(gamecode.game_dll, "resize");
    gamecode.on_mousemove = (gamecode_on_mousemove)GetProcAddress(gamecode.game_dll, "on_mousemove");
    gamecode.wndproc = (gamecode_wndproc)GetProcAddress(gamecode.game_dll, "wndproc");
    gamecode.initialize = (gamecode_initialize)GetProcAddress(gamecode.game_dll, "initialize");
    gamecode.update_and_render = (gamecode_update_and_render)GetProcAddress(gamecode.game_dll, "update_and_render");
    gamecode.cleanup = (gamecode_cleanup)GetProcAddress(gamecode.game_dll, "cleanup");

    if (!gamecode.resize || !gamecode.on_mousemove || !gamecode.wndproc || !gamecode.initialize || !gamecode.update_and_render || !gamecode.cleanup)
        return false;

    return true;
}

bool hotreload()
{
    gamecode.source_dll_write = file_last_write(gamecodedll_path);

    if (CompareFileTime(&gamecode.last_dll_write, &gamecode.source_dll_write) != 0)
    {
        game_is_ready = false;
        if (gamecode.game_dll)
        {
            gamecode.cleanup();
            FreeLibrary(gamecode.game_dll);
            gamecode.game_dll = NULL;
            gamecode.cleanup = NULL;
            gamecode.initialize = NULL;
            gamecode.resize = NULL;
            gamecode.on_mousemove = NULL;
            gamecode.wndproc = NULL;
            gamecode.update_and_render = NULL;
        }
        return true;
    }
    return false;
}

void update_window_titlebar()
{
    wchar_t hotreload_txtbuf[MAX_PATH] = L"";
    wchar_t prefix_hotreload_txtbuf[20] = L" - hot reloaded: ";
    wchar_t time_hotreload_txtbuf[30] = L"";
    wcscat(hotreload_txtbuf, window_text);
    wcscat(hotreload_txtbuf, prefix_hotreload_txtbuf);
    SYSTEMTIME systime = {0};
    GetLocalTime(&systime);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_FORCE24HOURFORMAT, &systime, NULL, time_hotreload_txtbuf, 30);
    wcscat(hotreload_txtbuf, time_hotreload_txtbuf);
    SetWindowTextW(g_hwnd, hotreload_txtbuf);
}

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (game_is_ready)
    {
        gamecode.wndproc(msg, wParam, lParam);
    }

    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (game_is_ready)
        {
            gamecode.on_mousemove();
        }
        return 0;

    case WM_SIZE:

        g_hwnd_width = LOWORD(lParam);
        g_hwnd_height = HIWORD(lParam);

        if (game_is_ready && wParam != SIZE_MINIMIZED && g_hwnd_width > 0 && g_hwnd_height > 0)
        {
            g_aspect_ratio = (float)g_hwnd_width / g_hwnd_height;
            gamecode.resize((int)g_hwnd_width, g_hwnd_height);
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
