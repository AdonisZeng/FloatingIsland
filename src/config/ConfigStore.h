#pragma once
// 配置持久化：注册表 HKCU\Software\FloatingIsland。
// 模式：Load() 把全部键值读进内存结构 Config；设置变更改 Data() 后调 Save() 全量写回。
// 全量写回而非逐项同步，避免"改了 A 忘了存 B"的漂移；注册表写入开销可忽略。
//
// 键名与语义对齐 NotchPeninsula（Theme/ToastContentMode/AutoHide* 等），
// 便于两边行为对照；浮点数以 REG_SZ 存储（人类可读，精度足够）。

#include <string>
#include <vector>

namespace fi {

// 岛上内容的显示次序表（内置模块统一编排，顺序即 ShowOrder 里的先后）
enum class IslandModule {
    Media,        // 封面 + 歌名/歌手 + 控制
    Lyrics,       // 歌词 / 卡拉OK
    Spectrum,     // 频谱柱
    Clock,        // 时间日期
    CpuMem,       // CPU / 内存
};

struct Config {
    // —— 通用 ——
    int         theme          = 0;     // 0 深 / 1 浅 / 2 跟随系统
    float       bgOpacity      = 0.95f; // 岛体背景不透明度 0.3~1.0
    std::wstring fontFamily;            // 空 = 默认 Segoe UI Variable Display
    float       dpiScale       = 0.0f;  // 0 = 跟随系统，否则全局缩放系数
    int         targetMonitor  = 0;     // 0 = 主屏，1..N = 枚举序号
    bool        topMost        = true;
    bool        autoStart      = false; // 镜像 HKCU\...\CurrentVersion\Run
    bool        autoUpdate     = true;

    // —— 显示内容 ——
    std::vector<IslandModule> showOrder = {
        IslandModule::Media, IslandModule::Spectrum,
        IslandModule::Clock, IslandModule::CpuMem,
    };
    bool showMedia     = true;
    bool showLyrics    = true;
    bool showSpectrum  = true;
    bool showClock     = true;
    bool showCpuMem    = true;
    bool showToast     = true;
    bool showClipboard = true;
    int  toastContentMode = 0;  // 0 缩略 / 1 紧凑 / 2 完整

    // —— 媒体 ——
    std::wstring musicPlatforms;  // 空 = 不过滤；逗号分隔的来源名（spotify,kugou...）
    int          lyricsDelayMs    = 0;     // 歌词整体偏移（正=延后）
    std::wstring soundName;                // 空 = 不播提示音
    float        soundVolume      = 0.8f;  // 提示音独立音量 0~1
    bool         justSoloEnabled  = true;

    // —— 交互 ——
    bool autoHideEnabled     = true;
    bool hideOnFocusLeave    = true;
    bool hideOnPause         = false;
    bool hideOnFullscreen    = true;
    bool clickThrough        = false;   // 鼠标穿透
    bool dblClickSwitchApp   = true;    // 双击封面切回播放应用
};

class ConfigStore {
public:
    static ConfigStore& Get();

    ConfigStore(const ConfigStore&)            = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    Config& Data() { return cfg_; }

    // 读注册表 -> Data()；键缺失时保留默认值。首次运行即产生默认配置。
    void Load();
    // Data() -> 注册表（全量写回）
    void Save();

private:
    ConfigStore() = default;

    std::wstring ReadString (const wchar_t* name, const std::wstring& def) const;
    int          ReadInt    (const wchar_t* name, int def) const;
    float        ReadFloat  (const wchar_t* name, float def) const;
    bool         ReadBool   (const wchar_t* name, bool def) const;
    void         WriteString(const wchar_t* name, const std::wstring& value);
    void         WriteInt   (const wchar_t* name, int value);
    void         WriteFloat (const wchar_t* name, float value);
    void         WriteBool  (const wchar_t* name, bool value);

    Config cfg_;
};

} // namespace fi
