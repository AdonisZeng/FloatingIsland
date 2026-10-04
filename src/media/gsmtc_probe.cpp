#include "gsmtc_probe.h"

#include <windows.h>
#include <appmodel.h>   // GetCurrentPackageFullName / APPMODEL_ERROR_NO_PACKAGE

#include "winrt_helpers.h"

#include <windows.foundation.collections.h>
#include <windows.media.control.h>

#include <wrl/wrappers/corewrappers.h>

#include <cstdio>
#include <string>

namespace fi {
namespace {

namespace MC = ABI::Windows::Media::Control;
namespace wf = ABI::Windows::Foundation;

using ManagerAsync = wf::IAsyncOperation<MC::GlobalSystemMediaTransportControlsSessionManager*>;
using SessionList  = wf::Collections::IVectorView<MC::GlobalSystemMediaTransportControlsSession*>;

// 进程是否有 package identity。
// 只有通过包的启动入口（开始菜单 / shell:AppsFolder）启动的进程才有；
// 直接双击 exe 永远没有 —— 这是 sparse MSIX 的关键限制。
bool HasPackageIdentity(std::wstring* fullName) {
    UINT32 len = 0;
    LONG rc = ::GetCurrentPackageFullName(&len, nullptr);
    if (rc == APPMODEL_ERROR_NO_PACKAGE) return false;
    if (rc != ERROR_INSUFFICIENT_BUFFER || len == 0) return false;

    // 多留一位：该 API 对「长度是否含终止符」的表述有歧义，多一个 wchar_t 零成本
    std::wstring name(static_cast<size_t>(len) + 1, L'\0');
    rc = ::GetCurrentPackageFullName(&len, name.data());
    if (rc != ERROR_SUCCESS) return false;
    name.resize(len > 0 ? len - 1 : 0);
    if (fullName) *fullName = name;
    return true;
}

} // namespace

HRESULT ProbeGsmtc(GsmtcProbeResult& out) {
    Microsoft::WRL::Wrappers::RoInitializeWrapper roinit(RO_INIT_SINGLETHREADED);
    out.comHr = static_cast<HRESULT>(roinit);
    out.comInitialized = SUCCEEDED(out.comHr);
    if (FAILED(out.comHr)) return out.comHr;

    out.hasIdentity = HasPackageIdentity(&out.identity);

    // 注意：SDK 10.0.26100 中该 statics 的方法名是 RequestAsync()，
    // 老版本文档里的 RequestSessionManagerAsync() 已不在头文件中。
    Microsoft::WRL::Wrappers::HStringReference cls(
        L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager");

    Microsoft::WRL::ComPtr<MC::IGlobalSystemMediaTransportControlsSessionManagerStatics> statics;
    out.factoryHr = ::RoGetActivationFactory(cls.Get(), IID_PPV_ARGS(statics.GetAddressOf()));
    out.factoryOk = SUCCEEDED(out.factoryHr);
    if (!out.factoryOk) return out.factoryHr;

    Microsoft::WRL::ComPtr<ManagerAsync> op;
    HRESULT hr = statics->RequestAsync(op.GetAddressOf());
    if (FAILED(hr)) {
        out.managerHr = hr;
        return hr;
    }

    Microsoft::WRL::ComPtr<MC::IGlobalSystemMediaTransportControlsSessionManager> manager;
    out.managerHr = wrt::AwaitAsync(op.Get(), manager.GetAddressOf(), 3000);
    out.managerOk = SUCCEEDED(out.managerHr);
    if (!out.managerOk) return out.managerHr;

    Microsoft::WRL::ComPtr<SessionList> sessions;
    hr = manager->GetSessions(sessions.GetAddressOf());
    if (FAILED(hr)) {
        out.notes += L"GetSessions 失败: " + wrt::HrText(hr) + L"\n";
        return hr;
    }

    unsigned count = 0;
    if (FAILED(sessions->get_Size(&count))) {
        out.notes += L"get_Size 失败\n";
        return E_FAIL;
    }
    out.sessionCount = static_cast<int>(count);

    if (count == 0) {
        // 会话注册存在时序延迟；等 3 秒复查一次再下结论
        ::Sleep(3000);
        sessions.Reset();
        if (SUCCEEDED(manager->GetSessions(sessions.GetAddressOf())) &&
            SUCCEEDED(sessions->get_Size(&count))) {
            out.sessionCount = static_cast<int>(count);
        }
    }

    if (out.sessionCount <= 0) {
        out.notes += L"会话列表为空（已自动重试一次）。\n";
        if (!out.hasIdentity) {
            out.notes += L"本进程当前没有 package identity。\n"
                         L"若确认系统此刻正在播放音乐，这就是 GSMTC 拒绝未打包进程，\n"
                         L"需要注册 sparse MSIX（packaging/register-sparse.ps1），\n"
                         L"且必须从开始菜单启动本程序。";
        } else {
            out.notes += L"进程已有包身份仍拿不到会话：\n"
                         L"1) 确认此刻确实在播放音乐；\n"
                         L"2) 该播放器是否接入了系统媒体控件（SMTC）。";
        }
        return S_OK;
    }

    Microsoft::WRL::ComPtr<MC::IGlobalSystemMediaTransportControlsSession> first;
    if (SUCCEEDED(sessions->GetAt(0, first.GetAddressOf())) && first) {
        Microsoft::WRL::Wrappers::HString appId;
        if (SUCCEEDED(first->get_SourceAppUserModelId(appId.GetAddressOf()))) {
            out.firstAppId = wrt::ToWString(appId.Get());
        }

        Microsoft::WRL::ComPtr<
            wf::IAsyncOperation<MC::GlobalSystemMediaTransportControlsSessionMediaProperties*>> propsOp;
        hr = first->TryGetMediaPropertiesAsync(propsOp.GetAddressOf());
        if (FAILED(hr)) {
            out.notes += L"TryGetMediaPropertiesAsync 失败: " + wrt::HrText(hr) + L"\n";
            return hr;
        }

        Microsoft::WRL::ComPtr<MC::IGlobalSystemMediaTransportControlsSessionMediaProperties> props;
        hr = wrt::AwaitAsync(propsOp.Get(), props.GetAddressOf(), 2500);
        if (FAILED(hr)) {
            out.notes += L"媒体属性异步等待失败: " + wrt::HrText(hr) + L"\n";
            return hr;
        }

        Microsoft::WRL::Wrappers::HString title, artist;
        if (SUCCEEDED(props->get_Title(title.GetAddressOf())))   out.firstTitle  = wrt::ToWString(title.Get());
        if (SUCCEEDED(props->get_Artist(artist.GetAddressOf()))) out.firstArtist = wrt::ToWString(artist.Get());
    }
    return S_OK;
}

std::wstring FormatProbeReport(const GsmtcProbeResult& r) {
    std::wstring s;
    s += L"GSMTC 探测结果\n";
    s += L"==============================\n";
    s += L"包身份         : " + (r.hasIdentity ? (L"有 (" + r.identity + L")") : std::wstring(L"无（未打包）")) + L"\n";
    s += L"WinRT 初始化   : " + std::wstring(r.comInitialized ? L"成功" : L"失败 ") + wrt::HrText(r.comHr) + L"\n";
    s += L"激活工厂       : " + std::wstring(r.factoryOk ? L"成功" : L"失败 ") + wrt::HrText(r.factoryHr) + L"\n";
    s += L"SessionManager : " + std::wstring(r.managerOk ? L"成功" : L"失败 ") + wrt::HrText(r.managerHr) + L"\n";
    s += L"会话数量       : " + (r.sessionCount >= 0 ? std::to_wstring(r.sessionCount) : L"未取得") + L"\n";

    if (!r.firstAppId.empty())  s += L"首个会话 AUMID : " + r.firstAppId + L"\n";
    if (!r.firstTitle.empty())  s += L"标题           : " + r.firstTitle + L"\n";
    if (!r.firstArtist.empty()) s += L"艺术家         : " + r.firstArtist + L"\n";
    if (!r.notes.empty())       s += L"\n" + r.notes + L"\n";

    s += L"\n判定: ";
    if (r.sessionCount > 0)       s += L"可用 - 未打包进程能正常获取系统媒体信息。";
    else if (r.sessionCount == 0) s += r.hasIdentity
        ? L"受限 - 有包身份但无会话，确认是否正在播放。"
        : L"受限 - 需要 sparse MSIX 赋予 package identity。";
    else                          s += L"不可用 - 检查 Windows 版本（需 1809+）。";
    return s;
}

} // namespace fi
