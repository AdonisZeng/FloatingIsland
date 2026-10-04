#include "ClipboardMonitor.h"

#include "../util/Logger.h"

#include <cwctype>

namespace fi {

namespace {
// 允许的 URL 字符：字母数字与 -._~:/?#[]@!$&()*+,;=% 和中文（会被编码交给浏览器）
bool IsUrlChar(wchar_t c) {
    if (c < 0x80) {
        return ::iswalnum(c) || wcschr(L"-._~:/?#[]@!$&()*+,;=%", c) != nullptr;
    }
    return true;   // 非 ASCII 交给 URL 打开逻辑处理
}
}

bool LooksLikeUrl(const std::wstring& text) {
    if (text.size() < 8 || text.size() > 2048) return false;
    // 前后无空白
    if (::iswspace(text.front()) || ::iswspace(text.back())) return false;
    for (wchar_t c : text) {
        if (c == L'\r' || c == L'\n' || c == L'\t') return false;
        if (!IsUrlChar(c)) return false;
    }
    std::wstring lower = text;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(::towlower(c));
    if (lower.compare(0, 7, L"http://") == 0 ||
        lower.compare(0, 8, L"https://") == 0 ||
        lower.compare(0, 6, L"ftp://") == 0 ||
        lower.compare(0, 4, L"www.") == 0) {
        return true;
    }
    return false;
}

void ClipboardMonitor::Start(HWND hwnd, std::function<void(std::wstring)> onUrl) {
    if (registered_) return;   // 幂等：重复 Start 会重复注册，Stop 只注销一次
    hwnd_ = hwnd;
    onUrl_ = std::move(onUrl);
    if (!::AddClipboardFormatListener(hwnd)) {
        LOGW(L"AddClipboardFormatListener 失败 GetLastError=%lu", ::GetLastError());
        return;
    }
    registered_ = true;
    LOGI(L"剪贴板监听已注册");
}

void ClipboardMonitor::Stop() {
    if (hwnd_ && registered_) ::RemoveClipboardFormatListener(hwnd_);
    hwnd_ = nullptr;
    registered_ = false;
}

bool ClipboardMonitor::TakeUrl(std::wstring& out) {
    if (pendingUrl_.empty()) return false;
    out = pendingUrl_;
    pendingUrl_.clear();
    return true;
}

void ClipboardMonitor::OnClipboardUpdate() {
    if (!onUrl_) return;

    // OpenClipboard 偶发被占用，重试 5 次；仍拿不到所有权就必须放弃，
    // 否则后面的 GetClipboardData / CloseClipboard 都是对别人的剪贴板操作
    bool opened = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (::OpenClipboard(hwnd_)) { opened = true; break; }
        ::Sleep(10);
    }
    if (!opened) {
        LOGW(L"剪贴板打开失败（重试 5 次后放弃）");
        return;
    }

    if (!::IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        ::CloseClipboard();
        return;
    }
    HANDLE h = ::GetClipboardData(CF_UNICODETEXT);
    if (!h) {
        ::CloseClipboard();
        return;
    }
    const wchar_t* p = static_cast<const wchar_t*>(::GlobalLock(h));
    std::wstring text;
    if (p) {
        // CF_UNICODETEXT 不保证以 L'\0' 结尾，必须按 GlobalSize 限定长度
        const size_t bytes = ::GlobalSize(h);
        size_t chars = bytes / sizeof(wchar_t);
        size_t len = 0;
        while (len < chars && p[len] != L'\0') ++len;
        text.assign(p, len);
        ::GlobalUnlock(h);
    }
    ::CloseClipboard();

    if (!LooksLikeUrl(text)) return;
    // 只记长度：URL 内容可能是隐私
    LOGI(L"剪贴板检测到链接 len=%zu", text.size());
    pendingUrl_ = text;
    onUrl_(text);
}


} // namespace fi
