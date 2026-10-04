#pragma once
// 显示级别调度：Toast > 剪贴板 > 媒体（对齐参照 NotificationCenter）。
// Toast 三档呈现由配置 toastMode 控制（0 缩略 / 1 紧凑 / 2 完整）；
// 面板展示期间不参与自动隐藏（由 AutoHide 读 Level() 判定）。

#include <windows.h>

#include <memory>
#include <mutex>
#include <string>

#include "ToastListener.h"

namespace fi {

class ClipboardMonitor;

enum class ContentLevel { Media = 0, Clipboard, Toast };

struct ToastView {
    bool valid = false;
    std::wstring appName, title, body;
    ULONGLONG arriveTick = 0;
    int mode = 0;          // 0 缩略 / 1 紧凑 / 2 完整
};

class NotificationCenter {
public:
    NotificationCenter();
    ~NotificationCenter();   // unique_ptr 成员类型不完整，析构放 cpp
    NotificationCenter(const NotificationCenter&)            = delete;
    NotificationCenter& operator=(const NotificationCenter&) = delete;

    void Start(HWND hwnd);
    void Stop();
    void Tick();           // 主线程 1s：过期清理
    // 透传 WM_CLIPBOARDUPDATE
    void OnClipboardUpdate();

    // 输入源
    void OnToast(const ToastRecord& rec);
    void OnClipboardUrl(const std::wstring& url);

    // 主线程：收到 kMsgToast 后排空监听器队列
    bool TakeToast(ToastRecord& out);

    // 渲染读取
    bool TryGetToast(ToastView& out) const;
    ContentLevel Level() const;
    // 点击剪贴板面板：取 URL 并清空
    bool TakeClipboardUrl(std::wstring& out);
    ULONGLONG ClipboardExpireTick() const;

private:
    std::unique_ptr<ToastListener>      toastListener_;
    std::unique_ptr<ClipboardMonitor>   clip_;
    mutable std::mutex m_;
    ToastView  toastView_;
    ULONGLONG  toastExpire_ = 0;
    std::wstring clipUrl_;
    ULONGLONG  clipExpire_ = 0;
};

} // namespace fi
