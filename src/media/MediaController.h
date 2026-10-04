#pragma once
// 媒体控制器：在 MediaSessionMonitor（GSMTC 全会话记账）之上叠加参照实现
// （NotchPeninsula MediaController）的应用层逻辑：
//   * 音乐/视频模式判定 —— 唯一判据：SMTC 同时给出歌名与歌手；
//     配 miss 宽限期（3s），防 seek/换轨瞬间空值导致模式翻转、歌词闪烁
//   * 显示位置自推进（Position + now-LastUpdated），200ms Tick 平滑推进
//   * 歌曲槽位环形缓冲：SMTC 短暂回零时用槽位续显，切平台来回进度不丢
//   * 拖动进度后静默期 800ms，防播放器回报旧位置抖动
//   * 换轨回调（供歌词/封面抓取，键为 稳定歌名|歌手）

#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "media_session.h"

namespace fi {

struct MediaSnapshot {
    bool         valid     = false;
    bool         musicMode = false;
    bool         isPlaying = false;
    std::wstring title;
    std::wstring artist;
    std::wstring appId;
    double       positionSec = 0.0;  // 显示位置（自推进，含槽位续显）
    double       durationSec = 0.0;
    bool         canPlay = false, canPause = false;
    bool         canNext = false, canPrev = false, canSeek = false;
};

class MediaController {
public:
    MediaController() = default;
    ~MediaController();
    MediaController(const MediaController&)            = delete;
    MediaController& operator=(const MediaController&) = delete;

    // 主线程调用；内部启动 monitor 并建 200ms 定时器（WM_TIMER -> Tick）
    HRESULT Start(HWND wakeWindow);

    void Tick();   // 定时器驱动：位置自推进 + 槽位维护 + 模式宽限判定

    const MediaSnapshot& Snapshot() const { return snap_; }

    // —— 播放控制 ——
    void TogglePlayPause();
    void Next();
    void Previous();
    void SeekTo(double seconds);   // 含拖动静默期

    // 状态变化（含换轨）触发；主线程回调
    std::function<void()> onChanged;
    // 换轨（稳定键变化）触发；主线程回调。视频模式 artist 为空
    std::function<void(const MediaSnapshot&)> onTrackChanged;

private:
    struct Slot {
        double    pos      = 0.0;
        double    duration = 0.0;
        ULONGLONG stamp    = 0;
    };

    void UpdateSnapshot();
    void AnnounceTrackIfChanged();

    HWND                            wakeWindow_ = nullptr;
    std::unique_ptr<MediaSessionMonitor> monitor_;

    MediaSnapshot snap_;

    // 模式判定与稳定键
    bool         musicMode_     = false;
    ULONGLONG    musicMissSince_ = 0;   // 音乐模式 miss 宽限起点
    std::wstring stableTitle_, stableArtist_;
    std::wstring announcedKey_;

    // 槽位缓冲（键: appId|title|artist）
    std::wstring currentSlotKey_;
    std::map<std::wstring, Slot> slots_;

    // 会话有效性（仅用于日志：状态翻转时记一条，避免每 200ms 刷日志）
    bool         hadSession_   = false;

    // 自推进
    ULONGLONG lastTick_        = 0;
    double    displayPos_      = 0.0;
    ULONGLONG seekSilenceUntil_ = 0;   // 拖动静默期截止（GetTickCount64）
};

} // namespace fi
