#include "DevLog.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <mutex>

static std::mutex g_LogMutex;
static HANDLE g_hLogFile = INVALID_HANDLE_VALUE;

static void EnsureLogFile()
{
    if (g_hLogFile != INVALID_HANDLE_VALUE)
        return;

    char szPath[MAX_PATH]{};
    HMODULE hSelf = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&EnsureLogFile), &hSelf))
        return;

    if (GetModuleFileNameA(hSelf, szPath, MAX_PATH) == 0)
        return;

    char* pSlash = strrchr(szPath, '\\');
    if (!pSlash)
        return;
    *(pSlash + 1) = '\0';
    strcat_s(szPath, "dysonbehind_debug.txt");

    g_hLogFile = CreateFileA(szPath, GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void DevLogWrite(const char* level, const char* fmt, ...)
{
    char body[512];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, args);
    va_end(args);

    char line[600];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[ %s ] %s\n", level, body);

    OutputDebugStringA(line);

    std::lock_guard lock(g_LogMutex);
    EnsureLogFile();
    if (g_hLogFile != INVALID_HANDLE_VALUE)
    {
        DWORD dw = 0;
        SetFilePointer(g_hLogFile, 0, nullptr, FILE_END);
        WriteFile(g_hLogFile, line, static_cast<DWORD>(strlen(line)), &dw, nullptr);
        FlushFileBuffers(g_hLogFile);
    }
}
