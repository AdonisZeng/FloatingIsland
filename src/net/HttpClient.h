#pragma once
// WinHTTP 同步封装：GET/POST + 自定义头 + 超时。
// 【线程约束】仅在网络工作线程调用，绝不上渲染线程（同步阻塞）。

#include <windows.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fi {

struct HttpResponse {
    DWORD                status = 0;
    std::vector<uint8_t> body;   // 原始字节
    bool                 truncated = false;   // 超过体积上限被截断（不可当完整响应用）

    bool Ok() const { return status == 200 && !body.empty() && !truncated; }
    std::wstring Text() const;             // body 按 UTF-8 解码
    std::string  Utf8() const;             // body 原样（假定 UTF-8）
};

using HeaderList = std::vector<std::pair<std::wstring, std::wstring>>;

class HttpClient {
public:
    static constexpr DWORD kDefaultTimeoutMs = 5000;
    static constexpr wchar_t kUserAgent[] =
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) FloatingIsland/0.2";

    static HttpResponse Request(const wchar_t* verb,
                                const std::wstring& url,
                                const HeaderList& headers = {},
                                const std::string& body = {},
                                const wchar_t* contentType = nullptr,
                                DWORD timeoutMs = kDefaultTimeoutMs);

    static HttpResponse Get(const std::wstring& url,
                            const HeaderList& headers = {},
                            DWORD timeoutMs = kDefaultTimeoutMs) {
        return Request(L"GET", url, headers, {}, nullptr, timeoutMs);
    }

    static HttpResponse Post(const std::wstring& url, const std::string& body,
                             const wchar_t* contentType,
                             const HeaderList& headers = {},
                             DWORD timeoutMs = kDefaultTimeoutMs) {
        return Request(L"POST", url, headers, body, contentType, timeoutMs);
    }
};

} // namespace fi
