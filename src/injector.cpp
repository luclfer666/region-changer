#include <Windows.h>
#include <TlHelp32.h>
#include <shellapi.h>
#include <wininet.h>
#include <winhttp.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <fstream>
#include <ctime>
#include <thread>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "wininet.lib")

static constexpr DWORD STEAM_APPID        = 1422450;
static constexpr DWORD PROCESS_STALE_SECS = 10;
static constexpr const char* TARGET_PROC  = "deadlock.exe";

static constexpr const char* CURRENT_VERSION    = "1.0.2";
static constexpr const char* UPDATE_API_HOST    = "api.github.com";
static constexpr const char* UPDATE_API_PATH    = "/repos/wrongsprat/region-changer/releases/latest";
static constexpr const char* UPDATE_ASSET_PATH  = "/wrongsprat/region-changer/releases/latest/download/release.zip";

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

static bool DownloadFile(const std::string& url, const std::string& outputPath)
{
    std::string value = url;
    bool secure = false;
    if (value.rfind("https://", 0) == 0)
    {
        secure = true;
        value.erase(0, 8);
    }
    else if (value.rfind("http://", 0) == 0)
    {
        value.erase(0, 7);
    }
    else
    {
        return false;
    }

    const size_t slash = value.find('/');
    const std::string host = slash == std::string::npos ? value : value.substr(0, slash);
    const std::string path = slash == std::string::npos ? "/" : value.substr(slash);
    const std::wstring wideHost(host.begin(), host.end());
    const std::wstring widePath(path.begin(), path.end());

    HINTERNET session = WinHttpOpen(L"region-changer-injector", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return false;

    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

    HINTERNET connect = WinHttpConnect(session, wideHost.c_str(),
                                       secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!connect)
    {
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", widePath.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request)
    {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE;
    if (ok)
        ok = WinHttpReceiveResponse(request, nullptr) != FALSE;

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (ok)
    {
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
        ok = status == 200;
    }

    std::ofstream file;
    if (ok)
    {
        file.open(outputPath, std::ios::binary | std::ios::trunc);
        ok = file.is_open();
    }

    DWORD available = 0;
    while (ok && WinHttpQueryDataAvailable(request, &available) && available > 0)
    {
        char buffer[65536];
        DWORD read = 0;
        const DWORD chunk = available < sizeof(buffer) ? available : static_cast<DWORD>(sizeof(buffer));
        if (!WinHttpReadData(request, buffer, chunk, &read) || read == 0)
        {
            ok = false;
            break;
        }
        file.write(buffer, read);
        if (!file.good())
        {
            ok = false;
            break;
        }
    }

    if (file.is_open())
        file.close();
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (!ok)
        DeleteFileA(outputPath.c_str());
    return ok;
}

static std::string QuoteArg(const std::string& value)
{
    std::string result = "\"";
    for (const char ch : value)
    {
        if (ch == '\"')
            result += '\\';
        result += ch;
    }
    result += '\"';
    return result;
}

static std::string BatchQuote(const std::string& value)
{
    std::string result = "\"";
    for (const char ch : value)
    {
        if (ch == '"')
            result += '^';
        if (ch == '%')
            result += '%';
        result += ch;
    }
    result += '"';
    return result;
}

static bool StartProcess(const std::string& commandLine, PROCESS_INFORMATION& pi, DWORD flags = 0)
{
    STARTUPINFOA si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    std::vector<char> command(commandLine.begin(), commandLine.end());
    command.push_back('\0');
    return CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi) != FALSE;
}

static bool RunHiddenCommand(const std::string& commandLine, DWORD timeout)
{
    PROCESS_INFORMATION pi{};
    if (!StartProcess(commandLine, pi, CREATE_NO_WINDOW))
        return false;
    const DWORD wait = WaitForSingleObject(pi.hProcess, timeout);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return wait == WAIT_OBJECT_0 && code == 0;
}

static bool RemoveTree(const std::string& path)
{
    WIN32_FIND_DATAA data{};
    HANDLE find = FindFirstFileA((path + "\\*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0)
                continue;
            const std::string child = path + "\\" + data.cFileName;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveTree(child);
            else
                DeleteFileA(child.c_str());
        } while (FindNextFileA(find, &data));
        FindClose(find);
    }
    RemoveDirectoryA(path.c_str());
    return true;
}

static bool IsRegularFile(const std::string& path)
{
    const DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool ExtractArchive(const std::string& archivePath, const std::string& outputDir)
{
    RemoveTree(outputDir);
    if (!CreateDirectoryA(outputDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    const std::string command = "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" +
                                archivePath + "' -DestinationPath '" + outputDir + "' -Force\"";
    return RunHiddenCommand(command, 60000);
}

static std::string FindUpdateRoot(const std::string& root)
{
    if (IsRegularFile(root + "\\Injector.exe") && IsRegularFile(root + "\\dysonbehind.dll"))
        return root;
    if (IsRegularFile(root + "\\release\\Injector.exe") && IsRegularFile(root + "\\release\\dysonbehind.dll"))
        return root + "\\release";

    WIN32_FIND_DATAA data{};
    HANDLE find = FindFirstFileA((root + "\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        return {};
    std::string result;
    do
    {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            strcmp(data.cFileName, ".") != 0 && strcmp(data.cFileName, "..") != 0)
        {
            const std::string candidate = root + "\\" + data.cFileName;
            if (IsRegularFile(candidate + "\\Injector.exe") && IsRegularFile(candidate + "\\dysonbehind.dll"))
            {
                result = candidate;
                break;
            }
        }
    } while (FindNextFileA(find, &data));
    FindClose(find);
    return result;
}

static bool WriteUpdaterScript(const std::string& path, const std::string& archive,
                               const std::string& extractDir, const std::string& sourceDir,
                               const std::string& exeDir, const std::string& restartArgs)
{
    std::ofstream script(path, std::ios::binary | std::ios::trunc);
    if (!script.is_open())
        return false;
    const DWORD pid = GetCurrentProcessId();
    const std::string errorPath = exeDir + "injector_update_error.txt";
    script << "@echo off\r\nsetlocal\r\n:wait_loop\r\ntasklist /FI \"PID eq " << pid << "\" 2>nul | find \"" << pid << "\" >nul\r\n";
    script << "if not errorlevel 1 (timeout /t 1 /nobreak >nul & goto wait_loop)\r\n";
    script << "timeout /t 1 /nobreak >nul\r\n";
    script << "copy /Y " << BatchQuote(sourceDir + "\\dysonbehind.dll") << " " << BatchQuote(exeDir + "dysonbehind.dll") << " >nul\r\n";
    script << "if errorlevel 1 goto failed\r\n";
    script << "copy /Y " << BatchQuote(sourceDir + "\\Injector.exe") << " " << BatchQuote(exeDir + "Injector.exe") << " >nul\r\n";
    script << "if errorlevel 1 goto failed\r\n";
    script << "start \"\" " << BatchQuote(exeDir + "Injector.exe") << restartArgs << "\r\n";
    script << "del /F /Q " << BatchQuote(archive) << " >nul 2>&1\r\n";
    script << "rmdir /S /Q " << BatchQuote(extractDir) << " >nul 2>&1\r\n";
    script << "del /F /Q \"%~f0\" >nul 2>&1\r\nexit /b 0\r\n:failed\r\necho update failed > " << QuoteArg(errorPath) << "\r\nexit /b 1\r\n";
    return script.good();
}

static bool PrepareAndLaunchUpdate(const std::string& archivePath, const std::string& restartArgs)
{
    char temp[MAX_PATH]{};
    if (!GetTempPathA(MAX_PATH, temp))
        return false;
    const std::string extractDir = std::string(temp) + "region_changer_update\\";
    if (!ExtractArchive(archivePath, extractDir))
        return false;
    const std::string sourceDir = FindUpdateRoot(extractDir);
    if (sourceDir.empty())
    {
        RemoveTree(extractDir);
        return false;
    }
    const std::string scriptPath = ExeDir() + "_region_changer_updater.cmd";
    DeleteFileA(scriptPath.c_str());
    if (!WriteUpdaterScript(scriptPath, archivePath, extractDir, sourceDir, ExeDir(), restartArgs))
    {
        RemoveTree(extractDir);
        DeleteFileA(scriptPath.c_str());
        return false;
    }
    PROCESS_INFORMATION pi{};
    if (!StartProcess("cmd.exe /c " + QuoteArg(scriptPath), pi, CREATE_NO_WINDOW))
    {
        RemoveTree(extractDir);
        DeleteFileA(scriptPath.c_str());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static bool CheckForUpdate(std::string& downloadedArchive)
{
    LogInfo("checking for updates (current: %s)...", CURRENT_VERSION);

    std::string body;
    if (!HttpGet(UPDATE_API_HOST, UPDATE_API_PATH, body))
    {
        LogWarn("update check failed (network error)");
        return false;
    }

    std::string tag = ExtractJsonString(body, "tag_name");
    if (tag.empty())
    {
        LogWarn("update check failed (bad response)");
        return false;
    }

    std::string version = tag;
    if (!version.empty() && (version[0] == 'v' || version[0] == 'V'))
        version.erase(0, 1);

    if (!IsNewerVersion(version, CURRENT_VERSION))
    {
        LogInfo("up to date");
        return false;
    }

    char temp[MAX_PATH]{};
    if (!GetTempPathA(MAX_PATH, temp))
    {
        LogWarn("unable to get temporary path");
        return false;
    }
    downloadedArchive = std::string(temp) + "region_changer_update.zip";
    DeleteFileA(downloadedArchive.c_str());
    if (!DownloadFile("https://github.com" + std::string(UPDATE_ASSET_PATH), downloadedArchive))
    {
        DeleteFileA(downloadedArchive.c_str());
        LogWarn("update download failed");
        return false;
    }

    LogInfo("update downloaded: %s", downloadedArchive.c_str());
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

static bool PrepareAndLaunchUpdate(const std::string& archivePath, const std::string& restartArgs);

static std::string BuildRestartArgs(int argc, char* argv[], int first)
{
    std::string args;
    for (int i = first; i < argc; ++i)
    {
        if (!args.empty())
            args += ' ';
        args += QuoteArg(argv[i]);
    }
    return args;
}

static bool PrepareAndLaunchUpdate(const std::string& archivePath, const std::string& restartArgs);int main(int argc, char* argv[])
{
    std::string dllPath;
    for (int i = 1; i < argc; ++i)
    {
        if (dllPath.empty())
            dllPath = argv[i];
    }
    if (dllPath.empty())
        dllPath = ExeDir() + "dysonbehind.dll";

    fopen_s(&g_pLog, (ExeDir() + "injector_debug.txt").c_str(), "w");

    LogInfo("dysonbehind injector");
    LogInfo("dll: %s", dllPath.c_str());

    std::string updateArchive;
    if (CheckForUpdate(updateArchive))
    {
        const std::string restartArgs = BuildRestartArgs(argc, argv, 1);
        if (PrepareAndLaunchUpdate(updateArchive, restartArgs))
        {
            LogInfo("update prepared, stopping current injector");
            if (g_pLog) fclose(g_pLog);
            return 0;
        }
        LogWarn("update preparation failed, continuing current version");
        DeleteFileA(updateArchive.c_str());
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
