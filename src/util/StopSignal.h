#pragma once
// 后台线程的统一停止协议：置标志 -> 唤醒 -> join。
//
// 背景：std::thread 在 joinable 状态下被析构会直接 std::terminate()，
// 因此每个持有线程的模块都必须有能 join 的析构；而线程内若用 ::Sleep 做
// 退避/帧间隔，单纯 join 会让退出延迟数秒。StopSignal 把「可中断的睡眠」
// 与「停止通知」合一：线程内一律用 SleepOrStop(ms) 替代 ::Sleep(ms)，
// Stop() 里 Notify() 后 join 即可毫秒级退出。

#include <windows.h>

namespace fi {

class StopSignal {
public:
    StopSignal()  { handle_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~StopSignal() { if (handle_) ::CloseHandle(handle_); }

    StopSignal(const StopSignal&)            = delete;
    StopSignal& operator=(const StopSignal&) = delete;

    bool Valid() const noexcept { return handle_ != nullptr; }

    // 请求停止（手动重置事件，置位后所有等待者立即被唤醒）
    void Notify() noexcept { if (handle_) ::SetEvent(handle_); }

    // 线程内替代 ::Sleep(ms)：
    //   返回 true  -> 超时，可继续工作
    //   返回 false -> 已收到停止请求，应立即退出循环
    bool SleepOrStop(DWORD ms) const noexcept {
        if (!handle_) { ::Sleep(ms); return true; }
        return ::WaitForSingleObject(handle_, ms) == WAIT_TIMEOUT;
    }

private:
    HANDLE handle_ = nullptr;
};

} // namespace fi
