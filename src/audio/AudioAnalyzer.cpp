#include "AudioAnalyzer.h"
#include "../util/Logger.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <vector>

#pragma comment(lib, "mmdevapi.lib")   // IMMDeviceEnumerator / IMMDevice
#pragma comment(lib, "avrt.lib")       // MMCSS（如后续需要提升采集线程优先级）

namespace fi {

using Microsoft::WRL::ComPtr;

namespace {

constexpr double kTargetRate   = 16000.0;   // 分析用采样率
constexpr double kBands[kBandCount] = { 80.0, 250.0, 600.0, 1500.0, 3500.0 };
constexpr float kHighBoost[kBandCount] = { 1.0f, 1.0f, 1.1f, 1.25f, 1.45f };
constexpr double kNoiseFloor = 0.02;        // 底噪红线
constexpr double kRelease    = 0.98;        // 每帧释放系数
constexpr DWORD  kFrameMs    = 50;          // 分析帧长
constexpr DWORD  kRetryMs    = 2000;        // 看门狗重建间隔

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT（不引入 ksmedia.h，避免内核头污染）
constexpr GUID kSubTypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }
};

// 单频段能量（Goertzel）：返回 0~1 归一化幅度的候选值
double Goertzel(const std::vector<float>& x, double rate, double freq) {
    if (x.size() < 8) return 0.0;
    const double w = 2.0 * 3.14159265358979323846 * freq / rate;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (float v : x) {
        const double s0 = v + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return std::sqrt(std::max(0.0, power)) / (x.size() / 2.0);
}

} // namespace

AudioAnalyzer::~AudioAnalyzer() { Stop(); }

void AudioAnalyzer::Start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { CaptureLoop(); });
}

void AudioAnalyzer::Stop() {
    running_ = false;
    stop_.Notify();                    // 打断帧间隔/退避睡眠
    if (thread_.joinable()) thread_.join();
}

void AudioAnalyzer::Levels(float out[kBandCount]) const {
    std::lock_guard lk(m_);
    for (int i = 0; i < kBandCount; ++i) {
        out[i] = extValid_ ? ext_[i] : levels_[i];
    }
}

void AudioAnalyzer::SetExternalBands(const float* bands12, bool valid) {
    if (!valid || !bands12) {
        std::lock_guard lk(m_);
        extValid_ = false;
        return;
    }
    // 12 -> 5 分组压缩：[0,1] [2,3] [4,5,6] [7,8,9] [10,11]
    static const int kGroup[kBandCount][3] = {
        { 0, 1, -1 }, { 2, 3, -1 }, { 4, 5, 6 }, { 7, 8, 9 }, { 10, 11, -1 }
    };
    std::lock_guard lk(m_);
    for (int b = 0; b < kBandCount; ++b) {
        float sum = 0.0f;
        int n = 0;
        for (int k = 0; k < 3; ++k) {
            if (kGroup[b][k] < 0) break;
            sum += bands12[kGroup[b][k]];
            ++n;
        }
        ext_[b] = n > 0 ? sum / n : 0.0f;
    }
    extValid_ = true;
}

void AudioAnalyzer::CaptureLoop() {
    // 采集线程自建 MTA COM
    const HRESULT coHr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coHr)) {
        LOGW(L"音频线程 COM 初始化失败 0x%08lX", static_cast<unsigned long>(coHr));
        return;
    }

    std::vector<float> accum;
    accum.reserve(4096);

    // 初始化失败只在状态翻转时记一条（每 2s 重试，逐次记会灌日志）
    bool failLogged = false;
    auto LogFail = [&](const wchar_t* what) {
        if (failLogged) return;
        failLogged = true;
        LOGW(L"音频采集初始化失败：%s，%lu ms 后重试", what,
             static_cast<unsigned long>(kRetryMs));
    };

    while (running_) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;

        bool ok = false;
        if (SUCCEEDED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(enumerator.GetAddressOf())))) {
            ok = SUCCEEDED(enumerator->GetDefaultAudioEndpoint(
                       eRender, eConsole, device.GetAddressOf()));
        }
        if (!ok) { LogFail(L"枚举/获取默认输出设备"); ::Sleep(kRetryMs); continue; }

        WAVEFORMATEX* pFormat = nullptr;
        // IAudioClient 不是可创建 coclass，必须由 IMMDevice::Activate 取得；
        // 旧写法把 IMMDevice 当成 CoCreateInstance 的 punkOuter，调用必然失败
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(client.GetAddressOf())))) {
            LogFail(L"IAudioClient 激活");
            if (!stop_.SleepOrStop(kRetryMs)) break;
            continue;
        }
        // 轮询式采集（不用事件回调，简化实现）
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_LOOPBACK,
                                      0, 0, 0, nullptr))) {
            LogFail(L"Loopback 流初始化");
            if (!stop_.SleepOrStop(kRetryMs)) break;
            continue;
        }

        if (FAILED(client->GetMixFormat(&pFormat)) || !pFormat) {
            ::CoTaskMemFree(pFormat);
            LogFail(L"获取混音格式");
            if (!stop_.SleepOrStop(kRetryMs)) break;
            continue;
        }

        // 统一转成 float 单声道累积
        const WORD srcCh   = pFormat->nChannels;
        const WORD srcBits  = pFormat->wBitsPerSample;
        const DWORD srcRate = pFormat->nSamplesPerSec;
        // 可扩展格式的真实样本类型在 SubFormat 里：只看 wFormatTag 会把
        // 32bit int PCM 误判成 float（旧实现的两个条件完全相同，恒为真）
        bool isFloat = (pFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        if (pFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pFormat->cbSize >= 22) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pFormat);
            isFloat = ::IsEqualGUID(ext->SubFormat, kSubTypeIeeeFloat);
        }
        if (srcCh == 0 || srcRate == 0) {
            ::CoTaskMemFree(pFormat);
            LogFail(L"混音格式字段异常（声道/采样率为 0）");
            if (!stop_.SleepOrStop(kRetryMs)) break;
            continue;
        }

        ComPtr<IAudioCaptureClient> capture;
        if (FAILED(client->GetService(__uuidof(IAudioCaptureClient),
                                      reinterpret_cast<void**>(capture.GetAddressOf())))) {
            ::CoTaskMemFree(pFormat);
            LogFail(L"IAudioCaptureClient 获取");
            if (!stop_.SleepOrStop(kRetryMs)) break;
            continue;
        }
        if (FAILED(client->Start())) {
            ::CoTaskMemFree(pFormat);
            LogFail(L"采集流启动");
            if (!stop_.SleepOrStop(kRetryMs)) break;
            continue;
        }

        failLogged = false;
        LOGI(L"音频采集就绪 rate=%lu ch=%u bits=%u float=%d",
             static_cast<unsigned long>(srcRate), srcCh, srcBits, isFloat ? 1 : 0);

        bool streaming = true;
        while (running_ && streaming) {
            if (!stop_.SleepOrStop(kFrameMs)) { streaming = false; break; }

            UINT32 avail = 0;
            if (FAILED(capture->GetNextPacketSize(&avail)) || avail == 0) continue;

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                LOGW(L"音频采集中断：GetBuffer 失败，稍后重建采集流");
                streaming = false;
                break;
            }
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                capture->ReleaseBuffer(frames);
                continue;
            }

            // float 源：直接缩放；int 源：按位深归一化
            const size_t sampleCount = static_cast<size_t>(frames) * srcCh;
            const size_t stride = (std::max<size_t>)(1, static_cast<size_t>(srcBits / 8));
            for (size_t i = 0; i < sampleCount; ++i) {
                const BYTE* src = data + i * stride;
                float v = 0.0f;
                if (isFloat && stride == 4) {
                    float f = 0.0f;
                    memcpy(&f, src, sizeof(float));
                    v = f;
                } else if (srcBits == 16) {
                    int16_t s = 0;
                    memcpy(&s, src, sizeof(int16_t));
                    v = s / 32768.0f;
                } else if (srcBits == 24) {
                    // 24bit 打包（3 字节小端）：手动组装并符号扩展
                    int32_t raw = static_cast<int32_t>(src[0]) |
                                  (static_cast<int32_t>(src[1]) << 8) |
                                  (static_cast<int32_t>(src[2]) << 16);
                    if (raw & 0x800000) raw |= ~0xFFFFFF;
                    v = static_cast<float>(raw) / 8388608.0f;
                } else if (srcBits == 32) {
                    int32_t s = 0;
                    memcpy(&s, src, sizeof(int32_t));
                    v = static_cast<float>(s) / 2147483648.0f;
                }
                accum.push_back(v);
            }
            capture->ReleaseBuffer(frames);

            // 降采样累积到目标帧长后分析
            const size_t need = static_cast<size_t>(kTargetRate * kFrameMs / 1000.0);
            if (accum.size() >= need) {
                std::vector<float> frame(accum.begin(), accum.begin() + need);
                accum.erase(accum.begin(), accum.begin() + need);
                Analyze(frame.data(), frame.size(), static_cast<double>(srcRate));
            }
        }

        client->Stop();
        ::CoTaskMemFree(pFormat);
        if (running_ && !stop_.SleepOrStop(kRetryMs)) break;   // 看门狗：限速重建
    }

    ::CoUninitialize();
}

void AudioAnalyzer::Analyze(const float* samples, size_t count, double srcRate) {
    if (!samples || count == 0 || srcRate <= 0.0) return;

    // 抽取降采样到目标采样率
    const double step = srcRate / kTargetRate;
    std::vector<float> x;
    if (step > 1.05) {
        x.reserve(static_cast<size_t>(count / step) + 1);
        for (double pos = 0.0; pos < count; pos += step) {
            x.push_back(samples[static_cast<size_t>(pos)]);
        }
    } else {
        x.assign(samples, samples + count);
    }
    const double rate = kTargetRate;

    double peak = 0.0;
    std::array<double, kBandCount> energy{};
    for (int b = 0; b < kBandCount; ++b) {
        energy[b] = Goertzel(x, rate, kBands[b]);
        peak = std::max(peak, energy[b]);
    }
    if (peak < kNoiseFloor) peak = kNoiseFloor;

    // AGC：包络追踪。attack = 瞬时压制；release = 每帧 x0.98
    const ULONGLONG now = ::GetTickCount64();
    const double dtFrames = lastFrame_ == 0
        ? 1.0 : (std::min)(8.0, (now - lastFrame_) / 16.0 + 0.15);
    lastFrame_ = now;
    if (peak > envelope_) {
        envelope_ = peak;                     // 瞬时跟随
        gain_ = 1.0 / std::max(peak, kNoiseFloor);
    } else {
        envelope_ *= std::pow(kRelease, dtFrames);
        gain_ = 1.0 / std::max(envelope_, kNoiseFloor);
    }

    std::lock_guard lk(m_);
    for (int b = 0; b < kBandCount; ++b) {
        double v = energy[b] * gain_ * kHighBoost[b];
        v = (std::min)(1.0, (std::max)(0.0, v));
        v = std::pow(v, 0.7);                 // 轻微提升低频可见度
        levels_[b] = static_cast<float>(v);
    }
}

} // namespace fi

