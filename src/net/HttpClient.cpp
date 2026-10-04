#include "HttpClient.h"

#include <winhttp.h>

#include "../util/Logger.h"

namespace fi {

#pragma comment(lib, "winhttp.lib")

namespace {
} // namespace

std::wstring HttpResponse::Text() const {
    // UTF-8 -> UTF-16
    const int n = ::MultiByteToWideChar(CP_UTF8, 0,
                                        reinterpret_cast<const char*>(body.data()),
                                        static_cast<int>(body.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    if (n > 0) {
        ::MultiByteToWideChar(CP_UTF8, 0,
                              reinterpret_cast<const char*>(body.data()),
                              static_cast<int>(body.size()), out.data(), n);
    }
    return out;
}

std::string HttpResponse::Utf8() const {
    return { body.begin(), body.end() };
}

HttpResponse HttpClient::Request(const wchar_t* verb,
                                 const std::wstring& url,
                                 const HeaderList& headers,
                                 const std::string& body,
                                 const wchar_t* contentType,
                                 DWORD timeoutMs) {
    HttpResponse resp;

    const ULONGLONG t0 = ::GetTickCount64();

    // 拆 URL
    URL_COMPONENTS uc{};
    uc.dwStructSize      = sizeof(uc);
    uc.dwSchemeLength    = 1;
    uc.dwHostNameLength  = 1;
    uc.dwUrlPathLength   = 1;
    uc.dwExtraInfoLength = 1;
    if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        LOGW(L"URL 解析失败（不记原文，避免隐私泄露）GetLastError=%lu", ::GetLastError());
        return resp;
    }

    const bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) {
        LOGW(L"不支持的 URL scheme：%u", static_cast<unsigned>(uc.nScheme));
        return resp;
    }
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength > 0) {
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/";

    const HINTERNET session = ::WinHttpOpen(kUserAgent,
                                            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                            WINHTTP_NO_PROXY_NAME,
                                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return resp;

    // 必须用 URL 里显式给出的端口：一律走 80/443 会让带端口的地址静默连接失败
    const INTERNET_PORT port = (uc.nPort != 0)
        ? static_cast<INTERNET_PORT>(uc.nPort)
        : (https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);
    const HINTERNET connect = ::WinHttpConnect(session, host.c_str(), port, 0);
    if (!connect) {
        LOGW(L"WinHttpConnect 失败 GetLastError=%lu", ::GetLastError());
        ::WinHttpCloseHandle(session);
        return resp;
    }
    const HINTERNET request =
        connect ? ::WinHttpOpenRequest(connect, verb, path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       https ? WINHTTP_FLAG_SECURE : 0)
                : nullptr;

    do {
        if (!request) break;

        ::WinHttpSetTimeouts(request, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

        // 重定向由 WinHTTP 自动跟随（同 scheme）；跨 scheme 也可跟随
        DWORD flags = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        ::WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY,
                           &flags, sizeof(flags));

        std::wstring headerStr;
        for (const auto& [name, value] : headers) {
            headerStr += name + L": " + value + L"\r\n";
        }
        if (contentType && !body.empty()) {
            headerStr += std::wstring(L"Content-Type: ") + contentType + L"\r\n";
        }

        if (!::WinHttpSendRequest(request,
                                  headerStr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                    : headerStr.c_str(),
                                  static_cast<DWORD>(headerStr.size()),
                                  body.empty() ? WINHTTP_NO_REQUEST_DATA
                                               : reinterpret_cast<LPVOID>(const_cast<char*>(body.data())),
                                  static_cast<DWORD>(body.size()),
                                  static_cast<DWORD>(body.size()), 0)) {
            LOGW(L"WinHttpSendRequest 失败 GetLastError=%lu", ::GetLastError());
            break;
        }
        if (!::WinHttpReceiveResponse(request, nullptr)) {
            LOGW(L"WinHttpReceiveResponse 失败 GetLastError=%lu", ::GetLastError());
            break;
        }

        DWORD status = 0, size = sizeof(status);
        ::WinHttpQueryHeaders(request,
                              WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX,
                              &status, &size, WINHTTP_NO_HEADER_INDEX);
        resp.status = status;

        // 循环读完整 body（上限 8MB 防异常响应）
        constexpr size_t kMaxBody = 8 * 1024 * 1024;
        for (;;) {
            DWORD available = 0;
            if (!::WinHttpQueryDataAvailable(request, &available)) {
                LOGW(L"WinHttpQueryDataAvailable 失败 GetLastError=%lu", ::GetLastError());
                break;
            }
            if (available == 0) break;
            if (resp.body.size() + available > kMaxBody) {
                // 截断必须让调用方知道：否则会拿到半张图去解码，且毫无日志
                resp.truncated = true;
                LOGW(L"响应超过 %u 字节上限，已截断", static_cast<unsigned>(kMaxBody));
                break;
            }

            const size_t before = resp.body.size();
            resp.body.resize(before + available);
            DWORD read = 0;
            if (!::WinHttpReadData(request, resp.body.data() + before,
                                   available, &read)) {
                LOGW(L"WinHttpReadData 失败 GetLastError=%lu", ::GetLastError());
                resp.body.resize(before);
                break;
            }
            resp.body.resize(before + read);
            if (read == 0) break;
        }
    } while (false);

    if (request)  ::WinHttpCloseHandle(request);
    if (connect)  ::WinHttpCloseHandle(connect);
    ::WinHttpCloseHandle(session);

    // 只记主机名与结果统计：完整 URL 可能携带歌名等用户内容
    LOGI(L"HTTP %s %s://%s -> status=%lu bytes=%zu 耗时=%lums%s",
         verb, https ? L"https" : L"http", host.c_str(),
         static_cast<unsigned long>(resp.status), resp.body.size(),
         static_cast<unsigned long>(::GetTickCount64() - t0),
         resp.truncated ? L"（已截断）" : L"");
    return resp;
}

} // namespace fi
