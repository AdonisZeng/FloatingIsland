#include "LyricsService.h"

#include "../config/ConfigStore.h"
#include "../util/Logger.h"

#include <utility>

namespace fi {

namespace {

constexpr auto kStopWaitMs = std::chrono::milliseconds(1500);

// 网易云 AUMID 判定（网易系播放器走「网易优先」）
bool IsNeteaseAppId(const std::wstring& appId) {
    std::wstring lower = appId;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(::towlower(c));
    return lower.find(L"netease") != std::wstring::npos ||
           lower.find(L"163music") != std::wstring::npos ||
           lower.find(L"cloudmusic") != std::wstring::npos;
}

} // namespace

LyricsService::~LyricsService() {
    quit_ = true;
    cv_.notify_all();

    if (worker_.joinable()) {
        // 正常情况下请求链有超时，能在等待窗口内结束；万一网络迟迟不返回，
        // 放弃所有权让其自行收尾——线程只持有 shared_ptr<Job>，不触碰 this
        const bool finished = done_.valid() &&
            done_.wait_for(kStopWaitMs) == std::future_status::ready;
        if (finished) {
            worker_.join();
            LOGI(L"歌词线程已结束");
        } else {
            worker_.detach();
            LOGW(L"歌词线程 %lu ms 内未结束，已 detach",
                 static_cast<unsigned long>(kStopWaitMs.count()));
        }
    }
}

void LyricsService::Start(HWND notifyWindow) {
    hwnd_ = notifyWindow;
    job_->hwnd = notifyWindow;

    if (worker_.joinable()) return;
    std::packaged_task<void()> task([this, job = job_] { WorkerMain(job); });
    done_ = task.get_future();
    worker_ = std::thread(std::move(task));
    LOGI(L"歌词工作线程已启动");
}

void LyricsService::OnTrackChanged(const MediaSnapshot& snap) {
    const uint64_t token = ++token_;

    // 旧结果立即作废（换轨后不应再被取用）
    {
        std::lock_guard lk(job_->m);
        job_->hasResult = false;
        job_->lyric     = LyricData{};
        job_->cover.clear();
    }

    if (!snap.valid || !snap.musicMode) {
        LOGI(L"换轨：非音乐模式（valid=%d musicMode=%d），清空歌词",
             snap.valid ? 1 : 0, snap.musicMode ? 1 : 0);
        data_ = LyricData{};
        if (hwnd_) ::PostMessageW(hwnd_, kMsgReady, static_cast<WPARAM>(token), 0);
        return;
    }
    LOGI(L"换轨请求 token=%llu 网易优先=%d len(title)=%zu len(artist)=%zu duration=%.1f",
         static_cast<unsigned long long>(token), IsNeteaseAppId(snap.appId) ? 1 : 0,
         snap.title.size(), snap.artist.size(), snap.durationSec);

    Request req;
    req.token         = token;
    req.snap          = snap;
    req.durationSec   = snap.durationSec;
    req.preferNetease = IsNeteaseAppId(snap.appId);

    job_->token = token;   // 旧请求完成后发现代次不符，自动丢弃

    {
        std::lock_guard lk(qm_);
        pending_ = std::move(req);   // 只保留最新一条请求
    }
    cv_.notify_one();
}

bool LyricsService::ConsumeResult(LyricData& outLyric, std::vector<uint8_t>& outCover) {
    std::lock_guard lk(job_->m);
    if (!job_->hasResult || job_->doneToken != token_.load()) return false;

    job_->hasResult = false;
    outLyric  = job_->lyric;
    outCover  = std::move(job_->cover);
    job_->cover.clear();
    data_ = outLyric;
    return true;
}

void LyricsService::WorkerMain(std::shared_ptr<Job> job) {
    for (;;) {
        Request req;
        {
            std::unique_lock lk(qm_);
            cv_.wait(lk, [this] { return quit_.load() || pending_.has_value(); });
            if (quit_.load()) return;
            if (!pending_) continue;
            req = std::move(*pending_);
            pending_.reset();
        }

        try {
            LyricsFetchResult r = FetchLyricsChain(
                req.snap.title, req.snap.artist, req.durationSec, req.preferNetease);

            std::vector<uint8_t> bytes;
            if (!quit_.load() && !r.coverUrl.empty()) {
                DownloadImageBytes(r.coverUrl, bytes);
            }

            {
                std::lock_guard lk(job->m);
                if (job->token.load() == req.token) {
                    job->lyric     = std::move(r.lyric);
                    job->cover     = std::move(bytes);
                    job->hasResult = true;
                    job->doneToken = req.token;
                    LOGI(L"抓取完成 token=%llu 命中=%d 行数=%zu 有译文=%d 封面字节=%zu",
                         static_cast<unsigned long long>(req.token),
                         job->lyric.valid ? 1 : 0, job->lyric.lines.size(),
                         job->lyric.hasTranslation ? 1 : 0, job->cover.size());
                } else {
                    LOGI(L"抓取结果已过期，丢弃 token=%llu",
                         static_cast<unsigned long long>(req.token));
                }
            }
            if (job->hwnd) {
                ::PostMessageW(job->hwnd, kMsgReady, static_cast<WPARAM>(req.token), 0);
            }
        } catch (...) {
            // 远端 JSON 结构异常等一律吞掉：绝不让异常穿出线程函数（会 terminate）
            LOGW(L"歌词抓取线程异常，忽略本次请求");
        }
    }
}

} // namespace fi
