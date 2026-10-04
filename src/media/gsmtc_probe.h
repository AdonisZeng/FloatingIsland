#pragma once
// Stage 0 验证工具：探测本进程能否通过 GSMTC 拿到系统媒体会话。
//
// 这是整个项目最大的外部风险点：未打包（无 package identity）的 Win32 进程
// 在部分 Windows 版本上拿不到其他应用的会话，GetSessions() 会返回空列表。
// 先跑通这个探测，再决定是否需要 sparse MSIX 方案。
//
// 实现用 WRL + SDK 自带的 WinRT ABI 头文件，不依赖 NuGet 的 C++/WinRT，
// 保持 /MT 静态链接下的零外部依赖。

#include <windows.h>

#include <string>

namespace fi {

struct GsmtcProbeResult {
    bool    hasIdentity    = false;   // 进程是否有 package identity（sparse MSIX 注册后才有）
    std::wstring identity;            // 包全名
    bool    comInitialized = false;
    HRESULT comHr          = S_OK;
    bool    factoryOk      = false;   // RoGetActivationFactory 是否成功
    HRESULT factoryHr      = S_OK;
    bool    managerOk      = false;   // RequestAsync 是否成功返回 SessionManager
    HRESULT managerHr      = S_OK;
    int     sessionCount   = -1;      // -1 表示没走到这一步
    std::wstring firstAppId;
    std::wstring firstTitle;
    std::wstring firstArtist;
    std::wstring notes;
};

// 内部会泵消息等待异步回调，最长阻塞约 4 秒
HRESULT ProbeGsmtc(GsmtcProbeResult& out);

// 生成给人看的诊断文本（用于 MessageBox / 日志）
std::wstring FormatProbeReport(const GsmtcProbeResult& r);

} // namespace fi
