#pragma once
// Just Solo LyricServer（ws://127.0.0.1:47290）客户端。
// 断线指数退避重连 1s->30s；频谱 500ms 无推送视为过期。

#include <windows.h>
#include <winhttp.h>

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fi {

class JustSoloClient {
public:
    void Start(HWND wakeWindow);
    void Stop();
    bool Connected() const { return connected_; }
    bool TryGetCurrentLyric(std::wstring& text, std::wstring& trans, float& progress) const;
    bool TryGetSpectrum(std::array<float, 12>& bands) const;

    JustSoloClient() = default;
    ~JustSoloClient();
    JustSoloClient(const JustSoloClient&) = delete;
    JustSoloClient& operator=(const JustSoloClient&) = delete;

private:
    void RunLoop();
    void HandleMessage(const std::string& utf8);
    void ResetState();

    HWND hwnd_ = nullptr;
    std::atomic<bool> running_{ false };
    std::atomic<bool> connected_{ false };
    std::thread thread_;
    // 当前 WebSocket 句柄：Stop() 需要关闭它才能打断阻塞中的 Receive
    std::atomic<HINTERNET> socket_{ nullptr };

    mutable std::mutex m_;
    struct Line { int timeMs = 0; std::wstring text, translation; };
    std::vector<Line> lines_;
    bool hasTranslation_ = false;
    int positionMs_ = 0;
    ULONGLONG positionStamp_ = 0;
    bool isPlaying_ = false;
    std::array<float, 12> spectrum_{};
    ULONGLONG spectrumStamp_ = 0;
};

} // namespace fi
