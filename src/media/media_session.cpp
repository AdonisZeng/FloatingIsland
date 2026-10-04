#include "media_session.h"

#include "winrt_helpers.h"
#include "../util/Logger.h"

#include <wrl/implements.h>

#include <windows.foundation.collections.h>

#include <algorithm>

namespace fi {

namespace MC = ABI::Windows::Media::Control;
namespace wf = ABI::Windows::Foundation;
namespace wfc = ABI::Windows::Foundation::Collections;

using ManagerAsyncOp = wf::IAsyncOperation<MC::GlobalSystemMediaTransportControlsSessionManager*>;
using PropsAsyncOp   = wf::IAsyncOperation<MC::GlobalSystemMediaTransportControlsSessionMediaProperties*>;
using BoolAsyncOp    = wf::IAsyncOperation<bool>;

// 播放控制命令为 fire-and-forget：只关心已发起，不消费 bool 结果
class IgnoreResultHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          wf::IAsyncOperationCompletedHandler<bool>> {
public:
    STDMETHODIMP Invoke(wf::IAsyncOperation<bool>*, wf::AsyncStatus) override {
        return S_OK;
    }
};

MediaSessionMonitor::~MediaSessionMonitor() {
    alive_->ok = false;                  // 先让所有在途回调失效
    for (SessionEntry& e : entries_) DetachEvents(e);
    if (manager_) {
        manager_->remove_SessionsChanged(sessionsToken_);
        manager_->remove_CurrentSessionChanged(currentToken_);
    }
}

HRESULT MediaSessionMonitor::Start(std::function<void()> onChanged) {
    onChanged_ = std::move(onChanged);

    if (FAILED(static_cast<HRESULT>(roInit_))) {
        LOGE(L"RoInitialize 失败 0x%08lX", static_cast<unsigned long>(static_cast<HRESULT>(roInit_)));
        return static_cast<HRESULT>(roInit_);
    }

    Microsoft::WRL::Wrappers::HStringReference cls(
        L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager");

    Microsoft::WRL::ComPtr<MC::IGlobalSystemMediaTransportControlsSessionManagerStatics> statics;
    HRESULT hr = ::RoGetActivationFactory(cls.Get(), IID_PPV_ARGS(statics.GetAddressOf()));
    if (FAILED(hr)) {
        LOGE(L"GSMTC SessionManager 激活失败 0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }

    Microsoft::WRL::ComPtr<ManagerAsyncOp> op;
    hr = statics->RequestAsync(op.GetAddressOf());
    if (FAILED(hr)) {
        LOGE(L"GSMTC RequestAsync 失败 0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }

    hr = wrt::AwaitAsync(op.Get(), manager_.GetAddressOf(), 3000);
    if (FAILED(hr) || !manager_) {
        LOGE(L"GSMTC RequestAsync 等待失败/超时 0x%08lX",
             static_cast<unsigned long>(FAILED(hr) ? hr : E_NOINTERFACE));
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }

    using SessionsChangedHandler = wrt::VoidEventHandler<
        wf::ITypedEventHandler<MC::GlobalSystemMediaTransportControlsSessionManager*,
                               MC::SessionsChangedEventArgs*>,
        MC::IGlobalSystemMediaTransportControlsSessionManager*,
        MC::ISessionsChangedEventArgs*>;
    std::weak_ptr<Alive> alive = alive_;
    auto onSessions = Microsoft::WRL::Make<SessionsChangedHandler>(
        [this, alive] { if (auto a = alive.lock()) { if (a->ok) OnSessionsChanged(); } });
    hr = manager_->add_SessionsChanged(onSessions.Get(), &sessionsToken_);
    if (FAILED(hr)) {
        LOGE(L"SessionsChanged 订阅失败 0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }

    using CurrentChangedHandler = wrt::VoidEventHandler<
        wf::ITypedEventHandler<MC::GlobalSystemMediaTransportControlsSessionManager*,
                               MC::CurrentSessionChangedEventArgs*>,
        MC::IGlobalSystemMediaTransportControlsSessionManager*,
        MC::ICurrentSessionChangedEventArgs*>;
    auto onCurrent = Microsoft::WRL::Make<CurrentChangedHandler>(
        [this, alive] { if (auto a = alive.lock()) { if (a->ok) OnSessionsChanged(); } });
    hr = manager_->add_CurrentSessionChanged(onCurrent.Get(), &currentToken_);
    if (FAILED(hr)) {
        LOGE(L"CurrentSessionChanged 订阅失败 0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }

    OnSessionsChanged();
    return S_OK;
}

MediaSessionMonitor::SessionEntry* MediaSessionMonitor::FindEntry(void* rawSession) {
    for (SessionEntry& e : entries_) {
        if (e.session.Get() == rawSession) return &e;
    }
    return nullptr;
}

void MediaSessionMonitor::OnSessionsChanged() {
    SyncEntries();
    ReconsiderCurrent();
}

void MediaSessionMonitor::AttachEvents(SessionEntry& e) {
    MC::IGlobalSystemMediaTransportControlsSession* raw = e.session.Get();
    std::weak_ptr<Alive> alive = alive_;   // 弱引用：对象析构后回调直接返回

    using PlaybackHandler = wrt::VoidEventHandler<
        wf::ITypedEventHandler<MC::GlobalSystemMediaTransportControlsSession*,
                               MC::PlaybackInfoChangedEventArgs*>,
        MC::IGlobalSystemMediaTransportControlsSession*,
        MC::IPlaybackInfoChangedEventArgs*>;
    if (auto h = Microsoft::WRL::Make<PlaybackHandler>(
            [this, raw, alive] {
                if (auto a = alive.lock()) { if (a->ok) OnSessionEvent(raw, false); }
            })) {
        if (FAILED(e.session->add_PlaybackInfoChanged(h.Get(), &e.playbackToken))) {
            e.playbackToken = EventRegistrationToken{};
            LOGW(L"PlaybackInfoChanged 订阅失败");
        }
    }
    using TimelineHandler = wrt::VoidEventHandler<
        wf::ITypedEventHandler<MC::GlobalSystemMediaTransportControlsSession*,
                               MC::TimelinePropertiesChangedEventArgs*>,
        MC::IGlobalSystemMediaTransportControlsSession*,
        MC::ITimelinePropertiesChangedEventArgs*>;
    if (auto h = Microsoft::WRL::Make<TimelineHandler>(
            [this, raw, alive] {
                if (auto a = alive.lock()) { if (a->ok) OnSessionEvent(raw, false); }
            })) {
        if (FAILED(e.session->add_TimelinePropertiesChanged(h.Get(), &e.timelineToken))) {
            e.timelineToken = EventRegistrationToken{};
            LOGW(L"TimelinePropertiesChanged 订阅失败");
        }
    }

    using PropsHandler = wrt::VoidEventHandler<
        wf::ITypedEventHandler<MC::GlobalSystemMediaTransportControlsSession*,
                               MC::MediaPropertiesChangedEventArgs*>,
        MC::IGlobalSystemMediaTransportControlsSession*,
        MC::IMediaPropertiesChangedEventArgs*>;
    if (auto h = Microsoft::WRL::Make<PropsHandler>(
            [this, raw, alive] {
                if (auto a = alive.lock()) { if (a->ok) OnSessionEvent(raw, true); }
            })) {
        if (FAILED(e.session->add_MediaPropertiesChanged(h.Get(), &e.propsToken))) {
            e.propsToken = EventRegistrationToken{};
            LOGW(L"MediaPropertiesChanged 订阅失败");
        }
    }
}

void MediaSessionMonitor::DetachEvents(SessionEntry& e) {
    if (!e.session) return;
    // token 为 0 表示从未订阅成功；注销后清零，保证重复调用幂等
    if (e.playbackToken.value) { e.session->remove_PlaybackInfoChanged(e.playbackToken); e.playbackToken = EventRegistrationToken{}; }
    if (e.timelineToken.value) { e.session->remove_TimelinePropertiesChanged(e.timelineToken); e.timelineToken = EventRegistrationToken{}; }
    if (e.propsToken.value)    { e.session->remove_MediaPropertiesChanged(e.propsToken);    e.propsToken = EventRegistrationToken{}; }
}

void MediaSessionMonitor::SyncEntries() {
    Microsoft::WRL::ComPtr<
        wfc::IVectorView<MC::GlobalSystemMediaTransportControlsSession*>> sessions;
    if (FAILED(manager_->GetSessions(sessions.GetAddressOf())) || !sessions) {
        LOGW(L"GSMTC GetSessions 失败");
        return;
    }

    UINT32 count = 0;
    if (FAILED(sessions->get_Size(&count))) {
        LOGW(L"GSMTC 会话枚举失败（get_Size）");
        return;
    }
    if (count != lastSessionCount_) {
        LOGI(L"GSMTC 会话数 %u", static_cast<unsigned>(count));
        lastSessionCount_ = count;
    }

    std::vector<MC::IGlobalSystemMediaTransportControlsSession*> present;
    present.reserve(count);
    for (UINT32 i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<MC::IGlobalSystemMediaTransportControlsSession> s;
        if (SUCCEEDED(sessions->GetAt(i, s.GetAddressOf())) && s) {
            present.push_back(s.Get());
        }
    }

    // 移除已消失的会话（COM 对象被系统更换 -> 指针不在表内 -> 重新记账）
    for (auto it = entries_.begin(); it != entries_.end();) {
        const bool found = std::find(present.begin(), present.end(),
                                     it->session.Get()) != present.end();
        if (!found) {
            DetachEvents(*it);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }

    // 为新会话挂事件（以接口指针身份判重，规避同 AppID 换 COM 对象的坑）
    for (MC::IGlobalSystemMediaTransportControlsSession* raw : present) {
        if (FindEntry(raw)) continue;
        SessionEntry e;
        e.session = raw;
        AttachEvents(e);
        entries_.push_back(std::move(e));
    }
}

namespace {

int ReadStatusOf(MC::IGlobalSystemMediaTransportControlsSession* s) {
    if (!s) return -1;
    Microsoft::WRL::ComPtr<MC::IGlobalSystemMediaTransportControlsSessionPlaybackInfo> pb;
    if (FAILED(s->GetPlaybackInfo(pb.GetAddressOf())) || !pb) return -1;
    MC::GlobalSystemMediaTransportControlsSessionPlaybackStatus st{};
    if (FAILED(pb->get_PlaybackStatus(&st))) return -1;
    return static_cast<int>(st);
}

} // namespace

void MediaSessionMonitor::ReconsiderCurrent() {
    SessionEntry* cur = current_ ? FindEntry(current_) : nullptr;

    // 当前目标仍在播放 -> 保持
    if (cur && ReadStatusOf(cur->session.Get()) == kPlaybackPlaying) return;

    // 否则挑任一播放中的会话
    SessionEntry* playing = nullptr;
    for (SessionEntry& e : entries_) {
        if (ReadStatusOf(e.session.Get()) == kPlaybackPlaying) { playing = &e; break; }
    }

    SessionEntry* target = playing ? playing
                         : cur     ? cur
                         : entries_.empty() ? nullptr : &entries_.front();
    MC::IGlobalSystemMediaTransportControlsSession* rawTarget =
        target ? target->session.Get() : nullptr;

    if (rawTarget == current_) {
        if (!state_.valid && target) {          // 之前无效，现在有会话了
            state_.valid = true;
            PullState(true);
            Notify();
        } else if (!target && state_.valid) {   // 会话全部消失
            state_ = MediaSessionState{};
            Notify();
        }
        return;
    }

    current_ = rawTarget;
    state_ = MediaSessionState{};
    if (target) {
        state_.valid = true;
        PullState(true);
        LOGI(L"接管目标切换：len(appId)=%zu status=%d len(title)=%zu",
             state_.appId.size(), state_.status, state_.title.size());
    } else {
        LOGI(L"接管目标切换：无可用会话");
    }
    Notify();
}

void MediaSessionMonitor::PullState(bool pullProps) {
    SessionEntry* cur = current_ ? FindEntry(current_) : nullptr;
    if (!cur || !state_.valid) return;

    auto session = cur->session;

    Microsoft::WRL::Wrappers::HString appId;
    if (SUCCEEDED(session->get_SourceAppUserModelId(appId.GetAddressOf()))) {
        state_.appId = wrt::ToWString(appId.Get());
    }

    if (Microsoft::WRL::ComPtr<MC::IGlobalSystemMediaTransportControlsSessionPlaybackInfo> pb;
        SUCCEEDED(session->GetPlaybackInfo(pb.GetAddressOf())) && pb) {
        MC::GlobalSystemMediaTransportControlsSessionPlaybackStatus st{};
        if (SUCCEEDED(pb->get_PlaybackStatus(&st))) state_.status = static_cast<int>(st);

        if (Microsoft::WRL::ComPtr<MC::IGlobalSystemMediaTransportControlsSessionPlaybackControls> ctl;
            SUCCEEDED(pb->get_Controls(ctl.GetAddressOf())) && ctl) {
            boolean b = 0;
            if (SUCCEEDED(ctl->get_IsPlayEnabled(&b)))    state_.canPlay  = b != 0;
            if (SUCCEEDED(ctl->get_IsPauseEnabled(&b)))   state_.canPause = b != 0;
            if (SUCCEEDED(ctl->get_IsNextEnabled(&b)))    state_.canNext  = b != 0;
            if (SUCCEEDED(ctl->get_IsPreviousEnabled(&b))) state_.canPrev = b != 0;
            if (SUCCEEDED(ctl->get_IsPlaybackPositionEnabled(&b))) state_.canSeek = b != 0;
        }
    }

    if (Microsoft::WRL::ComPtr<MC::IGlobalSystemMediaTransportControlsSessionTimelineProperties> tl;
        SUCCEEDED(session->GetTimelineProperties(tl.GetAddressOf())) && tl) {
        wf::TimeSpan pos{}, end{};
        wf::DateTime upd{};
        if (SUCCEEDED(tl->get_Position(&pos))) state_.positionSec = pos.Duration / 1e7;
        if (SUCCEEDED(tl->get_EndTime(&end)))  state_.durationSec = end.Duration / 1e7;
        if (SUCCEEDED(tl->get_LastUpdatedTime(&upd))) {
            ULARGE_INTEGER ul{};
            ul.QuadPart = static_cast<ULONGLONG>(upd.UniversalTime);
            state_.lastUpdated.dwLowDateTime  = ul.LowPart;
            state_.lastUpdated.dwHighDateTime = ul.HighPart;
        } else {
            // 拿不到采样时刻时清零：保留旧值会让进度自推进算出天文数字
            state_.lastUpdated = FILETIME{};
        }
    }

    if (pullProps) PullMediaProperties();
}

void MediaSessionMonitor::PullMediaProperties() {
    SessionEntry* cur = current_ ? FindEntry(current_) : nullptr;
    if (!cur || !state_.valid) return;

    Microsoft::WRL::ComPtr<PropsAsyncOp> op;
    if (FAILED(cur->session->TryGetMediaPropertiesAsync(op.GetAddressOf()))) return;

    // 完成回调经消息泵回主线程：应用前校验对象仍存活、目标未切换、代次为最新
    const uint64_t gen = ++propsGen_;
    MC::IGlobalSystemMediaTransportControlsSession* expected = current_;
    std::weak_ptr<Alive> alive = alive_;
    wrt::ThenAsync<MC::IGlobalSystemMediaTransportControlsSessionMediaProperties*>(
        op.Get(),
        [this, expected, gen, alive](MC::IGlobalSystemMediaTransportControlsSessionMediaProperties* props,
                         HRESULT result) {
            auto guard = alive.lock();
            if (!guard || !guard->ok) return;        // monitor 已析构
            if (FAILED(result) || !props) return;
            if (gen != propsGen_) return;        // 已有更新一代的请求，丢弃旧响应
            if (expected != current_) return;

            Microsoft::WRL::Wrappers::HString title, artist;
            std::wstring t, a;
            if (SUCCEEDED(props->get_Title(title.GetAddressOf())))  t = wrt::ToWString(title.Get());
            if (SUCCEEDED(props->get_Artist(artist.GetAddressOf()))) a = wrt::ToWString(artist.Get());

            if (t == state_.title && a == state_.artist) return;
            state_.title = std::move(t);
            state_.artist = std::move(a);
            Notify();
        });
}

void MediaSessionMonitor::OnSessionEvent(void* rawSession, bool propsChanged) {
    if (rawSession != current_) {
        // 后台会话状态变化（如开始播放）-> 重新挑选接管目标
        ReconsiderCurrent();
        return;
    }
    if (!state_.valid) return;
    // 时间轴事件每秒可达数次，只在属性/接管目标变化时才去拉媒体属性
    PullState(propsChanged);
    Notify();
}

void MediaSessionMonitor::Notify() {
    if (onChanged_) onChanged_();
}

namespace {

HRESULT SendControl(MC::IGlobalSystemMediaTransportControlsSession* s,
                    HRESULT (STDMETHODCALLTYPE MC::IGlobalSystemMediaTransportControlsSession::*op)(
                        wf::IAsyncOperation<bool>**)) {
    if (!s) return E_FAIL;
    Microsoft::WRL::ComPtr<BoolAsyncOp> async;
    HRESULT hr = (s->*op)(async.GetAddressOf());
    if (FAILED(hr)) return hr;
    auto handler = Microsoft::WRL::Make<IgnoreResultHandler>();
    return async ? async->put_Completed(handler.Get()) : E_FAIL;
}

} // namespace

HRESULT MediaSessionMonitor::TryPlay() {
    SessionEntry* cur = current_ ? FindEntry(current_) : nullptr;
    return cur ? SendControl(cur->session.Get(),
                             &MC::IGlobalSystemMediaTransportControlsSession::TryPlayAsync) : E_FAIL;
}

HRESULT MediaSessionMonitor::TryPause() {
    SessionEntry* cur = current_ ? FindEntry(current_) : nullptr;
    return cur ? SendControl(cur->session.Get(),
                             &MC::IGlobalSystemMediaTransportControlsSession::TryPauseAsync) : E_FAIL;
}

HRESULT MediaSessionMonitor::TryNext() {
    SessionEntry* cur = current_ ? FindEntry(current_) : nullptr;
    return cur ? SendControl(cur->session.Get(),
                             &MC::IGlobalSystemMediaTransportControlsSession::TrySkipNextAsync) : E_FAIL;
}

HRESULT MediaSessionMonitor::TryPrevious() {
    SessionEntry* cur = current_ ? FindEntry(current_) : nullptr;
    return cur ? SendControl(cur->session.Get(),
                             &MC::IGlobalSystemMediaTransportControlsSession::TrySkipPreviousAsync) : E_FAIL;
}

HRESULT MediaSessionMonitor::TrySeek(double seconds) {
    SessionEntry* cur = current_ ? FindEntry(current_) : nullptr;
    if (!cur) return E_FAIL;
    Microsoft::WRL::ComPtr<BoolAsyncOp> async;
    HRESULT hr = cur->session->TryChangePlaybackPositionAsync(
        static_cast<INT64>(seconds * 1e7), async.GetAddressOf());
    if (FAILED(hr)) return hr;
    auto handler = Microsoft::WRL::Make<IgnoreResultHandler>();
    return async ? async->put_Completed(handler.Get()) : E_FAIL;
}

} // namespace fi

