#pragma once
// 轻量系统信息：CPU 占用（GetSystemTimes 差分）、内存占用、时间日期。
// 每秒级调用即可，不使用 PerformanceCounter。

#include <string>

namespace fi {

class SysInfo {
public:
    // CPU 占用 0~100（内部保存上次采样）
    static float CpuUsagePercent();
    // 已用内存百分比
    static float MemoryUsagePercent();
    // 本地时间 "HH:mm" / "M月d日 E"（均用当前线程 locale，UTF-8 无关）
    static std::wstring TimeText();
    static std::wstring DateText();
};

} // namespace fi
