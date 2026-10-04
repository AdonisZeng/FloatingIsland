#include "NotificationCenter.h"

#include "ClipboardMonitor.h"
#include "../config/ConfigStore.h"
#include "../util/Logger.h"

namespace fi {

namespace {
constexpr ULONGLONG kToastMs  = 5000;   // Toast 展示时长
constexpr ULONGLONG kClipMs   = 3000;   // 剪贴板面板展示时长
}

NotificationCenter::NotificationCenter() = default;
NotificationCenter::~NotificationCenter() { Stop(); }

void NotificationCenter::Start(HWND hwnd) {
    toastListener_ = std::make_unique<ToastListener>();
    toastListener_->Start(hwnd);

    clip_ = std::make_unique<ClipboardMonitor>();
    clip_->Start(hwnd, [this](std::wstring url) { OnClipboardUrl(std::move(url)); });
}

void NotificationCenter::Stop() {
    LOGI(L"通知中心停止");
    if (clip_) clip_->Stop();
    if (toastListener_) toastListener_->Stop();   // 必须显式停止：std::thread joinable 析构会 terminate
    toastListener_.reset();
    clip_.reset();
}

void NotificationCenter::Tick() {
    const ULONGLONG now = ::GetTickCount64();
    {
        std::lock_guard lk(m_);
        if (toastExpire_ > 0 && now > toastExpire_) {
            toastView_ = ToastView{};
            toastExpire_ = 0;
        }
        if (clipExpire_ > 0 && now > clipExpire_) {
            clipUrl_.clear();
            clipExpire_ = 0;
        }
    }
}

void NotificationCenter::OnToast(const ToastRecord& rec) {
    if (!ConfigStore::Get().Data().showToast) return;
    // 正文是用户内容：只记长度，不记明文
    LOGI(L"Toast 展示 len(app)=%zu len(title)=%zu len(body)=%zu mode=%d",
         rec.appName.size(), rec.title.size(), rec.body.size(),
         ConfigStore::Get().Data().toastContentMode);
    std::lock_guard lk(m_);
    toastView_.valid = true;
    toastView_.appName = rec.appName;
    toastView_.title = rec.title;
    toastView_.body = rec.body;
    toastView_.arriveTick = ::GetTickCount64();
    toastView_.mode = ConfigStore::Get().Data().toastContentMode;
    toastExpire_ = toastView_.arriveTick + kToastMs;
}

bool NotificationCenter::TakeToast(ToastRecord& out) {
    if (!toastListener_) return false;
    return toastListener_->TryTake(out);
}

void NotificationCenter::OnClipboardUpdate() {
    if (clip_) clip_->OnClipboardUpdate();
}

void NotificationCenter::OnClipboardUrl(const std::wstring& url) {
    // URL 可能含隐私内容：只记长度
    LOGI(L"剪贴板链接入列 len=%zu", url.size());
    std::lock_guard lk(m_);
    clipUrl_ = url;
    clipExpire_ = ::GetTickCount64() + kClipMs;
}

bool NotificationCenter::TryGetToast(ToastView& out) const {
    std::lock_guard lk(m_);
    if (!toastView_.valid) return false;
    out = toastView_;
    return true;
}

ContentLevel NotificationCenter::Level() const {
    std::lock_guard lk(m_);
    if (toastView_.valid) return ContentLevel::Toast;
    if (!clipUrl_.empty()) return ContentLevel::Clipboard;
    return ContentLevel::Media;
}

bool NotificationCenter::TakeClipboardUrl(std::wstring& out) {
    std::lock_guard lk(m_);
    if (clipUrl_.empty()) return false;
    out = clipUrl_;
    clipUrl_.clear();
    clipExpire_ = 0;
    return true;
}

ULONGLONG NotificationCenter::ClipboardExpireTick() const {
    std::lock_guard lk(m_);
    return clipExpire_;
}

} // namespace fi

