#pragma once
// 轻量单实例守卫：命名 Mutex，保证进程级唯一。
// 用法：进入 wWinMain 后先构造，IsPrimary() 为 false 时立即退出。

#include <windows.h>

namespace fi {

class SingleInstance {
public:
    explicit SingleInstance(const wchar_t* mutexName) {
        handle_ = ::CreateMutexW(nullptr, TRUE, mutexName);
        if (!handle_) {
            // 创建失败（沙箱 / 句柄配额）不能等同于"已有实例在跑"：
            // 否则程序会毫无提示地直接退出，表现为双击没反应
            primary_ = true;
            return;
        }
        primary_ = ::GetLastError() != ERROR_ALREADY_EXISTS;
    }

    ~SingleInstance() {
        if (handle_) {
            ::ReleaseMutex(handle_);
            ::CloseHandle(handle_);
        }
    }

    SingleInstance(const SingleInstance&)            = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    bool IsPrimary() const { return primary_; }

private:
    HANDLE handle_  = nullptr;
    bool   primary_ = false;
};

} // namespace fi
