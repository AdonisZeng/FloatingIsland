#pragma once
// GSMTC 常驻监听 + 全会话记账层。
//
// 与参照实现（NotchPeninsula MediaController）对齐的关键行为：
//   * 给系统里【每个】会话挂 PlaybackInfo/Timeline/MediaProperties 监听，
//     任何会话开始播放都立刻重新挑选接管目标（不能只看 CurrentSession）。
//   * 同一 AppID 的会话可能被系统换成新 COM 对象 —— 以接口指针身份记账，
//     指针变化即视为新会话重新挂事件，天然规避该坑。
//   * 接管目标挑选：当前会话仍在播放则保持；否则挑任一播放中的会话；
//     都没播放则保持现有目标（显示暂停态）；无会话则清空。
//
// 线程模型：Start() 在 UI 主线程（STA）调用；事件回调经消息泵回到主线程，
// 全部状态同线程访问，无锁。Stage 0 已验证未打包进程可读 GSMTC。

#include <windows.h>
#include <windows.foundation.h>
#include <windows.media.control.h>

#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fi {

namespace MC = ABI::Windows::Media::Control;

// ABI::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus
// Closed=0 Opened=1 Changing=2 Stopped=3 Playing=4 Paused=5
constexpr int kPlaybackPlaying = 4;
constexpr int kPlaybackPaused  = 5;

struct MediaSessionState {
    bool         valid      = false;
    std::wstring title;
    std::wstring artist;
    std::wstring appId;        // 来源应用 AUMID

    int          status       = 0;   // PlaybackStatus 枚举值
    bool         canPlay      = false;
    bool         canPause     = false;
    bool         canNext      = false;
    bool         canPrev      = false;
    bool         canSeek      = false;

    double       positionSec  = 0.0; // Position@LastUpdatedTime
    double       durationSec  = 0.0; // 0 = 未知
    FILETIME     lastUpdated{};      // 时间轴采样时刻（UTC 100ns）

    bool IsPlaying() const { return status == kPlaybackPlaying; }
};

class MediaSessionMonitor {
public:
    MediaSessionMonitor() = default;
    ~MediaSessionMonitor();
    MediaSessionMonitor(const MediaSessionMonitor&)            = delete;
    MediaSessionMonitor& operator=(const MediaSessionMonitor&) = delete;

    // onChanged 在主线程被回调（用于唤醒渲染）。内部同步初始化，最多阻塞约 3 秒。
    HRESULT Start(std::function<void()> onChanged);

    const MediaSessionState& Current() const { return state_; }

    // —— 播放控制（对当前接管会话）——
    HRESULT TryPlay();
    HRESULT TryPause();
    HRESULT TryNext();
    HRESULT TryPrevious();
    HRESULT TrySeek(double seconds);   // 100ns ticks 位置变更

private:
    struct SessionEntry {
        Microsoft::WRL::ComPtr<MC::IGlobalSystemMediaTransportControlsSession> session;
        ::EventRegistrationToken playbackToken{};
        ::EventRegistrationToken timelineToken{};
        ::EventRegistrationToken propsToken{};
    };

    void OnSessionsChanged();
    void SyncEntries();                  // 全量枚举，增删记账项
    void ReconsiderCurrent();            // 重挑接管目标
    void PullState(bool pullProps);      // 同步拉取 播放状态/控制/时间轴；pullProps 时再异步拉媒体属性
    void PullMediaProperties();          // 异步：标题/歌手
    void OnSessionEvent(void* rawSession, bool propsChanged);
    SessionEntry* FindEntry(void* rawSession);
    void Notify();

    void AttachEvents(SessionEntry& entry);   // 给会话挂三类事件
    static void DetachEvents(SessionEntry& entry);

    Microsoft::WRL::Wrappers::RoInitializeWrapper roInit_{ RO_INIT_SINGLETHREADED };

    Microsoft::WRL::ComPtr<MC::IGlobalSystemMediaTransportControlsSessionManager> manager_;
    std::vector<SessionEntry> entries_;
    MC::IGlobalSystemMediaTransportControlsSession* current_ = nullptr;

    ::EventRegistrationToken sessionsToken_{};
    ::EventRegistrationToken currentToken_{};

    MediaSessionState state_;
    std::function<void()> onChanged_;

    // 存活哨兵：IAsyncOperation 的完成回调可能在 monitor 析构之后才被投递，
    // 回调首行用 weak_ptr 判活，绝不解引用已析构的对象
    struct Alive { std::atomic<bool> ok{ true }; };
    std::shared_ptr<Alive> alive_ = std::make_shared<Alive>();

    // 媒体属性请求代次：只接受最新一代的响应，避免乱序覆盖新曲目标题
    uint64_t propsGen_ = 0;

    // 仅用于日志：会话数量变化时才记一条
    UINT32 lastSessionCount_ = 0xFFFFFFFFu;
};

} // namespace fi
