#pragma once
// 歌词/封面抓取（同步实现）：五引擎回退链 + LRC/译文解析 + 封面链。
// 【线程约束】仅在网络工作线程调用；主线程只读 LyricsService 的结果快照。
//
// 引擎链（对齐参照实现）：
//   网易优先段（仅网易云会话）：落月-网易 → 网易云官方
//   常规链：落月(QQ) → QQ官方歌词(需songmid) → 落月(网易) → 网易云官方 → LRCLIB
// 判据 = 是否取到【可用时间轴】，而非文本非空。
// 封面链：网易搜索cover → 落月QQ搜索cover → 网易云song/detail → QQ直连(smartbox) → 无。

#include <string>
#include <vector>

namespace fi {

struct LyricLine {
    double      t    = 0.0;   // 秒
    std::wstring text;
};

struct LyricData {
    bool valid          = false;  // 有可用时间轴
    bool hasTranslation = false;
    std::vector<LyricLine> lines;       // 已按时间排序
    std::vector<LyricLine> translation; // 与 lines 等长、按行对齐（无译文行为空）
};

struct LyricsFetchResult {
    LyricData    lyric;
    std::wstring coverUrl;   // 封面地址（可能为空 = 用兜底）
};

// —— 同步抓取（网络线程）——
LyricsFetchResult FetchLyricsChain(const std::wstring& title,
                                   const std::wstring& artist,
                                   double durationSec,
                                   bool preferNetease);

bool DownloadImageBytes(const std::wstring& url, std::vector<uint8_t>& out);

// —— LRC 解析（无网络依赖，可单测）——
// 支持多行时间标签、百分秒/毫秒/帧(mm:ss:ff, 75fps)格式；空行与纯元数据行丢弃
std::vector<LyricLine> ParseLrc(const std::wstring& lrcText);

// 译文按时间戳 ±300ms 对齐进原文行
std::vector<LyricLine> AlignTranslation(const std::vector<LyricLine>& lines,
                                        const std::vector<LyricLine>& trans);

} // namespace fi
