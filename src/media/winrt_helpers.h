#pragma once
// WRL + WinRT ABI 共用工具：异步等待/回调、事件 handler、消息泵、HSTRING 转换。
//
// 两条 MIDL 投影规则（踩过坑的）：
//   * IAsyncOperation<T> / ITypedEventHandler<TSender,TArgs> 的模板参数必须填
//     runtime class 指针（头文件里只是前向声明的不完整类型），填接口指针会触发
//     "not_yet_specialized" 静态断言；
//   * GetResults / 事件 sender 的实际写入类型是【默认接口】指针（T_abi 映射），
//     所以结果一律用接口类型的 ComPtr 接收。

#include <windows.h>
#include <windows.foundation.h>

#include <wrl/client.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

#include <functional>
#include <string>
#include <type_traits>

namespace fi {
namespace wrt {

namespace wf = ABI::Windows::Foundation;

inline std::wstring ToWString(HSTRING hstr) {
    if (!hstr) return {};
    UINT32 len = 0;
    PCWSTR buf = ::WindowsGetStringRawBuffer(hstr, &len);
    return buf ? std::wstring(buf, len) : std::wstring{};
}

inline std::wstring HrText(HRESULT hr) {
    wchar_t buf[64];
    swprintf_s(buf, L"0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

// 等待句柄，同时泵送窗口消息（WinRT 异步完成回调在 STA 上依赖消息泵）
//
// 两个必须注意的点：
//   * 用 64 位 tick：GetTickCount 会在 49.7 天后回绕，导致 elapsed 计算异常；
//   * 取到 WM_QUIT 必须重新投递：PM_REMOVE 会把它从队列摘掉，不补回去的话
//     在等待窗口内发起的退出请求会被静默吞掉（表现为关不掉程序）。
inline bool PumpUntil(HANDLE handle, DWORD timeoutMs) {
    const ULONGLONG start = ::GetTickCount64();
    for (;;) {
        const ULONGLONG elapsed = ::GetTickCount64() - start;
        if (elapsed >= timeoutMs) return ::WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;

        const DWORD remain = static_cast<DWORD>(timeoutMs - elapsed);
        const DWORD r = ::MsgWaitForMultipleObjects(1, &handle, FALSE, remain, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0)     return true;
        if (r == WAIT_TIMEOUT)      return false;
        if (r == WAIT_OBJECT_0 + 1) {
            MSG msg{};
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    ::PostQuitMessage(static_cast<int>(msg.wParam));
                    continue;
                }
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
            continue;
        }
        return ::WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
    }
}

// TLogical: async/事件的模板参数（runtime class 指针，可为不完整类型）
// TAbi    : 结果指针的实际类型（默认接口指针）
template <typename TLogical, typename TAbi>
class AsyncHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          wf::IAsyncOperationCompletedHandler<TLogical>> {
public:
    using Callback = std::function<void(TAbi result, HRESULT hr)>;

    AsyncHandler() = default;

    // 事件句柄所有权移交给 handler：AwaitAsync 超时返回后不能再 CloseHandle，
    // 否则 op 稍后完成时会对已关闭（且句柄值可能已被复用）的句柄 SetEvent
    ~AsyncHandler() { if (event_) ::CloseHandle(event_); }

    void AttachEvent(HANDLE h) noexcept { event_ = h; }
    void SetCallback(Callback cb) { cb_ = std::move(cb); }
    HRESULT Status() const noexcept { return hr_; }
    TAbi    Result() const noexcept { return result_.Get(); }

    STDMETHODIMP Invoke(wf::IAsyncOperation<TLogical>* operation, wf::AsyncStatus status) override {
        if (status == wf::AsyncStatus::Completed) {
            hr_ = operation->GetResults(result_.GetAddressOf());
        } else {
            hr_ = E_FAIL;
            Microsoft::WRL::ComPtr<wf::IAsyncInfo> info;
            if (SUCCEEDED(operation->QueryInterface(IID_PPV_ARGS(info.GetAddressOf())))) {
                HRESULT errorCode = S_OK;
                if (SUCCEEDED(info->get_ErrorCode(&errorCode)) && FAILED(errorCode)) {
                    hr_ = errorCode;
                }
            }
        }
        if (cb_) cb_(result_.Get(), hr_);
        if (event_) ::SetEvent(event_);
        return S_OK;
    }

private:
    HANDLE    event_  = nullptr;   // 由 handler 自己关闭
    HRESULT   hr_     = E_PENDING;
    // GetResults 会 AddRef，用 ComPtr 持有：ThenAsync 路径没人取走结果，
    // 裸指针会每次泄漏一个 COM 对象。
    // TAbi 本身是指针类型（默认接口指针），ComPtr 要的是接口类型本身
    Microsoft::WRL::ComPtr<std::remove_pointer_t<TAbi>> result_;
    Callback  cb_;
};

// 同步等待异步操作完成（内部泵消息；调用线程需为 STA 或允许泵消息）
template <typename TAbi, typename TLogical>
HRESULT AwaitAsync(wf::IAsyncOperation<TLogical>* operation, TAbi* out, DWORD timeoutMs) {
    Microsoft::WRL::ComPtr<AsyncHandler<TLogical, TAbi>> handler =
        Microsoft::WRL::Make<AsyncHandler<TLogical, TAbi>>();

    HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return HRESULT_FROM_WIN32(::GetLastError());
    handler->AttachEvent(event);

    HRESULT hr = operation->put_Completed(handler.Get());
    if (SUCCEEDED(hr) && !PumpUntil(event, timeoutMs)) {
        hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    }
    if (SUCCEEDED(hr)) hr = handler->Status();
    if (SUCCEEDED(hr) && out) *out = handler->Result();

    // 句柄归 handler 所有（它可能比本次调用活得更久），这里不能关闭
    return hr;
}

// 回调式异步（不阻塞调用线程），完成回调经消息泵回到调用线程
template <typename TAbi, typename TLogical, typename TFn>
HRESULT ThenAsync(wf::IAsyncOperation<TLogical>* operation, TFn&& fn) {
    Microsoft::WRL::ComPtr<AsyncHandler<TLogical, TAbi>> handler =
        Microsoft::WRL::Make<AsyncHandler<TLogical, TAbi>>();
    handler->SetCallback(std::forward<TFn>(fn));
    return operation->put_Completed(handler.Get());
}

// 事件订阅 handler：不关心 sender/args，只触发一个闭包。
// 模板参数：
//   TEventInterface     : 事件的 handler 接口特化。必须用 runtime class 指针实例化
//                         （如 ITypedEventHandler<RCFoo*, RCBarEventArgs*>），否则触发
//                         not_yet_specialized 静态断言。
//   TSenderAbi/TArgsAbi : Invoke 实参的 ABI 类型（默认接口指针）。SDK 的 Invoke 声明
//                         用的是 GetAbiType 的结果而非模板参数本身，两者不能混。
// 注意：事件回调在注册线程的 STA 泵上触发，可安全访问 UI 状态。
template <typename TEventInterface, typename TSenderAbi, typename TArgsAbi>
class VoidEventHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          TEventInterface> {
public:
    using Callback = std::function<void()>;

    VoidEventHandler() = default;
    // WRL 的 Make<T>(args...) 走构造函数而非 RuntimeClassInitialize
    explicit VoidEventHandler(Callback fn) { fn_ = std::move(fn); }
    HRESULT RuntimeClassInitialize(Callback fn) { fn_ = std::move(fn); return S_OK; }

    STDMETHODIMP Invoke(TSenderAbi, TArgsAbi) override {
        if (fn_) fn_();
        return S_OK;
    }

private:
    Callback fn_;
};

} // namespace wrt
} // namespace fi
