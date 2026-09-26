#include <Windows.h>
#include <TlHelp32.h>
#include <shellapi.h>
#include <wininet.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <ctime>
#include <thread>

#pragma comment(lib, "shell32.lib")

static constexpr DWORD STEAM_APPID        = 1422450;
static constexpr DWORD PROCESS_STALE_SECS = 10;
static constexpr const char* TARGET_PROC  = "deadlock.exe";

static constexpr const char* CURRENT_VERSION    = "1.0.2";
static constexpr const char* UPDATE_API_HOST    = "api.github.com";
static constexpr const char* UPDATE_API_PATH    = "/repos/wrongsprat/region-changer/releases/latest";
static constexpr const char* UPDATE_RELEASE_URL = "https://github.com/wrongsprat/region-changer/releases/tag/1.0.2";

static FILE* g_pLog = nullptr;

static std::string ExeDir()
{
    char exe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    const auto pos = dir.find_last_of('\\');
    if (pos != std::string::npos)
        dir.resize(pos + 1);
    return dir;
}

static void LogV(const char* tag, const char* fmt, va_list args)
{
    char body[512];
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, args);

    printf("[%s] %s\n", tag, body);

    if (g_pLog)
    {
        char t[32]{};
        time_t now = time(nullptr);
        struct tm tm{};
        if (localtime_s(&tm, &now) == 0)
            strftime(t, sizeof(t), "%H:%M:%S", &tm);
        fprintf(g_pLog, "[%s][%s] %s\n", t, tag, body);
        fflush(g_pLog);
    }
}

static void LogInfo(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogV("+", fmt, args);
    va_end(args);
}

static void LogWarn(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogV("!", fmt, args);
    va_end(args);
}

static void LogErr(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogV("-", fmt, args);
    va_end(args);
}

static std::string ErrStr(DWORD e = 0)
{
    if (!e) e = GetLastError();
    char buf[256]{};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, buf, sizeof(buf), nullptr);
    std::string s = buf;
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    char code[32]{};
    _snprintf_s(code, sizeof(code), _TRUNCATE, " (0x%lX)", e);
    return s + code;
}

static bool HttpGet(const char* host, const char* path, std::string& out)
{
    HINTERNET hSession = InternetOpenA("region-changer-injector", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hSession)
        return false;

    HINTERNET hConnect = InternetConnectA(hSession, host, INTERNET_DEFAULT_HTTPS_PORT,
                                          nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConnect)
    {
        InternetCloseHandle(hSession);
        return false;
    }

    const char* headers = "User-Agent: region-changer-injector\r\nAccept: application/vnd.github+json\r\n";
    HINTERNET hRequest = HttpOpenRequestA(hConnect, "GET", path, nullptr, nullptr, nullptr,
                                          INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD, 0);
    if (!hRequest)
    {
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hSession);
        return false;
    }

    bool ok = HttpSendRequestA(hRequest, headers, static_cast<DWORD>(strlen(headers)), nullptr, 0) != FALSE;
    if (ok)
    {
        DWORD status = 0, statusLen = sizeof(status);
        HttpQueryInfoA(hRequest, HTTP_QUERY_FLAG_NUMBER | HTTP_QUERY_STATUS_CODE, &status, &statusLen, nullptr);
        ok = (status == 200);
    }

    if (ok)
    {
        char buf[4096];
        DWORD read = 0;
        while (InternetReadFile(hRequest, buf, sizeof(buf), &read) && read > 0)
            out.append(buf, read);
    }

    InternetCloseHandle(hRequest);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hSession);
    return ok;
}

static std::string ExtractJsonString(const std::string& json, const char* key)
{
    const std::string needle = std::string("\"") + key + "\":\"";
    const auto pos = json.find(needle);
    if (pos == std::string::npos)
        return {};
    const auto start = pos + needle.size();
    const auto end = json.find('"', start);
    if (end == std::string::npos)
        return {};
    return json.substr(start, end - start);
}

static int NextVersionPart(const std::string& v, size_t& idx)
{
    int val = 0;
    while (idx < v.size() && v[idx] >= '0' && v[idx] <= '9')
    {
        val = val * 10 + (v[idx] - '0');
        ++idx;
    }
    if (idx < v.size() && v[idx] == '.')
        ++idx;
    return val;
}

static bool IsNewerVersion(const std::string& latest, const std::string& current)
{
    size_t li = 0, ci = 0;
    while (li < latest.size() || ci < current.size())
    {
        const int lp = NextVersionPart(latest, li);
        const int cp = NextVersionPart(current, ci);
        if (lp != cp)
            return lp > cp;
    }
    return false;
}

static bool CheckForUpdate(std::string& latestTag)
{
    LogInfo("проверка обновлений (текущая версия: %s)...", CURRENT_VERSION);

    std::string body;
    if (!HttpGet(UPDATE_API_HOST, UPDATE_API_PATH, body))
    {
        LogWarn("не удалось проверить обновления");
        return false;
    }

    latestTag = ExtractJsonString(body, "tag_name");
    if (latestTag.empty())
    {
        LogWarn("сервер вернул некорректный ответ");
        return false;
    }

    std::string version = latestTag;
    if (!version.empty() && (version[0] == 'v' || version[0] == 'V'))
        version.erase(0, 1);

    if (!IsNewerVersion(version, CURRENT_VERSION))
    {
        LogInfo("установлена последняя версия");
        return false;
    }

    LogInfo("доступно обновление: %s -> %s", CURRENT_VERSION, latestTag.c_str());
    return true;
}

static bool IsAdmin()
{
    BOOL ok = FALSE;
    PSID g = nullptr;
    SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&auth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &g))
    {
        CheckTokenMembership(nullptr, g, &ok);
        FreeSid(g);
    }
    return ok != FALSE;
}

using Fn_OpenProcess        = HANDLE(WINAPI*)(DWORD, BOOL, DWORD);
using Fn_VirtualAllocEx     = LPVOID(WINAPI*)(HANDLE, LPVOID, SIZE_T, DWORD, DWORD);
using Fn_WriteProcessMemory = BOOL(WINAPI*)(HANDLE, LPVOID, LPCVOID, SIZE_T, SIZE_T*);
using Fn_VirtualFreeEx      = BOOL(WINAPI*)(HANDLE, LPVOID, LPVOID, DWORD);
using Fn_GetExitCodeThread  = BOOL(WINAPI*)(HANDLE, LPDWORD);
using Fn_LoadLibraryA       = LPTHREAD_START_ROUTINE;

using Fn_NtCreateThreadEx = LONG(NTAPI*)(
    PHANDLE, ACCESS_MASK, PVOID, HANDLE,
    PVOID, PVOID, ULONG, SIZE_T, SIZE_T, SIZE_T, PVOID);

template<typename T>
static T ResolveProc(HMODULE hMod, char* name)
{
    return reinterpret_cast<T>(GetProcAddress(hMod, name));
}

static DWORD FindProc(const char* name)
{
    DWORD pid = 0;
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32 pe{ sizeof(pe) };
    if (Process32First(h, &pe)) do {
        if (_stricmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
    } while (Process32Next(h, &pe));
    CloseHandle(h);
    return pid;
}

static DWORD Uptime(DWORD pid)
{
    char k32[] = {'k','e','r','n','e','l','3','2','.','d','l','l',0};
    char sQL[] = {'O','p','e','n','P','r','o','c','e','s','s',0};
    HMODULE hK = GetModuleHandleA(k32);
    if (!hK) return 0;
    auto fnOP = ResolveProc<Fn_OpenProcess>(hK, sQL);
    if (!fnOP) return 0;

    HANDLE h = fnOP(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    FILETIME fc{}, fe{}, fk{}, fu{};
    DWORD up = 0;
    if (GetProcessTimes(h, &fc, &fe, &fk, &fu))
    {
        FILETIME fn{};
        GetSystemTimeAsFileTime(&fn);
        ULARGE_INTEGER c{}, n{};
        c.LowPart = fc.dwLowDateTime; c.HighPart = fc.dwHighDateTime;
        n.LowPart = fn.dwLowDateTime; n.HighPart = fn.dwHighDateTime;
        up = static_cast<DWORD>((n.QuadPart - c.QuadPart) / 10000000ULL);
    }
    CloseHandle(h);
    return up;
}

static void KillProc(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h) { TerminateProcess(h, 0); CloseHandle(h); }
}

static void LaunchGame()
{
    char url[40]{};
    _snprintf_s(url, sizeof(url), _TRUNCATE, "steam://rungameid/%lu", STEAM_APPID);
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

static DWORD WaitForTarget()
{
    LogInfo("waiting for %s...", TARGET_PROC);
    DWORD waited = 0;
    bool launched = false;
    while (true)
    {
        const DWORD pid = FindProc(TARGET_PROC);
        if (!pid)
        {
            if (!launched)
            {
                waited += 1000;
                if (waited >= 2000)
                {
                    launched = true;
                    LogInfo("launching via steam...");
                    LaunchGame();
                }
            }
            Sleep(1000);
            continue;
        }

        const DWORD up = Uptime(pid);
        if (up == 0 || up <= PROCESS_STALE_SECS)
            return pid;

        LogWarn("stale PID %lu (%lus uptime), restarting...", pid, up);
        KillProc(pid);
        Sleep(1500);
        LaunchGame();
        while (true)
        {
            const DWORD np = FindProc(TARGET_PROC);
            if (np && (Uptime(np) == 0 || Uptime(np) <= PROCESS_STALE_SECS))
                return np;
            Sleep(1000);
        }
    }
}

static bool DoLoad(const std::string& path, DWORD pid)
{
    char k32[] = {'k','e','r','n','e','l','3','2','.','d','l','l',0};
    HMODULE hK = GetModuleHandleA(k32);
    if (!hK) { LogErr("kernel32 unavailable"); return false; }

    char sOP[]  = {'O','p','e','n','P','r','o','c','e','s','s',0};
    char sVAE[] = {'V','i','r','t','u','a','l','A','l','l','o','c','E','x',0};
    char sWPM[] = {'W','r','i','t','e','P','r','o','c','e','s','s','M','e','m','o','r','y',0};
    char sVFE[] = {'V','i','r','t','u','a','l','F','r','e','e','E','x',0};
    char sLL[]  = {'L','o','a','d','L','i','b','r','a','r','y','A',0};
    char sGET[] = {'G','e','t','E','x','i','t','C','o','d','e','T','h','r','e','a','d',0};

    auto fnOP  = ResolveProc<Fn_OpenProcess>(hK, sOP);
    auto fnVAE = ResolveProc<Fn_VirtualAllocEx>(hK, sVAE);
    auto fnWPM = ResolveProc<Fn_WriteProcessMemory>(hK, sWPM);
    auto fnVFE = ResolveProc<Fn_VirtualFreeEx>(hK, sVFE);
    auto fnLL  = ResolveProc<Fn_LoadLibraryA>(hK, sLL);
    auto fnGET = ResolveProc<Fn_GetExitCodeThread>(hK, sGET);

    if (!fnOP || !fnVAE || !fnWPM || !fnVFE || !fnLL || !fnGET)
    {
        LogErr("API resolution failed");
        return false;
    }

    char ntdll[] = {'n','t','d','l','l','.','d','l','l',0};
    char sNtT[]  = {'N','t','C','r','e','a','t','e','T','h','r','e','a','d','E','x',0};
    HMODULE hNt = GetModuleHandleA(ntdll);
    auto fnNtCTE = ResolveProc<Fn_NtCreateThreadEx>(hNt, sNtT);

    const DWORD acc = PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                      PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION;
    HANDLE hProc = fnOP(acc, FALSE, pid);
    if (!hProc)
    {
        LogErr("OpenProcess failed: %s", ErrStr().c_str());
        return false;
    }

    const size_t len = path.size() + 1;
    LPVOID pMem = fnVAE(hProc, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pMem)
    {
        CloseHandle(hProc);
        LogErr("VirtualAllocEx failed: %s", ErrStr().c_str());
        return false;
    }

    if (!fnWPM(hProc, pMem, path.c_str(), len, nullptr))
    {
        fnVFE(hProc, pMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        LogErr("WriteProcessMemory failed: %s", ErrStr().c_str());
        return false;
    }

    HANDLE hThread = nullptr;
    if (fnNtCTE)
    {
        fnNtCTE(&hThread, THREAD_ALL_ACCESS, nullptr, hProc,
                fnLL, pMem, 0, 0, 0, 0, nullptr);
    }

    if (!hThread)
    {
        char sCRT[] = {'C','r','e','a','t','e','R','e','m','o','t','e',
                       'T','h','r','e','a','d',0};
        using Fn_CRT = HANDLE(WINAPI*)(HANDLE, LPSECURITY_ATTRIBUTES, SIZE_T,
                                       LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
        auto fnCRT = ResolveProc<Fn_CRT>(hK, sCRT);
        if (fnCRT)
            hThread = fnCRT(hProc, nullptr, 0, fnLL, pMem, 0, nullptr);
    }

    if (!hThread)
    {
        LogErr("remote thread creation failed: %s", ErrStr().c_str());
        fnVFE(hProc, pMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    if (WaitForSingleObject(hThread, 30000) == WAIT_TIMEOUT)
        LogWarn("thread wait timeout (30s)");

    DWORD code = 0;
    fnGET(hThread, &code);
    CloseHandle(hThread);
    fnVFE(hProc, pMem, 0, MEM_RELEASE);
    CloseHandle(hProc);

    if (!code)
    {
        LogErr("LoadLibraryA returned NULL");
        return false;
    }

    LogInfo("injected OK (module handle=%p)", reinterpret_cast<void*>(code));
    return true;
}

int main(int argc, char* argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::string dllPath;
    for (int i = 1; i < argc; ++i)
    {
        if (dllPath.empty())
            dllPath = argv[i];
    }
    if (dllPath.empty())
        dllPath = ExeDir() + "dysonbehind.dll";

    fopen_s(&g_pLog, (ExeDir() + "injector_debug.txt").c_str(), "w");

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    LogInfo("dysonbehind injector");
    LogInfo("dll: %s", dllPath.c_str());

    std::string latestTag;
    if (CheckForUpdate(latestTag))
    {
        printf("\nДоступно обновление %s -> %s.\n", CURRENT_VERSION, latestTag.c_str());
        printf("Хотите открыть страницу нового обновления?\n");
        printf("1. Да\n");
        printf("2. Нет\n");
        printf("Ваш выбор: ");
        char answer[16]{};
        fgets(answer, sizeof(answer), stdin);
        if (answer[0] == '1')
        {
            const HINSTANCE opened = ShellExecuteA(nullptr, "open", UPDATE_RELEASE_URL,
                                                    nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(opened) <= 32)
                LogWarn("не удалось открыть страницу обновления");
            else
                LogInfo("страница обновления открыта");
        }
        else
        {
            LogInfo("обновление пропущено");
        }
    }

    if (!IsAdmin())
    {
        LogErr("admin rights required");
        printf("\n[!] Run as Administrator\n");
        system("pause");
        if (g_pLog) fclose(g_pLog);
        return 1;
    }

    if (GetFileAttributesA(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        LogErr("dll not found: %s", dllPath.c_str());
        printf("\n[!] DLL not found: %s\n", dllPath.c_str());
        system("pause");
        if (g_pLog) fclose(g_pLog);
        return 1;
    }

    const DWORD pid = WaitForTarget();
    LogInfo("target PID: %lu", pid);

    Sleep(2000);

    const bool ok = DoLoad(dllPath, pid);
    printf("\n%s\n", ok ? "[+] Injected. INSERT toggles menu in game."
                        : "[!] Injection failed, see injector_debug.txt");

    if (g_pLog) fclose(g_pLog);
    if (!ok)
        system("pause");
    return ok ? 0 : 1;
}
