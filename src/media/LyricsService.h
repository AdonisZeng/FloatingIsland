#pragma once
// 歌词/封面异步编排：换轨 -> 后台线程跑引擎链（LyricsFetcher）-> 结果经窗口
// 消息回主线程。同一时间只有最新请求有效（旧结果按 token 丢弃）。
//
// 消息：kMsgReady（wParam = token）—— 主线程收到后取走歌词快照与封面字节。
// 封面字节由主线程用 ImageLoader 解码（D2D 资源必须在渲染线程创建）。
//
// 生命周期：Start() 起一条常驻工作线程，任务队列只保留最新一条请求；
// 析构时请求停止并 join，若网络请求仍在飞则最多等 kStopWaitMs 后 detach。
// 线程只持有 std::shared_ptr<Job>，绝不引用 this，因此延迟退出也是安全的。

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "LyricsFetcher.h"
#include "MediaController.h"

namespace fi {

class LyricsService {
public:
    static constexpr UINT kMsgReady = WM_APP + 20;   // wParam = token

    LyricsService() = default;
    ~LyricsService();
    LyricsService(const LyricsService&)            = delete;
    LyricsService& operator=(const LyricsService&) = delete;

    void Start(HWND notifyWindow);

    // 主线程：换轨触发（视频模式只清空不抓取）
    void OnTrackChanged(const MediaSnapshot& snap);

    // 主线程：收到 kMsgReady 后取结果
    bool ConsumeResult(LyricData& outLyric, std::vector<uint8_t>& outCover);

    const LyricData& Data() const { return data_; }

private:
    // 结果块：独立于 LyricsService 生命周期，供工作线程安全写入
    struct Job {
        std::atomic<uint64_t> token{ 0 };   // 当前有效请求代次
        std::mutex            m;
        LyricData             lyric;
        std::vector<uint8_t>  cover;
        bool                  hasResult = false;
        uint64_t              doneToken = 0;
        HWND                  hwnd      = nullptr;
    };

    struct Request {
        uint64_t      token         = 0;
        MediaSnapshot snap;
        double        durationSec    = 0.0;
        bool          preferNetease  = false;
    };

    void WorkerMain(std::shared_ptr<Job> job);

    HWND hwnd_ = nullptr;
    std::atomic<uint64_t> token_{ 0 };

    std::shared_ptr<Job> job_ = std::make_shared<Job>();

    std::mutex              qm_;
    std::condition_variable cv_;
    std::optional<Request>  pending_;
    std::atomic<bool>       quit_{ false };

    std::thread       worker_;
    std::future<void> done_;   // 退出时限时等待工作线程收尾

    LyricData data_;   // 主线程可见快照
};

} // namespace fi
