#pragma once
// Windows Toast 监听：Windows.UI.Notifications.Management.UserNotificationListener。
// C++/WinRT 原始 ABI 无法表达 IVectorView<UserNotification>（MIDL 未特化），
// 因此本模块单独使用 SDK 自带 cppwinrt 头（$(WindowsSdkDir)..\cppwinrt）。
// 轮询在独立线程上进行（阻塞式 get() 绝不占用渲染线程），新通知经窗口消息
// 投递回主线程；ID 水位线判新，权限失效自动重新申请。

#include <windows.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../util/StopSignal.h"

namespace fi {

struct ToastRecord {
    uint64_t    id = 0;
    std::wstring appName, title, body;
};

class ToastListener {
public:
    // 通知消息不带负载：主线程收到后调用 TryTake 排空内部队列，
    // 避免「new ToastRecord 作为消息参数」在窗口销毁后泄漏。
    static constexpr UINT kMsgToast = WM_APP + 22;

    ToastListener() = default;
    ~ToastListener();
    ToastListener(const ToastListener&)            = delete;
    ToastListener& operator=(const ToastListener&) = delete;

    void Start(HWND notifyWindow);
    void Stop();

    // 主线程：收到 kMsgToast 后反复调用，取完返回 false
    bool TryTake(ToastRecord& out);

private:
    void RunLoop();

    HWND hwnd_ = nullptr;
    std::atomic<bool> running_{ false };
    std::thread thread_;
    StopSignal  stop_;
    std::vector<uint64_t> seenIds_;   // 启动前的历史通知不算新

    std::mutex             queueM_;
    std::deque<ToastRecord> queue_;   // 待主线程取走的新通知
};

} // namespace fi
