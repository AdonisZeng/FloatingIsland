#include "ConfigStore.h"

#include "../util/Logger.h"

namespace fi {

namespace {

constexpr wchar_t kSubKey[] = L"Software\\FloatingIsland";
constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

std::wstring ModuleToString(IslandModule m) {
    switch (m) {
    case IslandModule::Media:    return L"media";
    case IslandModule::Lyrics:   return L"lyrics";
    case IslandModule::Spectrum: return L"spectrum";
    case IslandModule::Clock:    return L"clock";
    case IslandModule::CpuMem:   return L"cpumem";
    }
    return L"media";
}

IslandModule ModuleFromString(const std::wstring& s) {
    if (s == L"lyrics")   return IslandModule::Lyrics;
    if (s == L"spectrum") return IslandModule::Spectrum;
    if (s == L"clock")    return IslandModule::Clock;
    if (s == L"cpumem")   return IslandModule::CpuMem;
    return IslandModule::Media;
}

} // namespace

ConfigStore& ConfigStore::Get() {
    static ConfigStore store;
    return store;
}

std::wstring ConfigStore::ReadString(const wchar_t* name,
                                     const std::wstring& def) const {
    DWORD type = 0, bytes = 0;
    if (::RegGetValueW(HKEY_CURRENT_USER, kSubKey, name,
                       RRF_RT_REG_SZ, &type, nullptr, &bytes) != ERROR_SUCCESS) {
        return def;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(HKEY_CURRENT_USER, kSubKey, name, RRF_RT_REG_SZ,
                       nullptr, value.data(), &bytes) != ERROR_SUCCESS) {
        return def;
    }
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

int ConfigStore::ReadInt(const wchar_t* name, int def) const {
    DWORD data = 0, bytes = sizeof(data), type = 0;
    if (::RegGetValueW(HKEY_CURRENT_USER, kSubKey, name, RRF_RT_REG_DWORD,
                       &type, &data, &bytes) != ERROR_SUCCESS) {
        return def;
    }
    return static_cast<int>(data);
}

float ConfigStore::ReadFloat(const wchar_t* name, float def) const {
    const std::wstring s = ReadString(name, L"");
    if (s.empty()) return def;
    return static_cast<float>(_wtof(s.c_str()));
}

bool ConfigStore::ReadBool(const wchar_t* name, bool def) const {
    return ReadInt(name, def ? 1 : 0) != 0;
}

void ConfigStore::WriteString(const wchar_t* name, const std::wstring& value) {
    const LSTATUS st = ::RegSetKeyValueW(HKEY_CURRENT_USER, kSubKey, name, REG_SZ,
                                         value.c_str(),
                                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (st != ERROR_SUCCESS) {
        LOGW(L"写入配置失败 %s (code=%ld)", name, static_cast<long>(st));
    }
}

void ConfigStore::WriteInt(const wchar_t* name, int value) {
    const DWORD data = static_cast<DWORD>(value);
    const LSTATUS st = ::RegSetKeyValueW(HKEY_CURRENT_USER, kSubKey, name, REG_DWORD,
                                         &data, sizeof(data));
    if (st != ERROR_SUCCESS) {
        LOGW(L"写入配置失败 %s (code=%ld)", name, static_cast<long>(st));
    }
}

void ConfigStore::WriteFloat(const wchar_t* name, float value) {
    wchar_t buf[32];
    _snwprintf_s(buf, _TRUNCATE, L"%.3f", value);
    WriteString(name, buf);
}

void ConfigStore::WriteBool(const wchar_t* name, bool value) {
    WriteInt(name, value ? 1 : 0);
}

void ConfigStore::Load() {
    cfg_.theme      = ReadInt(L"Theme", cfg_.theme);
    cfg_.bgOpacity  = ReadFloat(L"BgOpacity", cfg_.bgOpacity);
    cfg_.fontFamily = ReadString(L"FontFamily", cfg_.fontFamily);
    cfg_.dpiScale   = ReadFloat(L"DpiScale", cfg_.dpiScale);
    cfg_.targetMonitor = ReadInt(L"TargetMonitor", cfg_.targetMonitor);
    cfg_.topMost    = ReadBool(L"TopMost", cfg_.topMost);
    cfg_.autoStart  = ReadBool(L"AutoStart", cfg_.autoStart);
    cfg_.autoUpdate = ReadBool(L"AutoUpdate", cfg_.autoUpdate);

    const std::wstring order = ReadString(L"ShowOrder", L"");
    if (!order.empty()) {
        std::vector<IslandModule> parsed;
        size_t pos = 0;
        while (pos <= order.size()) {
            const size_t comma = order.find(L',', pos);
            const std::wstring token = comma == std::wstring::npos
                ? order.substr(pos) : order.substr(pos, comma - pos);
            if (!token.empty()) parsed.push_back(ModuleFromString(token));
            if (comma == std::wstring::npos) break;
            pos = comma + 1;
        }
        if (!parsed.empty()) cfg_.showOrder = std::move(parsed);
    }
    cfg_.showMedia        = ReadBool(L"ShowMedia", cfg_.showMedia);
    cfg_.showLyrics       = ReadBool(L"ShowLyrics", cfg_.showLyrics);
    cfg_.showSpectrum     = ReadBool(L"ShowSpectrum", cfg_.showSpectrum);
    cfg_.showClock        = ReadBool(L"ShowClock", cfg_.showClock);
    cfg_.showCpuMem       = ReadBool(L"ShowCpuMem", cfg_.showCpuMem);
    cfg_.showToast        = ReadBool(L"ShowToast", cfg_.showToast);
    cfg_.showClipboard    = ReadBool(L"ShowClipboard", cfg_.showClipboard);
    cfg_.toastContentMode = ReadInt(L"ToastContentMode", cfg_.toastContentMode);

    cfg_.musicPlatforms  = ReadString(L"MusicPlatforms", cfg_.musicPlatforms);
    cfg_.lyricsDelayMs   = ReadInt(L"LyricsDelayMs", cfg_.lyricsDelayMs);
    cfg_.soundName       = ReadString(L"SoundName", cfg_.soundName);
    cfg_.soundVolume     = ReadFloat(L"SoundVolume", cfg_.soundVolume);
    cfg_.justSoloEnabled = ReadBool(L"JustSoloEnabled", cfg_.justSoloEnabled);

    cfg_.autoHideEnabled   = ReadBool(L"AutoHideEnabled", cfg_.autoHideEnabled);
    cfg_.hideOnFocusLeave  = ReadBool(L"HideOnFocusLeave", cfg_.hideOnFocusLeave);
    cfg_.hideOnPause       = ReadBool(L"HideOnPause", cfg_.hideOnPause);
    cfg_.hideOnFullscreen  = ReadBool(L"HideOnFullscreen", cfg_.hideOnFullscreen);
    cfg_.clickThrough      = ReadBool(L"ClickThrough", cfg_.clickThrough);
    cfg_.dblClickSwitchApp = ReadBool(L"DblClickSwitchApp", cfg_.dblClickSwitchApp);

    LOGI(L"配置已载入：media=%d lyrics=%d spectrum=%d clock=%d cpumem=%d toast=%d clipboard=%d justSolo=%d",
         cfg_.showMedia ? 1 : 0, cfg_.showLyrics ? 1 : 0, cfg_.showSpectrum ? 1 : 0,
         cfg_.showClock ? 1 : 0, cfg_.showCpuMem ? 1 : 0, cfg_.showToast ? 1 : 0,
         cfg_.showClipboard ? 1 : 0, cfg_.justSoloEnabled ? 1 : 0);
}

void ConfigStore::Save() {
    WriteInt(L"Theme", cfg_.theme);
    WriteFloat(L"BgOpacity", cfg_.bgOpacity);
    WriteString(L"FontFamily", cfg_.fontFamily);
    WriteFloat(L"DpiScale", cfg_.dpiScale);
    WriteInt(L"TargetMonitor", cfg_.targetMonitor);
    WriteBool(L"TopMost", cfg_.topMost);
    WriteBool(L"AutoUpdate", cfg_.autoUpdate);

    // 开机自启：镜像 HKCU Run 键，设置页改动即刻生效
    if (cfg_.autoStart) {
        wchar_t exe[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring cmd = std::wstring(L"\"") + exe + L"\"";
        const LSTATUS st = ::RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey,
                                             L"FloatingIsland", REG_SZ, cmd.c_str(),
                                             static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
        if (st != ERROR_SUCCESS) {
            LOGW(L"写入开机自启失败 (code=%ld)", static_cast<long>(st));
        } else {
            LOGI(L"开机自启已写入");
        }
    } else {
        const LSTATUS st = ::RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey,
                                                L"FloatingIsland");
        if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
            LOGW(L"删除开机自启失败 (code=%ld)", static_cast<long>(st));
        } else {
            LOGI(L"开机自启已移除");
        }
    }

    std::wstring order;
    for (size_t i = 0; i < cfg_.showOrder.size(); ++i) {
        if (i) order += L',';
        order += ModuleToString(cfg_.showOrder[i]);
    }
    WriteString(L"ShowOrder", order);
    WriteBool(L"ShowMedia", cfg_.showMedia);
    WriteBool(L"ShowLyrics", cfg_.showLyrics);
    WriteBool(L"ShowSpectrum", cfg_.showSpectrum);
    WriteBool(L"ShowClock", cfg_.showClock);
    WriteBool(L"ShowCpuMem", cfg_.showCpuMem);
    WriteBool(L"ShowToast", cfg_.showToast);
    WriteBool(L"ShowClipboard", cfg_.showClipboard);
    WriteInt(L"ToastContentMode", cfg_.toastContentMode);

    WriteString(L"MusicPlatforms", cfg_.musicPlatforms);
    WriteInt(L"LyricsDelayMs", cfg_.lyricsDelayMs);
    WriteString(L"SoundName", cfg_.soundName);
    WriteFloat(L"SoundVolume", cfg_.soundVolume);
    WriteBool(L"JustSoloEnabled", cfg_.justSoloEnabled);

    WriteBool(L"AutoHideEnabled", cfg_.autoHideEnabled);
    WriteBool(L"HideOnFocusLeave", cfg_.hideOnFocusLeave);
    WriteBool(L"HideOnPause", cfg_.hideOnPause);
    WriteBool(L"HideOnFullscreen", cfg_.hideOnFullscreen);
    WriteBool(L"ClickThrough", cfg_.clickThrough);
    WriteBool(L"DblClickSwitchApp", cfg_.dblClickSwitchApp);
    LOGI(L"配置已保存");
}

} // namespace fi
