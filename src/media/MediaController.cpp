#include "MediaController.h"

#include "../util/Logger.h"

#include <algorithm>
#include <cmath>

namespace fi {

namespace {
constexpr UINT    kTimerId          = 1;
constexpr UINT    kTimerIntervalMs  = 200;
constexpr DWORD   kMusicGraceMs     = 3000;   // 音乐模式 miss 宽限
constexpr DWORD   kSeekSilenceMs    = 800;    // 拖动后静默期
constexpr double  kResyncThreshold  = 0.35;   // SMTC 校正阈值（秒）
constexpr size_t  kMaxSlots         = 16;
constexpr wchar_t kSlotSep          = L'|';
constexpr double  kMaxDisplayPos    = 36000.0;   // 时长未知时的位置上限（10 小时）

std::wstring MakeSlotKey(const std::wstring& appId,
                         const std::wstring& title,
                         const std::wstring& artist) {
    return appId + kSlotSep + title + kSlotSep + artist;
}

double SecondsSince(const FILETIME& then, const FILETIME& now) {
    const ULONGLONG t1 = (static_cast<ULONGLONG>(then.dwHighDateTime) << 32) | then.dwLowDateTime;
    const ULONGLONG t2 = (static_cast<ULONGLONG>(now.dwHighDateTime)  << 32) | now.dwLowDateTime;
    if (t1 == 0) return 0.0;   // 从未采样到时间戳：按全零推算会得到天文数字
    return t2 > t1 ? static_cast<double>(t2 - t1) / 1e7 : 0.0;
}
} // namespace

MediaController::~MediaController() {
    if (wakeWindow_) ::KillTimer(wakeWindow_, kTimerId);
}

HRESULT MediaController::Start(HWND wakeWindow) {
    wakeWindow_ = wakeWindow;

    monitor_ = std::make_unique<MediaSessionMonitor>();
    HRESULT hr = monitor_->Start([this] {
        UpdateSnapshot();
        AnnounceTrackIfChanged();
        if (onChanged) onChanged();
    });
    if (FAILED(hr)) {
        LOGE(L"GSMTC 监听启动失败 0x%08lX", static_cast<unsigned long>(hr));
        monitor_.reset();
        return hr;
    }
    LOGI(L"GSMTC 监听已启动");

    UpdateSnapshot();
    if (!::SetTimer(wakeWindow_, kTimerId, kTimerIntervalMs, nullptr)) {
        // 计时器起不来就没有 Tick()，进度不会自推进：完整回滚而不是留下半初始化状态
        const HRESULT thr = HRESULT_FROM_WIN32(::GetLastError());
        LOGW(L"媒体计时器创建失败 0x%08lX", static_cast<unsigned long>(thr));
        monitor_.reset();
        wakeWindow_ = nullptr;
        return thr;
    }
    lastTick_ = ::GetTickCount64();
    return S_OK;
}

void MediaController::Tick() {
    const ULONGLONG now = ::GetTickCount64();
    double dt = static_cast<double>(now - lastTick_) / 1000.0;
    lastTick_ = now;
    if (dt <= 0 || dt > 1.0) dt = 0.2;

    if (!monitor_) return;
    const MediaSessionState& st = monitor_->Current();

    if (!st.valid) {
        if (hadSession_) {           // 会话消失只记一次，不每 200ms 刷
            hadSession_ = false;
            LOGI(L"媒体会话已消失，媒体区停用");
        }
        displayPos_ = 0.0;
        currentSlotKey_.clear();
        UpdateSnapshot();
        return;
    }
    if (!hadSession_) {
        hadSession_ = true;
        LOGI(L"接管媒体会话 len(appId)=%zu len(title)=%zu",
             st.appId.size(), st.title.size());
    }

    // SMTC 绝对位置推算：Position + 自 LastUpdated 以来的时长（仅播放中）
    FILETIME nowFt{};
    ::GetSystemTimeAsFileTime(&nowFt);
    const double smtcPos = st.positionSec +
        (st.IsPlaying() ? SecondsSince(st.lastUpdated, nowFt) : 0.0);

    const bool silent = now < seekSilenceUntil_;
    if (!silent) {
        if (st.IsPlaying()) {
            // 事件校正：SMTC 推算位置与自推进位置偏差过大时对齐
            if (std::fabs(smtcPos - displayPos_) > kResyncThreshold) {
                displayPos_ = smtcPos;
            } else {
                displayPos_ += dt;
            }
        } else {
            displayPos_ = smtcPos;   // 暂停时跟随
        }
    }
    // 时长未知（durationSec == 0）时也要有上限，否则异常时间戳会让位置失控
    const double maxPos = st.durationSec > 0 ? st.durationSec : kMaxDisplayPos;
    displayPos_ = (std::min)((std::max)(displayPos_, 0.0), maxPos);

    // 槽位维护：当前曲目记录最新位置
    if (!currentSlotKey_.empty()) {
        Slot& s = slots_[currentSlotKey_];
        s.pos = displayPos_;
        s.duration = st.durationSec;
        s.stamp = now;
        // 环形上限：最旧的槽位淘汰
        while (slots_.size() > kMaxSlots) {
            auto oldest = slots_.begin();
            for (auto it = slots_.begin(); it != slots_.end(); ++it) {
                if (it->second.stamp < oldest->second.stamp) oldest = it;
            }
            slots_.erase(oldest);
        }
    }

    UpdateSnapshot();
    if (onChanged && snap_.isPlaying) onChanged();   // 播放中推进 -> 重绘
}

void MediaController::UpdateSnapshot() {
    if (!monitor_) return;
    const MediaSessionState& st = monitor_->Current();

    snap_.valid     = st.valid;
    snap_.title     = st.title;
    snap_.artist    = st.artist;
    snap_.appId     = st.appId;
    snap_.isPlaying = st.IsPlaying();
    snap_.durationSec = st.durationSec;
    snap_.canPlay   = st.canPlay;
    snap_.canPause  = st.canPause;
    snap_.canNext   = st.canNext;
    snap_.canPrev   = st.canPrev;
    snap_.canSeek   = st.canSeek;
    snap_.positionSec = displayPos_;

    // 稳定标题/歌手缓存：SMTC 抖出空值时保持最后已知值
    if (!st.title.empty())  stableTitle_  = st.title;
    if (!st.artist.empty()) stableArtist_ = st.artist;

    const ULONGLONG now = ::GetTickCount64();

    // 模式判定（唯一判据：同时给出歌名与歌手），带 miss 宽限期
    const bool reportedMusic = !st.title.empty() && !st.artist.empty();
    if (reportedMusic) {
        musicMode_ = true;
        musicMissSince_ = 0;
    } else if (st.valid && !st.title.empty()) {
        if (musicMode_ && musicMissSince_ == 0) musicMissSince_ = now;
        if (now - musicMissSince_ > kMusicGraceMs) musicMode_ = false;
    } else {
        musicMode_ = false;
        musicMissSince_ = 0;
    }
    snap_.musicMode = musicMode_ && st.valid;

    // 换轨 -> 切换槽位；SMTC 短暂回零时槽位续显
    const std::wstring key = MakeSlotKey(st.appId, stableTitle_, stableArtist_);
    if (st.valid && key != currentSlotKey_) {
        currentSlotKey_ = key;
        auto it = slots_.find(key);
        if (it != slots_.end() && it->second.duration > 0 &&
            std::fabs(it->second.duration - st.durationSec) < 1.0) {
            displayPos_ = it->second.pos;   // 恢复上次的显示进度
        } else {
            displayPos_ = st.positionSec;
        }
    }
}

void MediaController::AnnounceTrackIfChanged() {
    if (!snap_.valid) {
        if (!announcedKey_.empty()) {
            announcedKey_.clear();
            if (onTrackChanged) onTrackChanged(snap_);   // 通知清空
        }
        return;
    }
    // 通告键用稳定字符串：抖动空值不会造成重复通告
    const std::wstring key = snap_.musicMode
        ? stableTitle_ + L"|" + stableArtist_
        : stableTitle_;
    if (key.empty() || key == announcedKey_) return;

    announcedKey_ = key;
    LOGI(L"换轨: mode=%s len(title)=%zu len(artist)=%zu",
         snap_.musicMode ? L"music" : L"video",
         stableTitle_.size(), stableArtist_.size());
    if (onTrackChanged) onTrackChanged(snap_);
}

void MediaController::TogglePlayPause() {
    if (!monitor_) return;
    const HRESULT hr = snap_.isPlaying ? monitor_->TryPause() : monitor_->TryPlay();
    if (FAILED(hr)) LOGW(L"TogglePlayPause 失败 0x%08lX", static_cast<unsigned long>(hr));
}

void MediaController::Next() {
    if (!monitor_) return;
    const HRESULT hr = monitor_->TryNext();
    if (FAILED(hr)) LOGW(L"下一首失败 0x%08lX", static_cast<unsigned long>(hr));
}

void MediaController::Previous() {
    if (!monitor_) return;
    const HRESULT hr = monitor_->TryPrevious();
    if (FAILED(hr)) LOGW(L"上一首失败 0x%08lX", static_cast<unsigned long>(hr));
}

void MediaController::SeekTo(double seconds) {
    if (!monitor_) return;
    const HRESULT hr = monitor_->TrySeek(seconds);
    if (SUCCEEDED(hr)) {
        displayPos_ = seconds;
        seekSilenceUntil_ = ::GetTickCount64() + kSeekSilenceMs;
        UpdateSnapshot();
        if (onChanged) onChanged();
    } else {
        LOGW(L"跳转失败 0x%08lX（目标 %.1f 秒）",
             static_cast<unsigned long>(hr), seconds);
    }
}

} // namespace fi
