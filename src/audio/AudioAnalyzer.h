#pragma once
// WASAPI Loopback 采集 + 5 频段分析（对照参照 AudioAnalyzer）。
// 采集线程跟随系统默认输出设备；失败/设备切换由看门狗限速重建，永不退出。
// 分析：降采样后按 80/250/600/1500/3500Hz 做 Goertzel，高频补偿；
//       AGC 包络平滑（attack 瞬压 / release 每帧 x0.98，底噪红线 0.02）。
// 结果写入 5 组电平快照，渲染线程只读快照（互斥保护，无锁读可行但保持简单）。

#include <windows.h>

#include <array>
#include <atomic>
#include <mutex>
#include <thread>

#include "../util/StopSignal.h"

namespace fi {

constexpr int kBandCount = 5;

class AudioAnalyzer {
public:
    AudioAnalyzer() = default;
    ~AudioAnalyzer();
    AudioAnalyzer(const AudioAnalyzer&)            = delete;
    AudioAnalyzer& operator=(const AudioAnalyzer&) = delete;

    void Start();
    void Stop();

    // 渲染线程读取：0~1 电平
    void Levels(float out[kBandCount]) const;

    // Just Solo 12 频段 -> 5 组（服务端在线时优先使用）
    void SetExternalBands(const float* bands12, bool valid);

private:
    void CaptureLoop();
    void Analyze(const float* samples, size_t count, double srcRate);

    std::thread       thread_;
    std::atomic<bool> running_{ false };
    StopSignal        stop_;

    mutable std::mutex m_;
    std::array<float, kBandCount> levels_{};
    std::array<float, kBandCount> ext_{};    // 外部（Just Solo）频段
    bool     extValid_ = false;
    double   gain_     = 1.0;
    double   envelope_ = 0.0;
    ULONGLONG lastFrame_ = 0;
};

} // namespace fi
