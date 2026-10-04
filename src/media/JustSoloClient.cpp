#include "JustSoloClient.h"
#include "../util/Logger.h"
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <chrono>
#pragma comment(lib, "winhttp.lib")

namespace fi {
namespace json = nlohmann;

namespace {
constexpr DWORD kPort = 47290;
constexpr DWORD kMaxReconnectMs = 30000;
constexpr DWORD kTimeoutMs = 5000;
constexpr ULONGLONG kSpectrumStaleMs = 500;

// JSON 取值一律走安全封装：键存在但类型不符时 value()/get<T>() 会抛
// type_error，穿出线程函数就会 std::terminate
template <typename T>
T JNum(const json::json& j, const char* key, T def) {
    if (!j.is_object()) return def;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return def;
    return it->get<T>();
}

std::string JStr(const json::json& j, const char* key) {
    if (!j.is_object()) return {};
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
        static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
        static_cast<int>(s.size()), out.data(), n);
    return out;
}
} // namespace

JustSoloClient::~JustSoloClient() { Stop(); }

void JustSoloClient::Start(HWND wakeWindow) {
    if (running_.exchange(true)) return;
    hwnd_ = wakeWindow;
    thread_ = std::thread([this] { RunLoop(); });
}

void JustSoloClient::Stop() {
    running_ = false;
    // 关闭 socket 是打断阻塞中 WinHttpWebSocketReceive 的唯一手段，
    // 否则 join() 会一直等到连接超时（表现为退出卡死）
    const HINTERNET sock = socket_.exchange(nullptr);
    if (sock) ::WinHttpCloseHandle(sock);
    if (thread_.joinable()) thread_.join();
}

void JustSoloClient::ResetState() {
    std::lock_guard lk(m_);
    lines_.clear();
    hasTranslation_ = false;
    positionMs_ = 0;
    positionStamp_ = 0;      // 不重置会让重连后按旧时间戳插值，直接跳到末尾
    isPlaying_ = false;
    spectrum_.fill(0.0f);
    spectrumStamp_ = 0;
}

// [P3]

void JustSoloClient::RunLoop() {
    DWORD backoff = 1000;
    while (running_) {
        HINTERNET session = ::WinHttpOpen(L"FloatingIsland",
            WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        HINTERNET connect = session ? ::WinHttpConnect(session, L"127.0.0.1", kPort, 0) : nullptr;
        HINTERNET request = connect ? ::WinHttpOpenRequest(connect, L"GET", L"/",
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            0) : nullptr;   // ws:// 明文：不带 SECURE 标志
        if (request) {
            DWORD enable = TRUE;
            ::WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
                &enable, sizeof(enable));
            // 不设超时的话连接/接收阶段可能无限期挂起
            ::WinHttpSetTimeouts(request, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
        }
        HINTERNET socketHandle = nullptr;
        if (request &&
            ::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            ::WinHttpReceiveResponse(request, nullptr)) {
            socketHandle = ::WinHttpWebSocketCompleteUpgrade(request, 0);
        }

        if (socketHandle) {
            ::WinHttpSetTimeouts(socketHandle, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
            socket_ = socketHandle;   // 供 Stop() 中断阻塞中的 Receive
            connected_ = true;
            backoff = 1000;
            ResetState();
            LOGI(L"Just Solo 已连接 127.0.0.1:%u", static_cast<unsigned>(kPort));
            if (hwnd_) ::PostMessageW(hwnd_, WM_APP + 1, 0, 0);  // 唤醒渲染

            BYTE buffer[8192];
            for (;;) {
                DWORD read = 0;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE type =
                    WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
                if (::WinHttpWebSocketReceive(socketHandle, buffer,
                        sizeof(buffer), &read, &type) != ERROR_SUCCESS) break;
                if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
                if (read > 0) {
                    HandleMessage(std::string(reinterpret_cast<char*>(buffer), read));
                }
            }
            ::WinHttpWebSocketClose(socketHandle, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                nullptr, 0);
            // Close 只发协议层关闭帧，句柄必须单独关闭，否则每轮重连泄漏一个
            socket_.compare_exchange_strong(socketHandle, static_cast<HINTERNET>(nullptr));
            ::WinHttpCloseHandle(socketHandle);
            connected_ = false;
            ResetState();
            LOGI(L"Just Solo 连接断开，退避 %lu ms 后重连", backoff);
            if (hwnd_) ::PostMessageW(hwnd_, WM_APP + 1, 0, 0);
        } else {
            // 服务未启动是常态：只记一条，避免每次退避都刷日志
            static bool firstFailLogged = false;
            if (!firstFailLogged) {
                firstFailLogged = true;
                LOGI(L"Just Solo 服务不可达 127.0.0.1:%u，将自动重试",
                     static_cast<unsigned>(kPort));
            }
        }

        // [P4]
        if (request) ::WinHttpCloseHandle(request);
        if (connect) ::WinHttpCloseHandle(connect);
        if (session) ::WinHttpCloseHandle(session);

        if (!running_) break;
        ::Sleep(backoff);
        backoff = (std::min)(backoff * 2, kMaxReconnectMs);
    }
}

void JustSoloClient::HandleMessage(const std::string& utf8) {
    // 顶层兜底：任何解析/取值异常都不允许穿出线程函数
    try {
    json::json doc = json::json::parse(utf8);
    if (!doc.is_object()) return;
    const std::string type = JStr(doc, "type");
    if (type.empty()) return;

    if (type == "progress") {
        std::lock_guard lk(m_);
        positionMs_ = JNum(doc, "position", 0);
        positionStamp_ = ::GetTickCount64();
    } else if (type == "playback") {
        const std::string st = JStr(doc, "status");
        std::lock_guard lk(m_);
        isPlaying_ = (st == "playing");
        if (!isPlaying_) { spectrum_.fill(0.0f); spectrumStamp_ = 0; }
    } else if (type == "spectrum") {
        if (doc.contains("bands") && doc["bands"].is_array() &&
            doc["bands"].size() >= 12) {
            std::lock_guard lk(m_);
            for (int i = 0; i < 12; ++i) {
                const auto& v = doc["bands"][static_cast<size_t>(i)];
                spectrum_[static_cast<size_t>(i)] = v.is_number() ? v.get<float>() : 0.0f;
            }
            spectrumStamp_ = ::GetTickCount64();
        }
    } else if (type == "lyrics") {
        std::vector<Line> lines;
        bool anyTrans = false;
        if (doc.contains("lyrics") && doc["lyrics"].is_array()) {
            for (const auto& item : doc["lyrics"]) {
                if (!item.is_object()) continue;
                Line ln;
                ln.timeMs = JNum(item, "time", 0);
                ln.text = FromUtf8(JStr(item, "text"));
                ln.translation = FromUtf8(JStr(item, "translation"));
                if (!ln.translation.empty()) anyTrans = true;
                lines.push_back(std::move(ln));
            }
        }
        std::lock_guard lk(m_);
        lines_ = std::move(lines);
        hasTranslation_ = anyTrans;
        const size_t lineCount = lines_.size();
        const bool trans = hasTranslation_;
        LOGI(L"Just Solo 歌词已更新 行数=%zu 有译文=%d", lineCount, trans ? 1 : 0);
    }
    } catch (...) {
        LOGW(L"Just Solo 消息解析失败，已忽略");
    }
}

bool JustSoloClient::TryGetCurrentLyric(std::wstring& text,
                                        std::wstring& trans,
                                        float& progress) const {
    std::lock_guard lk(m_);
    if (!connected_ || lines_.empty()) return false;

    // 位置自推进（服务端 1s 级推送，本地插值）
    const ULONGLONG now = ::GetTickCount64();
    int pos = positionMs_;
    if (isPlaying_ && positionStamp_ > 0) {
        pos += static_cast<int>(now - positionStamp_);
    }

    // 二分找当前行
    size_t idx = lines_.size();
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (lines_[i].timeMs <= pos) idx = i;
        else break;
    }
    if (idx == lines_.size()) { progress = 0.0f; return true; }   // 已连接但暂无当前行

    text = lines_[idx].text;
    trans = lines_[idx].translation;

    const int endMs = (idx + 1 < lines_.size())
        ? lines_[idx + 1].timeMs : lines_[idx].timeMs + 4000;
    const int dur = endMs - lines_[idx].timeMs;
    progress = dur > 0
        ? (std::min)(1.0f, (std::max)(0.0f,
              static_cast<float>(pos - lines_[idx].timeMs) / dur))
        : 0.0f;
    return true;
}

bool JustSoloClient::TryGetSpectrum(std::array<float, 12>& bands) const {
    std::lock_guard lk(m_);
    if (!connected_ || spectrumStamp_ == 0) return false;
    if (::GetTickCount64() - spectrumStamp_ > kSpectrumStaleMs) return false;
    bands = spectrum_;
    return true;
}

} // namespace fi
