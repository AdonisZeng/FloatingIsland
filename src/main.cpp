// FloatingIsland - Stage 1
//
// 已验证（Stage 0）：
//   * DComp 透明合成 + 预乘 alpha（默认 WS_EX_NOREDIRECTIONBITMAP，可 --mode=layered 对比）
//   * 真圆角矩形 SDF + fwidth 抗锯齿
//   * WM_NCHITTEST 与着色器同源 SDF 的像素级穿透
//   * GSMTC 未打包进程可用（本机 Win11 实测，无需 sparse MSIX）
// Stage 1 新增：
//   * HLSL 外置 + fxc 离线编译（去 D3DCOMPILER_47.dll 依赖）
//   * D2D/DirectWrite 内容层：展开态显示真实歌名 / 艺术家（灰度 AA）
//   * GSMTC 常驻监听：切歌自动刷新
//
// 操作：鼠标移入展开 / 移出收起；左键固定展开；右键退出。

#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <shellapi.h>
#include <d2d1_3.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdarg>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "renderer.h"
#include "sdf.h"
#include "config/ConfigStore.h"
#include "util/Logger.h"
#include "util/Singleton.h"
#include "media/gsmtc_probe.h"
#include "media/MediaController.h"
#include "media/ImageLoader.h"
#include "media/LyricsService.h"
#include "media/JustSoloClient.h"
#include "audio/AudioAnalyzer.h"
#include "notify/NotificationCenter.h"
#include "system/SysInfo.h"

namespace {

constexpr wchar_t kClassName[] = L"FloatingIslandHost";
constexpr UINT    kMsgWake     = WM_APP + 1;
constexpr UINT    kMsgTray     = WM_APP + 2;   // 托盘图标回调消息

// 视口（DIP）。窗口尺寸恒定，形状只在视口内变化 -> 永不 resize，永不闪烁
constexpr int   kViewDipW      = 720;
constexpr int   kViewDipH      = 240;
constexpr float kTopMarginDip  = 10.0f;
constexpr float kRimStrength   = 0.08f;   // 边缘高光（旧 0.20 太生硬）

// 形状状态（DIP，半宽/半高/圆角）：收缩态为标准胶囊（圆角 == 半高），
// 展开态为固定小圆角的圆角矩形
constexpr float kCollapsedHalfW = 100.0f;
constexpr float kCollapsedHalfH = 16.0f;
constexpr float kCollapsedCornerR = 16.0f;   // == halfH -> 半球端头
constexpr float kExpandedHalfW  = 260.0f;
constexpr float kExpandedHalfH  = 60.0f;
constexpr float kExpandedCornerR = 24.0f;    // 平边 + 均匀小圆角

enum class TransparencyMode { NoRedirectBitmap, Layered };

struct Layout { int x = 0, y = 0, w = 0, h = 0; };

struct App {
    HWND             hwnd     = nullptr;
    fi::Renderer     renderer;
    TransparencyMode mode     = TransparencyMode::NoRedirectBitmap;

    UINT  dpi       = 96;
    float scale     = 1.0f;
    int   viewPxW   = 0;
    int   viewPxH   = 0;

    // 当前形状（DIP，半宽/半高/圆角）
    float halfW     = kCollapsedHalfW;
    float halfH     = kCollapsedHalfH;
    float cornerR   = kCollapsedCornerR;

    // 最近一帧的实际参数（物理像素），命中判定直接复用，保证与画面完全一致
    fi::ShapeFrame frame;

    bool expanded   = false;
    bool hovered    = false;
    bool pinned     = false;
    bool ready      = false;

    // 媒体部件命中区（DIP）：渲染与点击共用同一组矩形（防漂移纪律）
    D2D1_RECT_F rCover    {};
    D2D1_RECT_F rTitle    {};
    D2D1_RECT_F rArtist   {};
    D2D1_RECT_F rProgress {};
    D2D1_RECT_F rSysInfo   {};   // 时间 / CPU / 内存
    D2D1_RECT_F rClipPanel {};   // 剪贴板链接面板（可点击）
    D2D1_RECT_F rPrev     {};
    D2D1_RECT_F rPlay     {};
    D2D1_RECT_F rNext     {};
    int  hoverBtn  = 0;   // 0 无 / 1 prev / 2 play / 3 next

    ULONGLONG lastTick = 0;
    float     timeSec  = 0.0f;

    ULONGLONG lastRenderErrTick = 0;   // 渲染失败日志与设备重建的节流时间戳
};

App g_app;
std::unique_ptr<fi::MediaController> g_ctrl;
std::unique_ptr<fi::LyricsService>   g_lyrics;
std::unique_ptr<fi::JustSoloClient>  g_solo;
std::unique_ptr<fi::AudioAnalyzer>   g_audio;
std::unique_ptr<fi::NotificationCenter> g_notify;
Microsoft::WRL::ComPtr<ID2D1Bitmap1> g_cover;   // 当前封面（网络抓取后填充）

void Wake() {
    if (g_app.hwnd) ::PostMessageW(g_app.hwnd, kMsgWake, 0, 0);
}

// 屏幕像素 -> 视口 DIP（内容层 transform = Scale(scale)，原点在窗口左上角）
inline float PtToDipX(int screenX) {
    RECT rc{};
    ::GetWindowRect(g_app.hwnd, &rc);
    return static_cast<float>(screenX - rc.left) / g_app.scale;
}
inline float PtToDipY(int screenY) {
    RECT rc{};
    ::GetWindowRect(g_app.hwnd, &rc);
    return static_cast<float>(screenY - rc.top) / g_app.scale;
}
inline bool InRect(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

std::wstring FmtTime(double sec) {
    // 时间轴拿不到 LastUpdatedTime 时位置可能算出天文数字，
    // double -> int 越界是 UB，必须先夹紧
    if (!std::isfinite(sec) || sec < 0) sec = 0;
    if (sec > 59999.0) sec = 59999.0;
    wchar_t buf[16];
    swprintf_s(buf, L"%d:%02d", static_cast<int>(sec) / 60, static_cast<int>(sec) % 60);
    return buf;
}

// 封面字节 -> D2D 位图（必须在渲染线程；借 Begin/End 拿 ctx）
void RefreshCoverFromBytes(const std::vector<uint8_t>& bytes) {
    ID2D1DeviceContext* ctx = g_app.renderer.BeginContentDraw();
    if (!ctx) return;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bmp;
    const HRESULT hr = fi::ImageLoader::DecodeToBitmap(
        ctx, bytes.data(), bytes.size(), bmp.GetAddressOf());
    if (SUCCEEDED(hr)) {
        g_cover = std::move(bmp);
        LOGI(L"封面已解码 bytes=%zu", bytes.size());
    } else {
        LOGW(L"封面解码失败 0x%08lX bytes=%zu",
             static_cast<unsigned long>(hr), bytes.size());
    }
    g_app.renderer.EndContentDraw();
}

// 托盘图标：挂件没有任务栏入口，托盘是用户感知"程序在运行"与退出的唯一入口
NOTIFYICONDATAW g_tray{};

void AddTrayIcon(HWND hwnd) {
    g_tray               = {};
    g_tray.cbSize        = sizeof(g_tray);
    g_tray.hWnd          = hwnd;
    g_tray.uID           = 1;
    g_tray.uFlags        = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = kMsgTray;
    g_tray.hIcon         = ::LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_tray.szTip, L"FloatingIsland - 左键固定展开，右键菜单");
    ::Shell_NotifyIconW(NIM_ADD, &g_tray);
}

void RemoveTrayIcon() {
    if (g_tray.cbSize) {
        ::Shell_NotifyIconW(NIM_DELETE, &g_tray);
        g_tray.cbSize = 0;
    }
}

Layout ComputeLayout(UINT dpi) {
    const float scale = static_cast<float>(dpi) / 96.0f;
    Layout l;
    l.w = static_cast<int>(lroundf(kViewDipW * scale));
    l.h = static_cast<int>(lroundf(kViewDipH * scale));

    MONITORINFO mi{ sizeof(mi) };
    HMONITOR mon = ::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    ::GetMonitorInfoW(mon, &mi);

    const int monitorW = mi.rcMonitor.right - mi.rcMonitor.left;
    l.x = mi.rcMonitor.left + (monitorW - l.w) / 2;
    l.y = mi.rcMonitor.top + static_cast<int>(lroundf(4.0f * scale));
    return l;
}

// —— 媒体部件：渲染与命中共用同一组矩形（App::r*），改这里必须同步 WndProc ——
void DrawMediaWidget(ID2D1DeviceContext* ctx, bool /*open*/) {
    const fi::MediaSnapshot& ms = g_ctrl->Snapshot();
    // 展开过渡中的渐显
    const float fade = (std::min)(1.0f, (std::max)(0.0f, (g_app.halfH - 34.0f) / 20.0f));
    if (fade <= 0.0f) return;

    auto* R = &g_app.renderer;

    // 命中区（DIP，展开态）
    g_app.rCover    = D2D1::RectF(110, 34, 182, 106);
    g_app.rTitle    = D2D1::RectF(196, 34, 452, 58);
    g_app.rArtist   = D2D1::RectF(196, 60, 452, 78);
    g_app.rProgress = D2D1::RectF(196, 86, 452, 94);
    g_app.rPrev     = D2D1::RectF(496, 55, 526, 85);
    g_app.rPlay     = D2D1::RectF(534, 51, 570, 89);
    g_app.rNext     = D2D1::RectF(578, 55, 608, 85);

    // 封面位
    ctx->FillRoundedRectangle(D2D1::RoundedRect(g_app.rCover, 8.0f, 8.0f),
                              R->HoverBrush());
    if (g_cover) {
        ctx->PushAxisAlignedClip(g_app.rCover, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        ctx->DrawBitmap(g_cover.Get(), g_app.rCover, fade,
                        D2D1_INTERPOLATION_MODE_LINEAR);
        ctx->PopAxisAlignedClip();
    } else {
        const wchar_t note = L'\uE8D6';   // MusicInfo
        const D2D1_RECT_F rn = D2D1::RectF(110, 40, 182, 100);
        ctx->DrawTextW(&note, 1, R->TitleFormat(), rn, R->IconBrush());
    }
    // [MEDIA-B]
    // 歌名 / 歌手（视频模式 artist 为空，仅标题）
    if (!ms.title.empty()) {
        R->TitleBrush()->SetOpacity(fade);
        ctx->DrawTextW(ms.title.c_str(), (UINT32)ms.title.size(),
                       R->TitleFormat(), g_app.rTitle, R->TitleBrush());
    }
    if (!ms.artist.empty()) {
        R->ArtistBrush()->SetOpacity(fade);
        ctx->DrawTextW(ms.artist.c_str(), (UINT32)ms.artist.size(),
                       R->ArtistFormat(), g_app.rArtist, R->ArtistBrush());
    }

    // —— 歌词行（Just Solo 优先，其次网络歌词；卡拉OK渐进填色）——
    std::wstring ltext, ltrans;
    float lprog = 0.0f;
    bool hasLyric = false;
    if (g_solo && g_solo->Connected()) {
        hasLyric = g_solo->TryGetCurrentLyric(ltext, ltrans, lprog);
    }
    if (!hasLyric && g_lyrics) {
        const fi::LyricData& d = g_lyrics->Data();
        if (d.valid && !d.lines.empty()) {
            const double pos = ms.positionSec +
                fi::ConfigStore::Get().Data().lyricsDelayMs / 1000.0;
            size_t idx = d.lines.size();
            for (size_t i = 0; i < d.lines.size(); ++i) {
                if (d.lines[i].t <= pos) idx = i; else break;
            }
            if (idx < d.lines.size()) {
                ltext = d.lines[idx].text;
                ltrans = d.translation[idx].text;
                const double t0 = d.lines[idx].t;
                const double t1 = (idx + 1 < d.lines.size())
                    ? d.lines[idx + 1].t : t0 + 4.0;
                lprog = t1 > t0
                    ? static_cast<float>((std::min)(1.0, (std::max)(0.0,
                          (pos - t0) / (t1 - t0))))
                    : 0.0f;
                hasLyric = true;
            }
        }
    }
    if (hasLyric && !ltext.empty()) {
        const D2D1_RECT_F rl = D2D1::RectF(196, 58, 508, 84);
        const float w = R->MeasureTextWidth(ltext.c_str(),
                                            (UINT32)ltext.size(), R->TitleFormat());
        // 底色整行 + 按 progress 裁剪的高亮填充
        R->SubTextBrush()->SetOpacity(fade * 0.9f);
        ctx->DrawTextW(ltext.c_str(), (UINT32)ltext.size(),
                       R->TitleFormat(), rl, R->SubTextBrush());
        if (lprog > 0.01f && lprog < 0.99f && w > 0.0f) {
            const D2D1_RECT_F clip = D2D1::RectF(
                rl.left, rl.top, rl.left + w * lprog, rl.bottom);
            ctx->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
            R->TitleBrush()->SetOpacity(fade);
            ctx->DrawTextW(ltext.c_str(), (UINT32)ltext.size(),
                           R->TitleFormat(), rl, R->TitleBrush());
            ctx->PopAxisAlignedClip();
        } else if (lprog >= 0.99f) {
            R->TitleBrush()->SetOpacity(fade);
            ctx->DrawTextW(ltext.c_str(), (UINT32)ltext.size(),
                           R->TitleFormat(), rl, R->TitleBrush());
        }
        if (!ltrans.empty()) {
            const D2D1_RECT_F rt = D2D1::RectF(196, 76, 508, 92);
            R->SubTextBrush()->SetOpacity(fade);
            ctx->DrawTextW(ltrans.c_str(), (UINT32)ltrans.size(),
                           R->ArtistFormat(), rt, R->SubTextBrush());
        }
    }

    // 进度条 + 时间
    if (ms.durationSec > 0.5) {
        ctx->FillRoundedRectangle(
            D2D1::RoundedRect(g_app.rProgress, 3.0f, 3.0f), R->HoverBrush());
        const float w = g_app.rProgress.right - g_app.rProgress.left;
        const double ratio = (std::min)(1.0, (std::max)(0.0,
            ms.positionSec / ms.durationSec));
        if (ratio > 0.003) {
            const D2D1_RECT_F fill = D2D1::RectF(
                g_app.rProgress.left, g_app.rProgress.top,
                g_app.rProgress.left + static_cast<float>(w * ratio),
                g_app.rProgress.bottom);
            R->AccentBrush()->SetOpacity(fade);
            ctx->FillRoundedRectangle(D2D1::RoundedRect(fill, 3.0f, 3.0f),
                                      R->AccentBrush());
        }
        const std::wstring t = FmtTime(ms.positionSec) + L" / " +
                               FmtTime(ms.durationSec);
        const D2D1_RECT_F rt = D2D1::RectF(456, 82, 512, 98);
        R->SubTextBrush()->SetOpacity(fade);
        ctx->DrawTextW(t.c_str(), (UINT32)t.size(), R->TimeFormat(),
                       rt, R->SubTextBrush());
    }

    // 控制按钮：prev / play-pause / next
    struct Btn { D2D1_RECT_F rect; int id; wchar_t glyph; bool enabled; bool big; };
    const Btn btns[] = {
        { g_app.rPrev, 1, L'\uE892', ms.canPrev, false },
        { g_app.rPlay, 2, ms.isPlaying ? L'\uE769' : L'\uE768', true, true },
        { g_app.rNext, 3, L'\uE893', ms.canNext, false },
    };
    for (const Btn& b : btns) {
        if (g_app.hoverBtn == b.id) {
            R->HoverBrush()->SetOpacity(fade);
            ctx->FillRoundedRectangle(D2D1::RoundedRect(b.rect, 9.0f, 9.0f),
                                      R->HoverBrush());
        }
        R->IconBrush()->SetOpacity(fade * (b.enabled ? 1.0f : 0.3f));
        ctx->DrawTextW(&b.glyph, 1,
                       b.big ? R->TitleFormat() : R->IconFormat(),
                       b.rect, R->IconBrush());
    }
    // 复位画刷透明度
    R->TitleBrush()->SetOpacity(1.0f);
    R->ArtistBrush()->SetOpacity(1.0f);
    R->AccentBrush()->SetOpacity(1.0f);
    R->SubTextBrush()->SetOpacity(1.0f);
    R->IconBrush()->SetOpacity(1.0f);
    R->HoverBrush()->SetOpacity(1.0f);

    // —— 频谱柱（5 频段；Just Solo 在线时用其 12 频段压缩值）——
    if (g_audio) {
        if (g_solo) {
            std::array<float, 12> bands{};
            const bool ext = g_solo->TryGetSpectrum(bands);
            g_audio->SetExternalBands(bands.data(), ext);
        }
        float lv[fi::kBandCount] = {};
        g_audio->Levels(lv);

        const float baseY   = 112.0f;          // 柱底
        const float maxH    = 26.0f;
        const float barW    = 5.0f;
        const float gap     = 4.0f;
        const float startX  = 196.0f;
        for (int b = 0; b < fi::kBandCount; ++b) {
            const float h = (std::max)(2.0f, lv[b] * maxH);
            const D2D1_RECT_F bar = D2D1::RectF(
                startX + b * (barW + gap), baseY - h,
                startX + b * (barW + gap) + barW, baseY);
            R->AccentBrush()->SetOpacity(fade * (0.45f + 0.55f * lv[b]));
            ctx->FillRoundedRectangle(D2D1::RoundedRect(bar, 2.0f, 2.0f),
                                      R->AccentBrush());
        }
        R->AccentBrush()->SetOpacity(1.0f);
    }
}

// —— 通知 / 剪贴板 / 系统信息（显示级别：Toast > 剪贴板 > 媒体）——
void DrawNotifyAndSysInfo(ID2D1DeviceContext* ctx) {
    if (!g_notify) return;
    auto* R = &g_app.renderer;
    const fi::Config& cfg = fi::ConfigStore::Get().Data();

    // 系统信息行（时间 · CPU · 内存）
    if (cfg.showClock || cfg.showCpuMem) {
        g_app.rSysInfo = D2D1::RectF(496, 96, 608, 118);
        const std::wstring info = fi::SysInfo::TimeText() + L"  " +
            std::to_wstring((int)fi::SysInfo::CpuUsagePercent()) + L"%  " +
            std::to_wstring((int)fi::SysInfo::MemoryUsagePercent()) + L"%";
        R->SubTextBrush()->SetOpacity(0.95f);
        ctx->DrawTextW(info.c_str(), (UINT32)info.size(), R->TimeFormat(),
                       g_app.rSysInfo, R->SubTextBrush());
        R->SubTextBrush()->SetOpacity(1.0f);
    }

    fi::ToastView tv;
    if (g_notify->TryGetToast(tv)) {
        // 三档呈现：0 缩略（仅应用名+标题单行） / 1 紧凑（双行） / 2 完整（加正文）
        const bool twoLine = (tv.mode >= 1);
        const bool withBody = (tv.mode >= 2);
        const D2D1_RECT_F rIcon = D2D1::RectF(500, 40, 528, 68);
        R->IconBrush()->SetOpacity(0.9f);
        ctx->DrawTextW(L"\uE7F4", 1, R->TitleFormat(), rIcon, R->IconBrush()); // 消息气泡
        R->IconBrush()->SetOpacity(1.0f);

        std::wstring line1 = tv.appName;
        if (!tv.title.empty()) line1 += L"  " + tv.title;
        const D2D1_RECT_F r1 = D2D1::RectF(534.0f, twoLine ? 32.0f : 44.0f,
                                          620.0f, 56.0f);
        R->TitleBrush()->SetOpacity(1.0f);
        ctx->DrawTextW(line1.c_str(), (UINT32)line1.size(),
                       R->ArtistFormat(), r1, R->TitleBrush());

        if (twoLine && !tv.body.empty()) {
            const D2D1_RECT_F r2 = D2D1::RectF(534, 56, 620, 76);
            R->ArtistBrush()->SetOpacity(0.85f);
            ctx->DrawTextW(tv.body.c_str(), (UINT32)tv.body.size(),
                           R->ArtistFormat(), r2, R->ArtistBrush());
            R->ArtistBrush()->SetOpacity(1.0f);
        }
        (void)withBody;
        return;   // Toast 独占展示区
    }

    // 剪贴板链接面板
    if (g_notify->Level() == fi::ContentLevel::Clipboard) {
        g_app.rClipPanel = D2D1::RectF(470, 40, 620, 76);
        R->HoverBrush()->SetOpacity(0.18f);
        ctx->FillRoundedRectangle(
            D2D1::RoundedRect(g_app.rClipPanel, 8.0f, 8.0f), R->HoverBrush());
        R->HoverBrush()->SetOpacity(1.0f);
        const wchar_t link = L'\uE8A7';   // 超链接
        const D2D1_RECT_F rIcon = D2D1::RectF(476, 48, 498, 70);
        R->AccentBrush()->SetOpacity(1.0f);
        ctx->DrawTextW(&link, 1, R->IconFormat(), rIcon, R->AccentBrush());
        R->AccentBrush()->SetOpacity(1.0f);
    }
}

// 目标值随状态切换；Stage 3 将替换为弹簧动力学
void Tick() {
    const ULONGLONG now = ::GetTickCount64();
    float dt = static_cast<float>(now - g_app.lastTick) / 1000.0f;
    g_app.lastTick = now;
    dt = (std::min)(dt, 0.05f);
    g_app.timeSec += dt;

    const bool open = g_app.expanded || g_app.pinned;
    const float tgtHalfW = open ? kExpandedHalfW  : kCollapsedHalfW;
    const float tgtHalfH = open ? kExpandedHalfH  : kCollapsedHalfH;
    const float tgtCornerR = open ? kExpandedCornerR : kCollapsedCornerR;

    const float k = 1.0f - std::exp(-dt * 14.0f);
    g_app.halfW += (tgtHalfW - g_app.halfW) * k;
    g_app.halfH += (tgtHalfH - g_app.halfH) * k;
    g_app.cornerR += (tgtCornerR - g_app.cornerR) * k;

    // 顶边固定，向下生长
    const float centerYDip = kTopMarginDip + g_app.halfH;

    fi::ShapeFrame& f = g_app.frame;
    f.capsuleHalfW   = g_app.halfW * g_app.scale;
    f.capsuleHalfH   = g_app.halfH * g_app.scale;
    f.cornerRadius   = g_app.cornerR * g_app.scale;
    f.capsuleCenterY = (centerYDip - static_cast<float>(kViewDipH) * 0.5f) * g_app.scale;
    f.rimStrength    = kRimStrength;

    // 1) 内容层：D2D 画内容，SDF mask 负责裁剪
    if (ID2D1DeviceContext* ctx = g_app.renderer.BeginContentDraw()) {
        ctx->Clear();
        ctx->SetTransform(D2D1::Matrix3x2F::Scale(g_app.scale, g_app.scale));

        if (open && g_ctrl) {
            DrawMediaWidget(ctx, open);
        }
        if (open && g_notify) {
            DrawNotifyAndSysInfo(ctx);
        }
        g_app.renderer.EndContentDraw();
    }

    // 2) 形状合成：SDF mask + 内容 -> 预乘 alpha -> DComp
    const HRESULT rhr = g_app.renderer.RenderFrame(f, g_app.timeSec);
    if (FAILED(rhr)) {
        // 设备丢失后每帧都会失败：按秒节流，避免 60 行/秒灌日志并驱动一次重建
        const ULONGLONG errNow = ::GetTickCount64();
        if (errNow - g_app.lastRenderErrTick > 1000) {
            g_app.lastRenderErrTick = errNow;
            LOGE(L"RenderFrame 失败 0x%08lX", static_cast<unsigned long>(rhr));
            if (g_app.renderer.DeviceLost()) {
                if (SUCCEEDED(g_app.renderer.Recover())) {
                    LOGI(L"渲染管线已重建");
                } else {
                    LOGW(L"渲染管线重建失败，稍后重试");
                }
            }
        }
    }
}

bool NeedsFrame() {
    const bool open = g_app.expanded || g_app.pinned;
    const float tgtHalfW = open ? kExpandedHalfW  : kCollapsedHalfW;
    const float tgtHalfH = open ? kExpandedHalfH  : kCollapsedHalfH;
    const float tgtCornerR = open ? kExpandedCornerR : kCollapsedCornerR;

    const bool settling =
        std::fabs(tgtHalfW - g_app.halfW) > 0.05f ||
        std::fabs(tgtHalfH - g_app.halfH) > 0.05f ||
        std::fabs(tgtCornerR - g_app.cornerR) > 0.05f;

    // 展开态内容（进度/频谱/歌词）持续变化；收起并收敛后彻底停帧（CPU/GPU 归零）
    return settling || open;
}

// 与着色器同源的命中判定：d <= 0 视为命中
bool HitTestIsland(int screenX, int screenY) {
    if (!g_app.ready) return false;

    RECT rc{};
    ::GetWindowRect(g_app.hwnd, &rc);

    const float px = static_cast<float>(screenX - rc.left);
    const float py = static_cast<float>(screenY - rc.top);

    const fi::ShapeFrame& f = g_app.frame;
    const float originX = static_cast<float>(g_app.viewPxW) * 0.5f;
    const float originY = static_cast<float>(g_app.viewPxH) * 0.5f + f.capsuleCenterY;

    const float d = fi::sdRoundBox(px - originX, py - originY,
                                   f.capsuleHalfW, f.capsuleHalfH, f.cornerRadius);

    return d <= 0.0f;
}

// 托盘 / 岛体共用右键菜单（需要前台窗口才能在点击外部时正确关闭）
void ShowIslandMenu(HWND hwnd, POINT pt) {
    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING | (g_app.pinned ? MF_CHECKED : MF_UNCHECKED),
                  1, L"固定展开");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, 2, L"退出");
    ::SetForegroundWindow(hwnd);
    const int cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                     pt.x, pt.y, 0, hwnd, nullptr);
    ::DestroyMenu(menu);
    ::PostMessageW(hwnd, WM_NULL, 0, 0);
    if (cmd == 1) {
        g_app.pinned = !g_app.pinned;
        LOGI(L"菜单：固定展开 -> %s", g_app.pinned ? L"true" : L"false");
        Wake();
    } else if (cmd == 2) {
        LOGI(L"菜单：退出");
        ::PostQuitMessage(0);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case kMsgTray:
        if (lp == WM_LBUTTONUP) {
            g_app.pinned = !g_app.pinned;
            LOGI(L"托盘左键：固定展开 -> %s", g_app.pinned ? L"true" : L"false");
            Wake();
        } else if (lp == WM_RBUTTONUP) {
            POINT pt{};
            ::GetCursorPos(&pt);
            ShowIslandMenu(hwnd, pt);
        }
        return 0;

    case WM_TIMER:
        if (wp == 1) {
            if (g_ctrl) g_ctrl->Tick();
            if (g_notify) g_notify->Tick();
            Wake();
        }
        return 0;

    case WM_CLIPBOARDUPDATE:
        if (g_notify) g_notify->OnClipboardUpdate();
        Wake();
        return 0;

    case fi::ToastListener::kMsgToast: {
        // 消息本身不带负载：从监听器的内部队列排空，窗口销毁也不会泄漏记录
        fi::ToastRecord rec;
        bool any = false;
        if (g_notify) {
            while (g_notify->TakeToast(rec)) {
                LOGI(L"收到 Toast：id=%llu len(app)=%zu len(title)=%zu len(body)=%zu",
                     static_cast<unsigned long long>(rec.id), rec.appName.size(),
                     rec.title.size(), rec.body.size());
                g_notify->OnToast(rec);
                any = true;
            }
        }
        if (any) Wake();
        return 0;
    }

    case fi::LyricsService::kMsgReady: {
        // 换轨抓取完成：取歌词快照 + 解码封面（渲染线程）
        fi::LyricData data;
        std::vector<uint8_t> coverBytes;
        if (g_lyrics && g_lyrics->ConsumeResult(data, coverBytes)) {
            LOGI(L"歌词结果已取用：valid=%d 行数=%zu 有译文=%d 封面字节=%zu",
                 data.valid ? 1 : 0, data.lines.size(),
                 data.hasTranslation ? 1 : 0, coverBytes.size());
            if (!coverBytes.empty()) RefreshCoverFromBytes(coverBytes);
            Wake();
        }
        return 0;
    }

    case WM_NCHITTEST: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        // 透明区返回 HTTRANSPARENT：系统把鼠标事件继续投递给下层窗口
        return HitTestIsland(x, y) ? HTCLIENT : HTTRANSPARENT;
    }

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;   // 挂件不该抢焦点

    case WM_MOUSEMOVE:
        if (!g_app.hovered) {
            g_app.hovered = true;
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            ::TrackMouseEvent(&tme);
        }
        g_app.expanded = true;
        if (g_ctrl) {
            const float dx = PtToDipX(GET_X_LPARAM(lp));
            const float dy = PtToDipY(GET_Y_LPARAM(lp));
            int hb = 0;
            if      (InRect(g_app.rPlay, dx, dy)) hb = 2;
            else if (InRect(g_app.rPrev, dx, dy)) hb = 1;
            else if (InRect(g_app.rNext, dx, dy)) hb = 3;
            if (hb != g_app.hoverBtn) { g_app.hoverBtn = hb; Wake(); }
        }
        Wake();
        return 0;

    case WM_MOUSELEAVE:
        g_app.hovered = false;
        g_app.hoverBtn = 0;
        if (!g_app.pinned) g_app.expanded = false;
        Wake();
        return 0;

    case WM_LBUTTONDOWN: {
        const float dx = PtToDipX(GET_X_LPARAM(lp));
        const float dy = PtToDipY(GET_Y_LPARAM(lp));

        // 收起（含收起动画）期间命中矩形仍是上一次展开态的坐标，
        // 不判展开态就会点到看不见的播放/进度条
        if (!(g_app.expanded || g_app.pinned)) {
            g_app.pinned   = !g_app.pinned;
            g_app.expanded = true;
            Wake();
            return 0;
        }

        if (g_ctrl && InRect(g_app.rPlay, dx, dy)) {
            LOGI(L"点击：播放/暂停（当前 %s）",
                 g_ctrl->Snapshot().isPlaying ? L"播放中" : L"已暂停");
            g_ctrl->TogglePlayPause();
        } else if (g_ctrl && InRect(g_app.rPrev, dx, dy)) {
            LOGI(L"点击：上一首");
            g_ctrl->Previous();
        } else if (g_ctrl && InRect(g_app.rNext, dx, dy)) {
            LOGI(L"点击：下一首");
            g_ctrl->Next();
        } else if (g_notify && InRect(g_app.rClipPanel, dx, dy)) {
            std::wstring url;
            if (g_notify->TakeClipboardUrl(url)) {
                const std::wstring target =
                    (url.compare(0, 4, L"www.") == 0) ? L"https://" + url : url;
                const HINSTANCE r = ::ShellExecuteW(nullptr, L"open", target.c_str(),
                                                    nullptr, nullptr, SW_SHOWNORMAL);
                // 返回值 >32 表示成功；链接本身可能是隐私内容，只记长度
                if (reinterpret_cast<INT_PTR>(r) <= 32) {
                    LOGW(L"打开剪贴板链接失败 len=%zu code=%lld",
                         url.size(), static_cast<long long>(reinterpret_cast<INT_PTR>(r)));
                } else {
                    LOGI(L"已打开剪贴板链接 len=%zu", url.size());
                }
            }
            Wake();
        } else if (g_ctrl && InRect(g_app.rProgress, dx, dy) &&
                   g_ctrl->Snapshot().durationSec > 0.5) {
            const D2D1_RECT_F& r = g_app.rProgress;
            const double ratio = (std::min)(1.0, (std::max)(0.0,
                static_cast<double>((dx - r.left) / (r.right - r.left))));
            const double target = ratio * g_ctrl->Snapshot().durationSec;
            LOGI(L"点击：进度条跳转 -> %.1f/%.1f 秒",
                 target, g_ctrl->Snapshot().durationSec);
            g_ctrl->SeekTo(target);
        } else {
            g_app.pinned = !g_app.pinned;
            g_app.expanded = true;
            LOGI(L"点击：固定展开 -> %s", g_app.pinned ? L"true" : L"false");
        }
        Wake();
        return 0;
    }

    case WM_RBUTTONDOWN: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ::ClientToScreen(hwnd, &pt);
        ShowIslandMenu(hwnd, pt);
        return 0;
    }

    case WM_DPICHANGED: {
        const UINT newDpi = static_cast<UINT>(LOWORD(wp));
        LOGI(L"DPI 变化 %u -> %u", g_app.dpi, newDpi);
        g_app.dpi   = newDpi;
        g_app.scale = static_cast<float>(newDpi) / 96.0f;

        const Layout l = ComputeLayout(newDpi);
        ::SetWindowPos(hwnd, HWND_TOPMOST, l.x, l.y, l.w, l.h,
                       SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);

        const HRESULT rzHr = g_app.renderer.Resize(static_cast<UINT>(l.w), static_cast<UINT>(l.h));
        if (FAILED(rzHr)) {
            LOGW(L"Resize 失败 0x%08lX", static_cast<unsigned long>(rzHr));
            // 失败后 widthPx_/heightPx_ 会与实际交换链不一致，设备丢失时整体重建
            if (g_app.renderer.DeviceLost()) {
                if (SUCCEEDED(g_app.renderer.Recover())) LOGI(L"渲染管线已重建");
                else LOGW(L"渲染管线重建失败，稍后重试");
            }
        }
        g_app.viewPxW = l.w;
        g_app.viewPxH = l.h;
        Wake();
        return 0;
    }

    case WM_DISPLAYCHANGE: {
        const Layout l = ComputeLayout(g_app.dpi);
        ::SetWindowPos(hwnd, HWND_TOPMOST, l.x, l.y, 0, 0,
                       SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
        LOGI(L"显示变化，窗口移到 (%d,%d)", l.x, l.y);
        Wake();
        return 0;
    }

    case WM_DESTROY:
        LOGI(L"WM_DESTROY：移除托盘图标并退出");
        RemoveTrayIcon();
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

int RunUi(HINSTANCE instance, TransparencyMode mode) {
    ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = instance;
    wc.lpszClassName = kClassName;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    if (!::RegisterClassW(&wc)) {
        LOGE(L"RegisterClass 失败 GetLastError=%lu", ::GetLastError());
        ::MessageBoxW(nullptr, L"RegisterClass 失败", L"FloatingIsland", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_app.mode = mode;
    g_app.dpi  = ::GetDpiForSystem();
    if (g_app.dpi == 0) g_app.dpi = 96;
    g_app.scale = static_cast<float>(g_app.dpi) / 96.0f;

    Layout layout = ComputeLayout(g_app.dpi);

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    exStyle |= (mode == TransparencyMode::Layered)
                   ? WS_EX_LAYERED                 // 候选 A：分层窗口
                   : WS_EX_NOREDIRECTIONBITMAP;    // 候选 B：无重定向位图（默认）

    HWND hwnd = ::CreateWindowExW(
        exStyle, kClassName, L"FloatingIsland", WS_POPUP,
        layout.x, layout.y, layout.w, layout.h,
        nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        LOGE(L"CreateWindowEx 失败 GetLastError=%lu", ::GetLastError());
        ::MessageBoxW(nullptr, L"CreateWindowEx 失败", L"FloatingIsland", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_app.hwnd = hwnd;
    LOGI(L"窗口已创建 hwnd=0x%p pos=(%d,%d) size=%dx%d",
         hwnd, layout.x, layout.y, layout.w, layout.h);

    // 以窗口真实 DPI 校正一次尺寸（多显示器不同 DPI 时 GetDpiForSystem 可能不准）
    const UINT realDpi = ::GetDpiForWindow(hwnd);
    if (realDpi != 0 && realDpi != g_app.dpi) {
        LOGI(L"DPI 校正 %u -> %u（系统 DPI 与窗口 DPI 不一致）", g_app.dpi, realDpi);
        g_app.dpi   = realDpi;
        g_app.scale = static_cast<float>(realDpi) / 96.0f;
        layout = ComputeLayout(realDpi);
        ::SetWindowPos(hwnd, HWND_TOPMOST, layout.x, layout.y, layout.w, layout.h,
                       SWP_NOACTIVATE | SWP_NOZORDER);
    }

    g_app.viewPxW = layout.w;
    g_app.viewPxH = layout.h;

#ifdef _DEBUG
    const bool debugLayer = true;
#else
    const bool debugLayer = false;
#endif

    HRESULT hr = g_app.renderer.Initialize(hwnd,
                                           static_cast<UINT>(layout.w),
                                           static_cast<UINT>(layout.h),
                                           debugLayer);
    if (FAILED(hr)) {
        LOGE(L"渲染管线初始化失败 0x%08lX debugLayer=%d viewport=%dx%d",
             static_cast<unsigned long>(hr), debugLayer ? 1 : 0, layout.w, layout.h);
        wchar_t buf[128];
        swprintf_s(buf, L"渲染管线初始化失败 (D3D11/DComp)\nHRESULT=0x%08lX",
                   static_cast<unsigned long>(hr));
        ::MessageBoxW(nullptr, buf, L"FloatingIsland", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_app.ready    = true;
    g_app.lastTick = ::GetTickCount64();

    // 首帧必须立即渲染：DComp visual 的交换链在第一次 Present 之前内容未定义，
    // 不渲染的话窗口区域完全透明，屏幕上什么都看不到
    Tick();

    LOGI(L"就绪：mode=%s dpi=%u scale=%.2f viewport=%dx%d",
         (mode == TransparencyMode::Layered) ? L"WS_EX_LAYERED" : L"WS_EX_NOREDIRECTIONBITMAP",
         g_app.dpi, g_app.scale, layout.w, layout.h);

    ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    AddTrayIcon(hwnd);
    LOGI(L"窗口已显示，托盘图标已添加");

    // 媒体控制（GSMTC 全会话记账 + 应用层逻辑；主线程 STA）
    g_ctrl = std::make_unique<fi::MediaController>();
    g_lyrics = std::make_unique<fi::LyricsService>();
    g_solo = std::make_unique<fi::JustSoloClient>();

    g_lyrics->Start(hwnd);
    LOGI(L"歌词服务线程已启动");
    g_ctrl->onChanged = [hwnd] { ::PostMessageW(hwnd, kMsgWake, 0, 0); };
    g_ctrl->onTrackChanged = [](const fi::MediaSnapshot& snap) {
        g_cover.Reset();                       // 旧封面立即失效
        g_lyrics->OnTrackChanged(snap);        // 换轨 -> 引擎链抓取
    };

    const HRESULT mhr = g_ctrl->Start(hwnd);
    if (FAILED(mhr)) {
        LOGE(L"媒体控制启动失败: 0x%08lX", static_cast<unsigned long>(mhr));
    } else {
        LOGI(L"媒体控制已启动（GSMTC 监听 + 200ms 定时器）");
        Wake();
    }

    // Just Solo LyricServer（本地歌词/频谱；连接由后台线程自愈）
    if (fi::ConfigStore::Get().Data().justSoloEnabled) {
        g_solo->Start(hwnd);
        LOGI(L"Just Solo 客户端已启动（127.0.0.1:47290）");
    } else {
        LOGI(L"Just Solo 未启用，跳过");
    }

    // 音频频谱（WASAPI Loopback；未播放时保留音量动态）
    g_audio = std::make_unique<fi::AudioAnalyzer>();
    g_audio->Start();

    // 通知 / 剪贴板 / 系统信息
    g_notify = std::make_unique<fi::NotificationCenter>();
    g_notify->Start(hwnd);
    LOGI(L"通知中心已启动（Toast 监听 + 剪贴板监听）");

    MSG msg{};
    for (;;) {
        if (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            continue;
        }
        if (NeedsFrame()) {
            Tick();
        } else {
            ::WaitMessage();   // 静止时完全让出 CPU
        }
    }

    LOGI(L"消息循环退出 wParam=%llu，开始清理模块",
         static_cast<unsigned long long>(msg.wParam));

    g_ctrl.reset();
    g_notify.reset();
    g_solo.reset();
    g_lyrics.reset();
    g_audio.reset();
    g_app.renderer.Discard();
    LOGI(L"模块已全部释放");
    return static_cast<int>(msg.wParam);
}

int RunMediaProbe() {
    fi::GsmtcProbeResult result;
    const HRESULT hr = fi::ProbeGsmtc(result);
    (void)hr;
    LOGI(L"--probe-media 探测完成");

    const std::wstring report = fi::FormatProbeReport(result);
    ::OutputDebugStringW(report.c_str());
    ::MessageBoxW(nullptr, report.c_str(), L"FloatingIsland - GSMTC 探测",
                  MB_OK | MB_TOPMOST);
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    fi::LogInit();
    LOGI(L"FloatingIsland 启动，日志文件=%s",
         fi::LogFilePath().empty() ? L"(未创建)" : fi::LogFilePath().c_str());

    fi::ConfigStore::Get().Load();

    fi::SingleInstance guard(L"Local\\FloatingIsland_SingleInstance");
    if (!guard.IsPrimary()) {
        LOGW(L"已有实例在运行，退出");
        fi::LogShutdown();
        return 0;
    }

    bool probeMedia = false;
    TransparencyMode mode = TransparencyMode::NoRedirectBitmap;

    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring a = argv[i];
            if (a == L"--probe-media")           probeMedia = true;
            else if (a == L"--mode=layered")     mode = TransparencyMode::Layered;
            else if (a == L"--mode=noredirect")  mode = TransparencyMode::NoRedirectBitmap;
        }
        ::LocalFree(argv);
    }
    LOGI(L"参数解析完成：probeMedia=%d mode=%s",
         probeMedia ? 1 : 0,
         (mode == TransparencyMode::Layered) ? L"layered" : L"noredirect");

    // 所有后台线程此时都已随各模块析构 join，可以安全关闭日志文件
    const int exitCode = probeMedia ? RunMediaProbe() : RunUi(instance, mode);
    LOGI(L"退出码=%d", exitCode);
    fi::LogShutdown();
    return exitCode;
}
