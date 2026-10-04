#include "ToastListener.h"

#include "../util/Logger.h"

#include <objbase.h>

#include <winrt/base.h>
// SDK 的 cppwinrt 存在大小写两套 impl 头，完整的那套是小写文件名；
// 先包含它们以免被同名大写头遮蔽（守卫宏同名，先到先得）
#include <winrt/impl/windows.ui.notifications.0.h>
#include <winrt/impl/windows.ui.notifications.management.0.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>

namespace fi {

using namespace winrt::Windows::UI::Notifications;
using namespace winrt::Windows::UI::Notifications::Management;

ToastListener::~ToastListener() { Stop(); }

void ToastListener::Start(HWND notifyWindow) {
    if (running_.exchange(true)) return;
    hwnd_ = notifyWindow;
    thread_ = std::thread([this] { RunLoop(); });
    LOGI(L"Toast 监听线程已启动");
}

void ToastListener::Stop() {
    running_ = false;
    stop_.Notify();                    // 打断线程内的退避/轮询睡眠
    if (thread_.joinable()) thread_.join();
    LOGI(L"Toast 监听线程已停止");
}

bool ToastListener::TryTake(ToastRecord& out) {
    std::lock_guard lk(queueM_);
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void ToastListener::RunLoop() {
    // 独立线程：自己初始化 MTA，阻塞式 get() 不影响渲染线程
    const HRESULT coHr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coHr)) {
        LOGW(L"Toast 线程 COM 初始化失败 0x%08lX", static_cast<unsigned long>(coHr));
        return;
    }


    bool primed = false;
    bool accessDeniedLogged = false;
    int  errorCount = 0;
    while (running_) {
        try {
            auto listener = UserNotificationListener::Current();

            if (listener.GetAccessStatus() !=
                UserNotificationListenerAccessStatus::Allowed) {
                if (!accessDeniedLogged) {
                    accessDeniedLogged = true;
                    LOGW(L"通知监听权限未授权，尝试请求…");
                }
                listener.RequestAccessAsync().get();
                if (listener.GetAccessStatus() !=
                    UserNotificationListenerAccessStatus::Allowed) {
                    if (!stop_.SleepOrStop(3000)) break;
                    continue;
                }
                LOGI(L"通知监听权限已获取");
            }

            auto notes = listener.GetNotificationsAsync(
                NotificationKinds::Toast).get();

            if (!primed) {
                // 首次拿到快照：全部记为已见，之后只报新通知
                for (auto&& n : notes) seenIds_.push_back(n.Id());
                primed = true;
                LOGI(L"通知监听就绪，已有 %zu 条通知记为已见", seenIds_.size());
            } else {
                for (auto&& n : notes) {
                    const uint64_t id = n.Id();
                    bool isNew = true;
                    for (uint64_t seen : seenIds_) {
                        if (seen == id) { isNew = false; break; }
                    }
                    if (!isNew) continue;

                    ToastRecord rec;
                    rec.id = id;
                    // 注：当前 Windows SDK 的 WinRT 通知元数据里 INotification 未投影
                    // Title/Body（OS 运行时实际有），因此这里只能取应用名；
                    // 待 SDK 组件修复后即可补上标题/正文（代码位置已就绪）。
                    try { rec.appName = n.AppInfo().DisplayInfo().DisplayName(); }
                    catch (...) { rec.appName = L"\u901a\u77e5"; }

                    seenIds_.push_back(id);
                    LOGI(L"新通知入队 id=%llu len(app)=%zu",
                         static_cast<unsigned long long>(id), rec.appName.size());
                    {
                        std::lock_guard lk(queueM_);
                        queue_.push_back(std::move(rec));
                    }
                    if (hwnd_) ::PostMessageW(hwnd_, kMsgToast, 0, 0);
                }
                // 只保留最近 64 个 ID，防无限增长
                if (seenIds_.size() > 64) {
                    seenIds_.erase(seenIds_.begin(), seenIds_.begin() + 32);
                }
            }
        } catch (...) {
            // 权限被拒 / API 不可用：退避重试，绝不退出线程
            ++errorCount;
            if (errorCount == 1 || errorCount % 20 == 0) {
                LOGW(L"通知监听循环异常（第 %d 次，已持续退避重试）", errorCount);
            }
            if (!stop_.SleepOrStop(3000)) break;
            continue;
        }
        if (!stop_.SleepOrStop(800)) break;
    }
    ::CoUninitialize();
}

} // namespace fi



