#pragma once
// 轻量日志：OutputDebugString + 文件。
// 每次启动在工程根 log\ 目录新建一个独立 txt：log_YYYYMMDD_HHMMSS.txt
// （年月日时分秒取启动时刻本地时间；同秒启动自动加 _2/_3 后缀）。
// 目录定位：从 exe 所在目录逐级向上找工程根（含 FloatingIsland.vcxproj 或
// src+log 的那一层），找不到时退回 exe 同级 log\，再退回 %LOCALAPPDATA%。
// 线程安全（内部临界区）；只保留最近 30 份日志，超出清理最旧的。
// 约定：不输出用户媒体内容明文（歌名/歌词只记长度），避免日志含 PII。

#include <windows.h>

#include <string>

namespace fi {

enum class LogLevel { Info, Warn, Error };

// 初始化文件日志；目录创建失败时静默退化为仅 OutputDebugString
void LogInit();
void LogShutdown();

void LogWrite(LogLevel level, const wchar_t* fmt, ...);

// 当前日志文件的完整路径（文件未创建成功时为空串）
const std::wstring& LogFilePath();

} // namespace fi

#define LOGI(...) ::fi::LogWrite(::fi::LogLevel::Info,  __VA_ARGS__)
#define LOGW(...) ::fi::LogWrite(::fi::LogLevel::Warn,  __VA_ARGS__)
#define LOGE(...) ::fi::LogWrite(::fi::LogLevel::Error, __VA_ARGS__)
