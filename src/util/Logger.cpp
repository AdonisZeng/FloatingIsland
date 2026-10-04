#include "Logger.h"

#include <shlobj.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

namespace fi {

namespace {

constexpr size_t kMaxLine   = 1024;
constexpr int    kKeepFiles = 30;   // 保留的最新日志文件数量

CRITICAL_SECTION g_cs;
FILE*            g_file   = nullptr;
LONG             g_csInit = 0;   // Interlocked 访问：LogWrite 可能在任意线程
std::wstring     g_path;         // 当前日志文件完整路径

const wchar_t* LevelTag(LogLevel level) {
    switch (level) {
    case LogLevel::Warn:  return L"W";
    case LogLevel::Error: return L"E";
    default:              return L"I";
    }
}

std::filesystem::path ModuleDir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf).parent_path();
}

// 工程根判据：有 vcxproj，或同时有 src 与 log 两个子目录
bool LooksLikeProjectRoot(const std::filesystem::path& dir) {
    std::error_code ec;
    if (dir.empty()) return false;
    if (std::filesystem::exists(dir / L"FloatingIsland.vcxproj", ec)) return true;
    return std::filesystem::is_directory(dir / L"src", ec) &&
           std::filesystem::is_directory(dir / L"log", ec);
}

// exe 通常在 <根>\x64\Debug\，向上最多 5 级即可回到工程根
std::filesystem::path ResolveLogDir() {
    const std::filesystem::path exeDir = ModuleDir();
    if (!exeDir.empty()) {
        std::filesystem::path dir = exeDir;
        for (int i = 0; i < 5 && !dir.empty(); ++i) {
            if (LooksLikeProjectRoot(dir)) return dir / L"log";
            const std::filesystem::path parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
        return exeDir / L"log";
    }
    wchar_t* local = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        const std::filesystem::path p =
            std::filesystem::path(local) / L"FloatingIsland" / L"log";
        ::CoTaskMemFree(local);
        return p;
    }
    return std::filesystem::path(L"log");
}

// log_YYYYMMDD_HHMMSS.txt
std::wstring StampName(const SYSTEMTIME& st) {
    wchar_t name[64]{};
    _snwprintf_s(name, _TRUNCATE, L"log_%04u%02u%02u_%02u%02u%02u.txt",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return name;
}

std::filesystem::path UniquePath(const std::filesystem::path& dir,
                                 const std::wstring& stamp) {
    std::error_code ec;
    const std::wstring base = stamp.substr(0, stamp.rfind(L'.'));   // 去扩展名
    std::filesystem::path path = dir / stamp;
    if (!std::filesystem::exists(path, ec)) return path;
    for (int i = 2; i < 100; ++i) {   // 同一秒内的第 2、3… 次启动
        wchar_t suffix[16]{};
        _snwprintf_s(suffix, _TRUNCATE, L"_%d.txt", i);
        path = dir / (base + suffix);
        if (!std::filesystem::exists(path, ec)) return path;
    }
    return dir / stamp;
}

// 只清理本模块产出的 log_*.txt；名字含时间戳，字典序即时间序
void PruneOldLogs(const std::filesystem::path& dir) {
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        const std::filesystem::path p = it->path();
        if (!std::filesystem::is_regular_file(p, ec)) continue;
        const std::wstring n = p.filename().wstring();
        if (n.size() < 8 || n.compare(0, 4, L"log_") != 0) continue;
        if (::_wcsicmp(p.extension().c_str(), L".txt") != 0) continue;
        files.push_back(p);
    }
    if (static_cast<int>(files.size()) <= kKeepFiles) return;
    std::sort(files.begin(), files.end());
    const size_t del = files.size() - static_cast<size_t>(kKeepFiles);
    for (size_t i = 0; i < del; ++i) std::filesystem::remove(files[i], ec);
}

void WriteHeader() {
    if (!g_file) return;

    SYSTEMTIME st{};
    ::GetLocalTime(&st);

    wchar_t exe[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);

    ::fwprintf(g_file,
               L"==== FloatingIsland 会话开始 %04u-%02u-%02u %02u:%02u:%02u ====\r\n",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    ::fwprintf(g_file, L"进程: pid=%lu\r\n", ::GetCurrentProcessId());
    ::fwprintf(g_file, L"模块: %s\r\n", exe);
    ::fwprintf(g_file, L"命令行: %s\r\n",
               ::GetCommandLineW() ? ::GetCommandLineW() : L"");
    ::fwprintf(g_file, L"日志: %s\r\n", g_path.c_str());
    ::fflush(g_file);
}

void OpenFileLocked() {
    const std::filesystem::path dir = ResolveLogDir();

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return;

    SYSTEMTIME st{};
    ::GetLocalTime(&st);

    const std::filesystem::path path = UniquePath(dir, StampName(st));
    if (_wfopen_s(&g_file, path.c_str(), L"a, ccs=UTF-8") != 0) {
        g_file = nullptr;
        return;
    }
    g_path = path.wstring();
    WriteHeader();
    PruneOldLogs(dir);
}

} // namespace

void LogInit() {
    if (!::InterlockedCompareExchange(&g_csInit, 1, 0)) {
        ::InitializeCriticalSection(&g_cs);
    }
    ::EnterCriticalSection(&g_cs);
    if (!g_file) OpenFileLocked();
    ::LeaveCriticalSection(&g_cs);
}

// 只能在所有后台线程 join 之后调用。
// 注意这里刻意不 DeleteCriticalSection：万一还有迟到的 LogWrite，
// 踩到已删除的临界区会直接崩溃，而保留它最多是几字节内存不回收。
void LogShutdown() {
    if (!::InterlockedCompareExchange(&g_csInit, 1, 1)) return;   // 从未 LogInit：临界区未初始化
    ::EnterCriticalSection(&g_cs);
    if (g_file) {
        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        ::fwprintf(g_file, L"==== 会话结束 %02u:%02u:%02u ====\r\n",
                   st.wHour, st.wMinute, st.wSecond);
        ::fclose(g_file);
        g_file = nullptr;
    }
    ::LeaveCriticalSection(&g_cs);
}

const std::wstring& LogFilePath() {
    return g_path;
}

void LogWrite(LogLevel level, const wchar_t* fmt, ...) {
    wchar_t line[kMaxLine];

    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(line, _TRUNCATE, fmt, args);
    va_end(args);

    SYSTEMTIME st{};
    ::GetLocalTime(&st);

    wchar_t stamped[kMaxLine + 64];
    _snwprintf_s(stamped, _TRUNCATE,
                 L"[%02u-%02u %02u:%02u:%02u.%03u][%s][%u] %s\r\n",
                 st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 st.wMilliseconds, LevelTag(level), ::GetCurrentThreadId(), line);

    ::OutputDebugStringW(stamped);

    if (!::InterlockedCompareExchange(&g_csInit, 1, 1)) return;   // 未初始化：只走调试输出
    ::EnterCriticalSection(&g_cs);
    if (g_file) {
        ::fwprintf(g_file, L"%s", stamped);
        ::fflush(g_file);
    }
    ::LeaveCriticalSection(&g_cs);
}

} // namespace fi
