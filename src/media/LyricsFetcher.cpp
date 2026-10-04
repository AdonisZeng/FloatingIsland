#include "LyricsFetcher.h"

#include "../net/HttpClient.h"
#include "../util/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <string_view>

namespace fi {

namespace json = nlohmann;

namespace {

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                        static_cast<int>(s.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    if (n > 0) {
        ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                              out.data(), n, nullptr, nullptr);
    }
    return out;
}

std::wstring UrlEscape(const std::wstring& s) {
    const std::string utf8 = ToUtf8(s);
    std::wstring out;
    wchar_t buf[8];
    for (unsigned char c : utf8) {
        if (::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<wchar_t>(c);
        } else {
            swprintf_s(buf, L"%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// QQ 接口正文是 HTML 实体转义的，统一还原（单次扫描：具名实体 + 数字实体）
std::wstring UnescapeQqText(const std::wstring& s) {
    static const struct { const wchar_t* from; const wchar_t* to; } kNamed[] = {
        { L"amp",  L"&"  }, { L"lt",   L"<"  }, { L"gt",  L">"  },
        { L"quot", L"\"" }, { L"apos", L"'"  }, { L"nbsp", L" " },
        { L"#10",  L"\n" }, { L"#13",  L"\r" }, { L"#32", L" "  },
        { L"#45",  L"-"  },
    };

    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'&') { out += s[i]; continue; }
        const size_t semi = s.find(L';', i + 1);
        if (semi == std::wstring::npos || semi - i > 16) { out += s[i]; continue; }

        const std::wstring name = s.substr(i + 1, semi - i - 1);
        bool replaced = false;
        for (const auto& row : kNamed) {
            if (name == row.from) { out += row.to; replaced = true; break; }
        }
        // &#169; / &#x1F600; 之类：十/十六进制码点
        if (!replaced && name.size() > 1 && name[0] == L'#') {
            const bool hex = (name[1] == L'x' || name[1] == L'X');
            const wchar_t* digits = name.c_str() + (hex ? 2 : 1);
            wchar_t* end = nullptr;
            const long v = std::wcstol(digits, &end, hex ? 16 : 10);
            if (end && *end == L'\0' && v > 0 && v < 0x10FFFF) {
                out += static_cast<wchar_t>(v);
                replaced = true;
            }
        }

        if (replaced) i = semi;
        else          out += s[i];
    }
    return out;
}

// 归一化：只留字母/数字/汉字假名，转小写（对齐参照 NormalizeToken）
std::wstring NormalizeToken(const std::wstring& in) {
    std::wstring out;
    for (wchar_t c : in) {
        if (::iswalnum(c) || c >= 0x2E80) {
            out += static_cast<wchar_t>(::towlower(c));
        }
    }
    return out;
}

// 歌手串按常见分隔符拆成多个名字
std::vector<std::wstring> SplitArtists(const std::wstring& in) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : in) {
        if (c == L'/' || c == L'\\' || c == L',' || c == L'、' ||
            c == L';' || c == L'；' || c == L'|' || c == L'&') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    for (std::wstring& a : out) a = NormalizeToken(a);
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                        static_cast<int>(s.size()),
                                        nullptr, 0);
    std::wstring out(n, L'\0');
    if (n > 0) {
        ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                              out.data(), n);
    }
    return out;
}

// nlohmann 只支持窄字符串：统一经 UTF-8 取字段
std::wstring JStr(const json::json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_string()) return {};
    return FromUtf8(j[key].get<std::string>());
}

// 数值同理：value(k, default) 只在键缺失时给默认值，类型不符仍抛 type_error
template <typename T>
T JNum(const json::json& j, const char* key, T def) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_number()) return def;
    return j[key].get<T>();
}

// 判据：出现形如 [12: 的时间标签才算"可用时间轴"。
// 只看 "[数字" 会被 "[1st verse]"、"chorus [2x]" 之类的文本骗过
bool HasTimedLyric(const std::wstring& s) {
    for (size_t i = s.find(L'['); i != std::wstring::npos; i = s.find(L'[', i + 1)) {
        size_t j = i + 1;
        while (j < s.size() && ::iswdigit(s[j])) ++j;
        if (j > i + 1 && j < s.size() && s[j] == L':') return true;
    }
    return false;
}

// 落月搜索结果里挑最匹配的一条：标题归一化后全等(2分)/互相包含(1分)，
// 歌手任一对得上才候用；返回最高分 id，带出 cover / mid
long MatchSong(const json::json& list, const std::wstring& title,
               const std::wstring& artist, std::wstring& cover, std::wstring& mid) {
    const std::wstring wantTitle = NormalizeToken(title);
    if (wantTitle.empty()) return 0;
    const std::vector<std::wstring> wantArtists = SplitArtists(artist);

    long bestId = 0;
    int bestScore = 0;
    for (const auto& song : list) {
        if (!song.is_object()) continue;
        const std::wstring songName = NormalizeToken(JStr(song, "song"));
        const long id = JNum(song, "id", 0L);
        if (songName.empty() || id <= 0) continue;

        int score = 0;
        if (songName == wantTitle) score = 2;
        else if (songName.find(wantTitle) != std::wstring::npos ||
                 wantTitle.find(songName) != std::wstring::npos) score = 1;
        if (score == 0) continue;

        // 歌手：落月字段 singer，任一对得上即过；查询无歌手时放行
        const std::wstring singer = NormalizeToken(JStr(song, "singer"));
        bool artistOk = wantArtists.empty();
        for (const std::wstring& w : wantArtists) {
            if (!w.empty() && singer.find(w) != std::wstring::npos) { artistOk = true; break; }
        }
        if (!artistOk) continue;

        if (score > bestScore) {
            bestScore = score;
            bestId = id;
            cover = JStr(song, "cover");
            mid = JStr(song, "mid");
        }
    }
    return bestId;
}

} // namespace（工具/匹配打分）

// ==================== 引擎实现 ====================

constexpr wchar_t kLuoYueHost[]   = L"https://api.vkeys.cn";
constexpr wchar_t kQqLyricApi[]   = L"https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg";
constexpr wchar_t kQqSmartBox[]   = L"https://c.y.qq.com/splcloud/fcgi-bin/smartbox_new.fcg";
constexpr wchar_t kQqSingleSong[] = L"https://c.y.qq.com/v8/fcg-bin/fcg_play_single_song.fcg";

struct ChainOut {
    std::wstring lrc, trans, cover, mid;
};

// 引擎1/3：落月 API（QQ / 网易两路；搜索同构：data[].song/singer/id/cover[/mid]）
ChainOut FetchLuoYue(const std::wstring& title, const std::wstring& artist,
                     bool neteaseSrc) {
    ChainOut out;
    const std::wstring word = UrlEscape(artist.empty() ? title : title + L" " + artist);
    const std::wstring searchUrl = neteaseSrc
        ? std::wstring(kLuoYueHost) + L"/v2/music/netease?word=" + word
        : std::wstring(kLuoYueHost) + L"/v2/music/tencent/search/song?word=" + word;

    const HttpResponse resp = HttpClient::Get(searchUrl);
    if (!resp.Ok()) return out;
    json::json doc;
    try { doc = json::json::parse(resp.Utf8()); } catch (...) { return out; }
    if (!doc.contains("data") || !doc["data"].is_array()) return out;

    std::wstring cover, mid;
    const long songId = MatchSong(doc["data"], title, artist, cover, mid);
    out.cover = cover;
    out.mid = mid;
    if (songId <= 0) return out;

    const std::wstring lyricUrl = neteaseSrc
        ? std::wstring(kLuoYueHost) + L"/v2/music/netease/lyric?id=" + std::to_wstring(songId)
        : std::wstring(kLuoYueHost) + L"/v2/music/tencent/lyric?id=" + std::to_wstring(songId);

    const HttpResponse lr = HttpClient::Get(lyricUrl);
    if (!lr.Ok()) return out;
    json::json ldoc;
    try { ldoc = json::json::parse(lr.Utf8()); } catch (...) { return out; }
    if (JNum(ldoc, "code", 0) != 200 || !ldoc.contains("data") ||
        !ldoc["data"].is_object()) {
        return out;   // 封面/mid 已带出，歌词留给下一引擎
    }
    const auto& data = ldoc["data"];
    out.lrc = JStr(data, "lrc");
    out.trans = JStr(data, "trans");
    return out;
}

// 引擎2：QQ 官方歌词（需 songmid；trans 常空，仅在译文缺失时采纳）
bool FetchQqOfficialLyric(const std::wstring& mid, std::wstring& lrc,
                          std::wstring& trans) {
    const std::wstring url = std::wstring(kQqLyricApi) + L"?songmid=" + mid +
                             L"&format=json&nobase64=1";
    const HttpResponse resp = HttpClient::Get(
        url, { { L"Referer", L"https://y.qq.com/" } });
    if (!resp.Ok()) return false;
    json::json doc;
    try { doc = json::json::parse(resp.Utf8()); } catch (...) { return false; }
    bool ok = false;
    if (doc.contains("lyric")) {
        lrc = UnescapeQqText(JStr(doc, "lyric"));
        ok = HasTimedLyric(lrc);
    }
    if (doc.contains("trans")) {
        trans = UnescapeQqText(JStr(doc, "trans"));
    }
    return ok;
}

// 引擎4：网易云官方（搜索 -> 歌词 -> 封面，三重校验：歌名+歌手+时长±4s）
ChainOut FetchNeteaseOfficial(const std::wstring& title,
                              const std::wstring& artist, double durationSec) {
    ChainOut out;
    const HeaderList headers = {
        { L"Referer", L"https://music.163.com" },
        { L"X-Real-IP", L"114.114.114.114" },
    };
    // 搜索（POST 表单）
    const std::string form = "s=" + ToUtf8(UrlEscape(title + L" " + artist)) +
                             "&type=1&limit=60&offset=0";
    const HttpResponse sr = HttpClient::Post(
        L"https://music.163.com/api/search/get/web", form,
        L"application/x-www-form-urlencoded", headers);
    if (!sr.Ok()) return out;
    json::json sdoc;
    try { sdoc = json::json::parse(sr.Utf8()); } catch (...) { return out; }
    if (!sdoc.contains("result") || !sdoc["result"].contains("songs")) return out;

        // 候选匹配：歌名包含 + 歌手任一匹配 + 时长±4s
    const auto& songs = sdoc["result"]["songs"];
    if (!songs.is_array()) return out;
    const std::wstring wtitle = NormalizeToken(title);
    const std::vector<std::wstring> warts = SplitArtists(artist);
    long songId = 0;
    for (const auto& s : songs) {
        if (!s.is_object()) continue;
        const std::wstring name = NormalizeToken(JStr(s, "name"));
        if (name.find(wtitle) == std::wstring::npos &&
            wtitle.find(name) == std::wstring::npos) continue;
        bool artistOk = warts.empty();
        if (s.contains("artists") && s["artists"].is_array()) {
            for (const auto& a : s["artists"]) {
                const std::wstring an = NormalizeToken(JStr(a, "name"));
                for (const std::wstring& w : warts) {
                    if (!w.empty() && an.find(w) != std::wstring::npos) artistOk = true;
                }
            }
        }
        if (!artistOk) continue;
        if (durationSec > 0 && s.contains("duration")) {
            const double dsec = static_cast<double>(JNum(s, "duration", 0L)) / 1000.0;
            if (std::fabs(dsec - durationSec) > 4.0) continue;
        }
        songId = JNum(s, "id", 0L);
        if (songId > 0) break;
    }
    if (songId <= 0) return out;

    // 歌词（lrc.lyric + tlyric.lyric）
    const std::wstring lyricUrl =
        L"https://music.163.com/api/song/lyric?id=" + std::to_wstring(songId) +
        L"&lv=-1&kv=-1&tv=-1";
    const HttpResponse lr = HttpClient::Get(lyricUrl, headers);
    if (lr.Ok()) {
        json::json ldoc;
        try { ldoc = json::json::parse(lr.Text()); } catch (...) { ldoc = json::json{}; }
        if (ldoc.contains("lrc") && ldoc["lrc"].is_object()) {
            out.lrc = JStr(ldoc["lrc"], "lyric");
        }
        if (ldoc.contains("tlyric") && ldoc["tlyric"].is_object()) {
            out.trans = JStr(ldoc["tlyric"], "lyric");
        }
    }

    // 封面（song/detail?ids=[id] -> songs[0].album.picUrl）
    const std::wstring detUrl = L"https://music.163.com/api/song/detail?ids=%5B" +
                                std::to_wstring(songId) + L"%5D";
    const HttpResponse dr = HttpClient::Get(detUrl, headers);
    if (dr.Ok()) {
        json::json ddoc;
        try { ddoc = json::json::parse(dr.Utf8()); } catch (...) { return out; }
        if (ddoc.contains("songs") && ddoc["songs"].is_array() &&
            !ddoc["songs"].empty() && ddoc["songs"][0].contains("album")) {
            out.cover = JStr(ddoc["songs"][0]["album"], "picUrl");
        }
    }
    return out;
}

// 引擎5：LRCLIB（读 syncedLyrics）
bool FetchLrclib(const std::wstring& title, const std::wstring& artist,
                 double durationSec, std::wstring& lrc) {
    std::wstring url = L"https://lrclib.net/api/get?track_name=" +
                       UrlEscape(title) + L"&artist_name=" + UrlEscape(artist);
    if (durationSec > 0) {
        url += L"&duration=" + std::to_wstring(static_cast<int>(durationSec));
    }
    const HttpResponse resp = HttpClient::Get(url);
    if (!resp.Ok()) return false;
    json::json doc;
    try { doc = json::json::parse(resp.Utf8()); } catch (...) { return false; }
    lrc = JStr(doc, "syncedLyrics");
    return HasTimedLyric(lrc);
}

// QQ 直连封面兜底：smartbox 搜索建议 -> songmid -> album.mid -> CDN 图
std::wstring FetchQqCover(const std::wstring& title, const std::wstring& artist) {
    const HeaderList headers = { { L"Referer", L"https://y.qq.com/" } };
    const std::wstring word = UrlEscape(artist.empty() ? title : title + L" " + artist);
    const HttpResponse sr = HttpClient::Get(
        std::wstring(kQqSmartBox) + L"?key=" + word + L"&format=json&utf8=1",
        headers);
    if (!sr.Ok()) return {};
    json::json sdoc;
    try { sdoc = json::json::parse(sr.Utf8()); } catch (...) { return {}; }
    if (!sdoc.contains("data") || !sdoc["data"].contains("song") ||
        !sdoc["data"]["song"].contains("itemlist") ||
        !sdoc["data"]["song"]["itemlist"].is_array()) {
        return {};
    }

    const std::wstring wtitle = NormalizeToken(title);
    const std::vector<std::wstring> warts = SplitArtists(artist);
    std::wstring songMid;
    for (const auto& item : sdoc["data"]["song"]["itemlist"]) {
        if (!item.is_object()) continue;
        const std::wstring name = NormalizeToken(JStr(item, "name"));
        if (name != wtitle && name.find(wtitle) == std::wstring::npos &&
            wtitle.find(name) == std::wstring::npos) continue;
        const std::wstring singer = NormalizeToken(JStr(item, "singer"));
        bool ok = warts.empty();
        for (const std::wstring& w : warts) {
            if (!w.empty() && singer.find(w) != std::wstring::npos) ok = true;
        }
        if (!ok) continue;
        songMid = JStr(item, "mid");
        if (!songMid.empty()) break;
    }
    if (songMid.empty()) return {};

    const HttpResponse dr = HttpClient::Get(
        std::wstring(kQqSingleSong) + L"?songmid=" + songMid +
        L"&platform=yqq&format=json", headers);
    if (!dr.Ok()) return {};
    json::json ddoc;
    try { ddoc = json::json::parse(dr.Utf8()); } catch (...) { return {}; }
    if (!ddoc.contains("data") || !ddoc["data"].is_array() ||
        ddoc["data"].empty() || !ddoc["data"][0].contains("album")) {
        return {};
    }
    const std::wstring albumMid = JStr(ddoc["data"][0]["album"], "mid");
    if (albumMid.empty()) return {};
    return std::wstring(L"https://y.gtimg.cn/music/photo_new/T002R300x300M000") +
           albumMid + L".jpg";
}

// ==================== LRC 解析 ====================

namespace {

// 解析单个时间标签 [mm:ss.xx] / [mm:ss.xxx] / [mm:ss:ff(75fps)] -> 秒
bool ParseTimeTag(const std::wstring& tag, double& out) {
    // tag 形如 mm:ss.xx（无括号）
    const size_t c1 = tag.find(L':');
    if (c1 == std::wstring::npos) return false;
    const size_t c2 = tag.find(L':', c1 + 1);
    const size_t dot = tag.find_first_of(L".:", c2 == std::wstring::npos ? c1 + 1 : c2 + 1);

    int m = 0, s = 0;
    double frac = 0.0;
    if (c2 == std::wstring::npos) {
        if (dot == std::wstring::npos) {
            // mm:ss 整秒
            try {
                m = std::stoi(tag.substr(0, c1));
                s = std::stoi(tag.substr(c1 + 1));
            } catch (...) { return false; }
            if (m < 0 || s < 0 || s >= 60) return false;
            out = m * 60.0 + s;
            return true;
        }
        // mm:ss.xx
        try {
            m = std::stoi(tag.substr(0, c1));
            s = std::stoi(tag.substr(c1 + 1, dot - c1 - 1));
            frac = std::stod(L"0." + tag.substr(dot + 1));
        } catch (...) { return false; }
    } else if (dot == c2 || dot == std::wstring::npos) {
        // mm:ss:ff 帧格式（75fps）
        try {
            m = std::stoi(tag.substr(0, c1));
            s = std::stoi(tag.substr(c1 + 1, c2 - c1 - 1));
            frac = std::stod(tag.substr(c2 + 1)) / 75.0;
        } catch (...) { return false; }
    } else {
        return false;
    }
    if (m < 0 || s < 0 || s >= 60) return false;
    out = m * 60.0 + s + frac;
    return true;
}

} // namespace

std::vector<LyricLine> ParseLrc(const std::wstring& lrcText) {
    std::vector<LyricLine> lines;
    size_t pos = 0;
    while (pos <= lrcText.size()) {
        const size_t eol = lrcText.find(L'\n', pos);
        // 末行通常没有换行符：必须先解析再退出（旧实现在解析前就 break，
        // 导致最后一句歌词永远丢失）
        const std::wstring_view line = (eol == std::wstring::npos)
            ? std::wstring_view(lrcText).substr(pos)
            : std::wstring_view(lrcText).substr(pos, eol - pos);

        // 一行可带多个时间标签
        size_t p = 0;
        std::vector<double> tags;
        for (;;) {
            const size_t lb = line.find(L'[', p);
            if (lb == std::wstring::npos) break;
            const size_t rb = line.find(L']', lb);
            if (rb == std::wstring::npos) break;
            const std::wstring inner(line.substr(lb + 1, rb - lb - 1));
            double t = 0;
            // 跳过元数据标签（ti/ar/al/by/offset 等）
            if (!inner.empty() && ::iswdigit(inner[0]) && ParseTimeTag(inner, t)) {
                tags.push_back(t);
            }
            p = rb + 1;
        }

        if (!tags.empty()) {
            std::wstring text = (p <= line.size()) ? std::wstring(line.substr(p)) : std::wstring{};
            // 去掉行内增强标签 <mm:ss.xx>
            size_t lt = 0;
            while ((lt = text.find(L'<')) != std::wstring::npos) {
                const size_t gt = text.find(L'>', lt);
                if (gt == std::wstring::npos) break;
                text.erase(lt, gt - lt + 1);
            }
            while (!text.empty() && (text.back() == L'\r' || text.back() == L' ')) text.pop_back();
            if (!text.empty()) {
                for (double t : tags) lines.push_back({ t, text });
            }
        }

        if (eol == std::wstring::npos) break;
        pos = eol + 1;
    }
    std::sort(lines.begin(), lines.end(),
              [](const LyricLine& a, const LyricLine& b) { return a.t < b.t; });
    return lines;
}

std::vector<LyricLine> AlignTranslation(const std::vector<LyricLine>& lines,
                                        const std::vector<LyricLine>& trans) {
    std::vector<LyricLine> out(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        out[i].t = lines[i].t;
        double bestDt = 0.31;   // ±300ms 容差
        for (const LyricLine& tr : trans) {
            const double dt = std::fabs(tr.t - lines[i].t);
            if (dt < bestDt) {
                bestDt = dt;
                out[i].text = tr.text;
            }
        }
    }
    return out;
}

// ==================== 主链编排 ====================

LyricsFetchResult FetchLyricsChain(const std::wstring& title,
                                   const std::wstring& artist,
                                   double durationSec,
                                   bool preferNetease) {
    LyricsFetchResult out;
    std::wstring lrc, trans, mid;

    // 网易优先段（网易云会话时提前）：落月-网易 → 网易云官方
    if (preferNetease) {
        const ChainOut n1 = FetchLuoYue(title, artist, true);
        if (out.coverUrl.empty()) out.coverUrl = n1.cover;
        if (!HasTimedLyric(lrc) && HasTimedLyric(n1.lrc)) {
            lrc = n1.lrc;
            if (trans.empty()) trans = n1.trans;
        }
        if (!HasTimedLyric(lrc) || out.coverUrl.empty()) {
            const ChainOut n2 = FetchNeteaseOfficial(title, artist, durationSec);
            if (!HasTimedLyric(lrc) && HasTimedLyric(n2.lrc)) {
                lrc = n2.lrc;
                if (trans.empty()) trans = n2.trans;
            }
            if (out.coverUrl.empty()) out.coverUrl = n2.cover;
        }
    }

    // 常规链：落月(QQ) → QQ官方 → 落月(网易) → 网易云官方 → LRCLIB
    const ChainOut q1 = FetchLuoYue(title, artist, false);
    if (out.coverUrl.empty()) out.coverUrl = q1.cover;
    if (mid.empty()) mid = q1.mid;
    if (!HasTimedLyric(lrc) && HasTimedLyric(q1.lrc)) {
        lrc = q1.lrc;
        if (trans.empty()) trans = q1.trans;
    }

    // QQ 官方（需要 songmid）；译文仅在完全没拿到时采纳（QQ 译文常空）
    if (!HasTimedLyric(lrc) && !mid.empty()) {
        std::wstring qlrc, qtrans;
        if (FetchQqOfficialLyric(mid, qlrc, qtrans)) {
            lrc = qlrc;
            if (trans.empty()) trans = qtrans;
        }
    }

    if (!HasTimedLyric(lrc)) {
        const ChainOut n1 = FetchLuoYue(title, artist, true);
        if (HasTimedLyric(n1.lrc)) {
            lrc = n1.lrc;
            if (trans.empty()) trans = n1.trans;
        }
    }

    if (!HasTimedLyric(lrc)) {
        const ChainOut n2 = FetchNeteaseOfficial(title, artist, durationSec);
        if (HasTimedLyric(n2.lrc)) {
            lrc = n2.lrc;
            if (trans.empty()) trans = n2.trans;
        }
    }

    if (!HasTimedLyric(lrc)) {
        FetchLrclib(title, artist, durationSec, lrc);
    }

    // 封面兜底：整条歌词链没给封面 -> QQ 直连
    if (out.coverUrl.empty()) {
        out.coverUrl = FetchQqCover(title, artist);
    }

    // 解析 + 译文对齐
    out.lyric.lines = ParseLrc(lrc);
    if (!out.lyric.lines.empty()) {
        out.lyric.valid = true;
        out.lyric.translation = AlignTranslation(out.lyric.lines, ParseLrc(trans));
        for (const LyricLine& t : out.lyric.translation) {
            if (!t.text.empty()) { out.lyric.hasTranslation = true; break; }
        }
    }
    // 只记长度与命中情况：歌名/歌词明文不进日志
    LOGI(L"歌词链结束：命中=%d 行数=%zu 有译文=%d 封面=%d len(title)=%zu len(artist)=%zu duration=%.1f",
         out.lyric.valid ? 1 : 0, out.lyric.lines.size(),
         out.lyric.hasTranslation ? 1 : 0, out.coverUrl.empty() ? 0 : 1,
         title.size(), artist.size(), durationSec);
    return out;
}

bool DownloadImageBytes(const std::wstring& url, std::vector<uint8_t>& out) {
    if (url.empty()) return false;
    HttpResponse resp = HttpClient::Get(url, {}, 8000);   // 不能是 const，否则 move 退化成 8MB 拷贝
    if (!resp.Ok()) {
        LOGW(L"封面下载失败 status=%lu", static_cast<unsigned long>(resp.status));
        return false;
    }
    if (resp.truncated) {
        LOGW(L"封面下载被截断，放弃解码");
        return false;
    }
    out = std::move(resp.body);
    LOGI(L"封面已下载 bytes=%zu", out.size());
    return true;
}

} // namespace fi
