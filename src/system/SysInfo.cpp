#include "SysInfo.h"

#include <windows.h>

namespace fi {

namespace {
FILETIME s_lastIdle{}, s_lastKernel{}, s_lastUser{};
bool     s_haveLast = false;
}

float SysInfo::CpuUsagePercent() {
    FILETIME idle{}, kernel{}, user{};
    if (!::GetSystemTimes(&idle, &kernel, &user)) return 0.0f;
    if (!s_haveLast) {
        s_lastIdle = idle; s_lastKernel = kernel; s_lastUser = user;
        s_haveLast = true;
        return 0.0f;
    }
    auto ticks = [](const FILETIME& ft) -> ULONGLONG {
        return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    const ULONGLONG dIdle   = ticks(idle)   - ticks(s_lastIdle);
    const ULONGLONG dKernel = ticks(kernel) - ticks(s_lastKernel);
    const ULONGLONG dUser   = ticks(user)   - ticks(s_lastUser);
    s_lastIdle = idle; s_lastKernel = kernel; s_lastUser = user;

    const ULONGLONG total = dKernel + dUser;
    if (total == 0) return 0.0f;
    const float busy = static_cast<float>(total - dIdle) * 100.0f /
                       static_cast<float>(total);
    return (busy < 0.0f) ? 0.0f : (busy > 100.0f ? 100.0f : busy);
}

float SysInfo::MemoryUsagePercent() {
    MEMORYSTATUSEX st{};
    st.dwLength = sizeof(st);
    if (!::GlobalMemoryStatusEx(&st)) return 0.0f;
    return static_cast<float>(st.dwMemoryLoad);
}

std::wstring SysInfo::TimeText() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t buf[16];
    swprintf_s(buf, L"%02d:%02d", st.wHour, st.wMinute);
    return buf;
}

std::wstring SysInfo::DateText() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%d月%d日 %s", st.wMonth, st.wDay,
               [&]() -> const wchar_t* {
                   static const wchar_t* kWeek[] = { L"日", L"一", L"二", L"三",
                                                     L"四", L"五", L"六" };
                   return (st.wDayOfWeek < 7) ? kWeek[st.wDayOfWeek] : L"";
               }());
    return buf;
}

} // namespace fi
