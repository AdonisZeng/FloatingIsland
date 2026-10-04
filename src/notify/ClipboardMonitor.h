#pragma once
// 剪贴板链接监听：AddClipboardFormatListener + WM_CLIPBOARDUPDATE 事件驱动
// （稳态零 CPU）。仅当整段文本就是一个 URL 时才触发。
// 触发后弹 3 秒面板，点击用默认浏览器打开（由主线程处理点击）。

#include <windows.h>

#include <functional>
#include <string>

namespace fi {

class ClipboardMonitor {
public:
    void Start(HWND hwnd, std::function<void(std::wstring)> onUrl);
    void Stop();

    // 供 WndProc 调用
    void OnClipboardUpdate();
    // 取回最近一次命中的 URL（并清空）
    bool TakeUrl(std::wstring& out);

private:
    HWND hwnd_ = nullptr;
    bool registered_ = false;
    std::function<void(std::wstring)> onUrl_;
    std::wstring pendingUrl_;
};

// 整段文本是否是一个 http/https/ftp/www 链接
bool LooksLikeUrl(const std::wstring& text);

} // namespace fi
