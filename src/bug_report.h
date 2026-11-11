// BugReport -- phase 89b-d: the one-press bug report (the Insert capture). Pure helpers, no D3D: CRC-32, a "stored" ZIP writer,
// the privacy scrubber (user name / home folder / Steam library root out of every text file), the capture plan (which A/B sets,
// settle + frame counts per capture_mode), the keep-N pruning of old captures, the double-press guard of the capture key and the
// rewind length in frames. inject.cpp runs the capture (render thread + the rewind writer thread); tests/bug_report_test.cpp
// checks this file. Both builds include it (Windows file APIs only).
#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace BugReport {

// ---- CRC-32 (IEEE, as ZIP uses it) ----------------------------------------------------------------------------------------------
inline uint32_t Crc32(const void* data, size_t n, uint32_t crc = 0) {
    struct Table { uint32_t t[256]; Table() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        } } };
    static const Table s_tab;                              // review fix (2026-11-01): a magic static is built once, thread-safe
    const uint32_t* const table = s_tab.t;
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// ---- ZIP writer: method 0 (stored), ZIP32 (< 4 GB), names in UTF-8 (flag bit 11). PNGs are compressed already; the text files
// are small. AddFile streams the file (64 KB chunks) and patches its local header afterwards (CRC / sizes).
class Zip {
public:
    // review fix: 64-bit file positions, and the ZIP32 limits checked -- an archive that would pass them is refused (Bad()) instead
    // of written with wrapped offsets; the caller deletes it. FitsZip32 = the same test up front from the files' total size.
    static constexpr uint64_t kMaxBytes = 0xFFFFFFFFull - (64ull << 20);   // all offsets / sizes stay below 4 GB (64 MB headroom)
    static constexpr size_t   kMaxEntries = 65535;
    static bool FitsZip32(uint64_t totalBytes, size_t files) {
        return files <= kMaxEntries && totalBytes + (uint64_t)files * 400ull <= kMaxBytes;
    }
    ~Zip() { if (m_f) fclose(m_f); }
    bool Open(const wchar_t* path) { return _wfopen_s(&m_f, path, L"wb") == 0 && m_f; }
    bool AddData(const char* name, const void* data, size_t n) {
        if (!m_f || m_bad) return false;
        const long long off = _ftelli64(m_f);
        if (off < 0 || m_e.size() >= kMaxEntries || (uint64_t)off + n + 512 > kMaxBytes) { m_bad = true; return false; }
        Ent e{ name, Crc32(data, n), (uint32_t)n, (uint32_t)off };
        if (!(Header(e) && fwrite(data, 1, n, m_f) == n)) { m_bad = true; return false; }
        m_e.push_back(e);
        return true;
    }
    // false + Bad() = the archive is unusable (a write failed / a limit passed); false alone = `src` could not be opened (skipped)
    // phase 89g: bytes [off, off + len) of `src` as its own entry (one piece of a file split across parts). false + Bad() as AddFile.
    bool AddFileRange(const char* name, const wchar_t* src, uint64_t off, uint64_t len) {
        if (!m_f || m_bad) return false;
        FILE* in = _wfsopen(src, L"rb", _SH_DENYNO);
        if (!in) return false;
        if (_fseeki64(in, (long long)off, SEEK_SET) != 0) { fclose(in); return false; }
        const long long at = _ftelli64(m_f);
        if (at < 0 || m_e.size() >= kMaxEntries || (uint64_t)at + len + 512 > kMaxBytes) { fclose(in); m_bad = true; return false; }
        Ent e{ name, 0, 0, (uint32_t)at };
        bool ok = Header(e);
        std::vector<uint8_t> buf(1u << 16);
        uint64_t left = len;
        while (ok && left > 0) {
            const size_t want = (size_t)(left < buf.size() ? left : buf.size());
            const size_t k = fread(buf.data(), 1, want, in);
            if (k == 0) break;
            e.crc = Crc32(buf.data(), k, e.crc);
            ok = fwrite(buf.data(), 1, k, m_f) == k;
            left -= k;
        }
        fclose(in);
        if (!ok || left) { m_bad = true; return false; }
        e.size = (uint32_t)len;
        const long long end = _ftelli64(m_f);
        ok = end >= 0 && _fseeki64(m_f, at, SEEK_SET) == 0 && Header(e) && _fseeki64(m_f, end, SEEK_SET) == 0;
        if (!ok) { m_bad = true; return false; }
        m_e.push_back(e);
        return true;
    }
    bool AddFile(const char* name, const wchar_t* src) {
        if (!m_f || m_bad) return false;
        FILE* in = _wfsopen(src, L"rb", _SH_DENYNO);
        if (!in) return false;
        const long long off = _ftelli64(m_f);
        if (off < 0 || m_e.size() >= kMaxEntries || (uint64_t)off > kMaxBytes) { fclose(in); m_bad = true; return false; }
        Ent e{ name, 0, 0, (uint32_t)off };
        bool ok = Header(e);
        std::vector<uint8_t> buf(1u << 16);
        uint64_t total = 0;
        size_t k;
        while (ok && (k = fread(buf.data(), 1, buf.size(), in)) > 0) {
            e.crc = Crc32(buf.data(), k, e.crc);
            total += k;
            ok = (uint64_t)off + total + 512 <= kMaxBytes && fwrite(buf.data(), 1, k, m_f) == k;
        }
        fclose(in);
        if (!ok) { m_bad = true; return false; }
        e.size = (uint32_t)total;
        const long long end = _ftelli64(m_f);
        ok = end >= 0 && _fseeki64(m_f, off, SEEK_SET) == 0 && Header(e) && _fseeki64(m_f, end, SEEK_SET) == 0;
        if (!ok) { m_bad = true; return false; }
        m_e.push_back(e);
        return true;
    }
    bool Close() {
        if (!m_f) return false;
        const long long cdOff64 = _ftelli64(m_f);
        bool ok = !m_bad && cdOff64 >= 0 && (uint64_t)cdOff64 + m_e.size() * 600ull < 0xFFFFFFFFull;
        const uint32_t cdOff = (uint32_t)cdOff64;
        for (const Ent& e : m_e) {
            if (!ok) break;
            uint8_t h[46] = {};
            Put32(h, 0x02014b50u); Put16(h + 4, 20); Put16(h + 6, 20); Put16(h + 8, 0x0800); Put16(h + 10, 0);
            Put16(h + 12, 0); Put16(h + 14, 0x21);          // 1980-01-01 00:00
            Put32(h + 16, e.crc); Put32(h + 20, e.size); Put32(h + 24, e.size); Put16(h + 28, (uint16_t)e.name.size());
            Put32(h + 42, e.off);
            ok = fwrite(h, 1, 46, m_f) == 46 && fwrite(e.name.data(), 1, e.name.size(), m_f) == e.name.size();
        }
        const uint32_t cdSize = (uint32_t)(_ftelli64(m_f) - cdOff64);
        uint8_t z[22] = {};
        Put32(z, 0x06054b50u); Put16(z + 8, (uint16_t)m_e.size()); Put16(z + 10, (uint16_t)m_e.size());
        Put32(z + 12, cdSize); Put32(z + 16, cdOff);
        ok = ok && fwrite(z, 1, 22, m_f) == 22;
        ok = fclose(m_f) == 0 && ok;
        m_f = nullptr;
        return ok;
    }
    size_t Count() const { return m_e.size(); }
    bool Bad() const { return m_bad; }
private:
    struct Ent { std::string name; uint32_t crc, size, off; };
    static void Put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
    static void Put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
    bool Header(const Ent& e) {
        uint8_t h[30] = {};
        Put32(h, 0x04034b50u); Put16(h + 4, 20); Put16(h + 6, 0x0800); Put16(h + 8, 0);
        Put16(h + 10, 0); Put16(h + 12, 0x21);
        Put32(h + 14, e.crc); Put32(h + 18, e.size); Put32(h + 22, e.size); Put16(h + 26, (uint16_t)e.name.size());
        return fwrite(h, 1, 30, m_f) == 30 && fwrite(e.name.data(), 1, e.name.size(), m_f) == e.name.size();
    }
    FILE* m_f = nullptr;
    bool m_bad = false;
    std::vector<Ent> m_e;
};

// ---- privacy scrubber: case-insensitive replace of each needle (longest first), with '\' and '/' spellings of paths ----------
class Scrubber {
public:
    void Add(const std::string& needle, const std::string& repl, size_t minLen = 3) {
        if (needle.size() < minLen) return;
        m_r.push_back({ needle, repl });
        std::string alt = needle;
        std::replace(alt.begin(), alt.end(), '\\', '/');
        if (alt != needle) m_r.push_back({ alt, repl });
        std::string dbl;                                    // JSON / C-escaped paths: C:\\Users\\name
        for (char c : needle) { dbl += c; if (c == '\\') dbl += '\\'; }
        if (dbl != needle) m_r.push_back({ dbl, repl });
        std::sort(m_r.begin(), m_r.end(), [](const Rule& a, const Rule& b) { return a.n.size() > b.n.size(); });
    }
    void AddExact(const std::string& needle, const std::string& repl) {   // this spelling only
        if (needle.size() < 2) return;
        m_r.push_back({ needle, repl });
        std::sort(m_r.begin(), m_r.end(), [](const Rule& a, const Rule& b) { return a.n.size() > b.n.size(); });
    }
    void AddWide(const wchar_t* needle, const std::string& repl, size_t minLen = 3) {
        if (!needle || !needle[0]) return;
        char b[1024];
        if (WideCharToMultiByte(CP_UTF8, 0, needle, -1, b, sizeof(b), nullptr, nullptr) > 0) Add(b, repl, minLen);
    }
    std::string Apply(const std::string& in) const {
        std::string s = in;
        for (const Rule& r : m_r) {
            size_t pos = 0;
            while ((pos = FindI(s, r.n, pos)) != std::string::npos) { s.replace(pos, r.n.size(), r.r); pos += r.r.size(); }
        }
        return s;
    }
    size_t Rules() const { return m_r.size(); }
private:
    struct Rule { std::string n, r; };
    static size_t FindI(const std::string& s, const std::string& n, size_t from) {
        if (n.empty() || s.size() < n.size()) return std::string::npos;
        for (size_t i = from; i + n.size() <= s.size(); ++i) {
            size_t k = 0;
            while (k < n.size() && tolower((unsigned char)s[i + k]) == tolower((unsigned char)n[k])) ++k;
            if (k == n.size()) return i;
        }
        return std::string::npos;
    }
    std::vector<Rule> m_r;
};

// The machine's personal strings: %USERPROFILE% (-> %USERPROFILE%), the Windows user name as a PATH COMPONENT (after a backslash
// or a slash -> \<user>, /<user>: a short name like "Max" must not turn every "max" of the log into <user>), the Steam library root of `exePath`
// (the part before \steamapps\ -> <steam>).
inline Scrubber MakeScrubber(const wchar_t* exePath) {
    Scrubber s;
    wchar_t b[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", b, MAX_PATH)) {
        s.AddWide(b, "%USERPROFILE%");
        // phase 94b (Fable audit): the 8.3 short form too (C:\Users\JOHNDO~1 -- TEMP is often short): the home folder, and its last
        // component as a path part (\JOHNDO~1 -> \<user>)
        wchar_t sh[MAX_PATH];
        const DWORD n8 = GetShortPathNameW(b, sh, MAX_PATH);
        if (n8 > 0 && n8 < MAX_PATH && _wcsicmp(sh, b) != 0) {
            s.AddWide(sh, "%USERPROFILE%");
            const wchar_t* last = wcsrchr(sh, L'\\');
            char u8[512];
            if (last && last[1] && WideCharToMultiByte(CP_UTF8, 0, last + 1, -1, u8, sizeof(u8), nullptr, nullptr) > 0) {
                s.AddExact(std::string("\\\\") + u8, "\\\\<user>");
                s.AddExact(std::string("\\") + u8, "\\<user>");
                s.AddExact(std::string("/") + u8, "/<user>");
            }
        }
    }
    if (exePath) {
        std::wstring e = exePath, lower = e;
        for (wchar_t& c : lower) c = (wchar_t)towlower(c);
        const size_t k = lower.find(L"\\steamapps\\");
        if (k != std::wstring::npos && k > 3) s.AddWide(e.substr(0, k).c_str(), "<steam>");
    }
    DWORD n = MAX_PATH;
    if (GetUserNameW(b, &n) && b[0]) {
        char u8[512];
        if (WideCharToMultiByte(CP_UTF8, 0, b, -1, u8, sizeof(u8), nullptr, nullptr) > 0) {
            s.AddExact(std::string("\\\\") + u8, "\\\\<user>");   // C-escaped path: two backslashes before the name
            s.AddExact(std::string("\\") + u8, "\\<user>");
            s.AddExact(std::string("/") + u8, "/<user>");
        }
    }
    return s;
}

// The last `maxLines` lines of a text (whole lines).
inline std::string TailLines(const std::string& s, size_t maxLines) {
    size_t n = 0, i = s.size();
    while (i > 0) {
        --i;
        if (s[i] == '\n' && i + 1 < s.size() && ++n >= maxLines) return s.substr(i + 1);
    }
    return s;
}

inline bool ReadFileText(const wchar_t* path, std::string& out, size_t maxBytes = 64u << 20) {
    FILE* f = _wfsopen(path, L"rb", _SH_DENYNO);
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    const long long len = _ftelli64(f);
    long long from = 0;
    if (len > (long long)maxBytes) from = len - (long long)maxBytes;   // the END of a big log
    _fseeki64(f, from, SEEK_SET);
    out.resize((size_t)(len - from));
    const size_t got = out.empty() ? 0 : fread(&out[0], 1, out.size(), f);
    out.resize(got);
    fclose(f);
    return true;
}
inline bool WriteFileText(const wchar_t* path, const std::string& s) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return false;
    const bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
    return fclose(f) == 0 && ok;
}

// ---- the capture plan -------------------------------------------------------------------------------------------------------
// keep = the player's own value. One set = `settle` frames after the switch (DLSS history refills / the view changes), then
// `frames` consecutive frames saved. phase 89e ORDER: the player's picture first, DLAA OFF second (the A/B a report needs most --
// a capture that stops early never loses it), then the rest. `diag` = the planes DLSS got for the first kDiagFrames frames.
enum { kKeep = -1 };
struct CapSet {
    const char* name;     // file prefix
    const char* what;     // README.txt / the status box
    int  dlaa;            // kKeep / 0 = DLAA off
    int  jitOnly, mvDebug, mvOff, partsOff;   // 1 = switched on for this set
    int  settle, frames;
    bool diag;
};
constexpr int kCaptureQuick = 1, kCaptureFull = 2;
constexpr int kDiagFrames = 3;                            // frames of the first set that also save their planes (Full)
inline const CapSet* Plan(int level, int* n) {
    static const CapSet kQuick[] = {
        { "on",  "your picture", kKeep, 0, 0, 0, 0, 0, 6, false },
        { "off", "DLAA off", 0, 0, 0, 0, 0, 2, 4, false },
    };
    static const CapSet kFull[] = {
        { "on",       "your picture + what DLSS got", kKeep, 0, 0, 0, 0, 0, 6, true },
        { "off",      "DLAA off", 0, 0, 0, 0, 0, 2, 4, false },
        { "mvview",   "motion-vector view", kKeep, 0, 1, 0, 0, 1, 2, false },
        { "partsoff", "moving parts off", kKeep, 0, 0, 0, 1, 16, 3, false },
        { "mvoff",    "motion vectors off", kKeep, 0, 0, 1, 0, 16, 3, false },
        { "jitonly",  "jitter only", kKeep, 1, 0, 0, 0, 2, 3, false },
    };
    if (level >= kCaptureFull) { *n = (int)(sizeof(kFull) / sizeof(kFull[0])); return kFull; }
    *n = (int)(sizeof(kQuick) / sizeof(kQuick[0]));
    return kQuick;
}

// ---- phase 89e: crops, exposure and MV scale of the saved pictures ---------------------------------------------------------------
struct Box { uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; uint32_t W() const { return x1 - x0; } uint32_t H() const { return y1 - y0; } };
// a maxW x maxH box (at most W x H) centred on (cu, cv) of a W x H picture, kept inside it; true = smaller than the picture
inline bool CentreCrop(uint32_t W, uint32_t H, uint32_t maxW, uint32_t maxH, float cu, float cv, Box* b) {
    const uint32_t w = W < maxW ? W : maxW, h = H < maxH ? H : maxH;
    long x = (long)std::lround(cu * (double)W - w / 2.0), y = (long)std::lround(cv * (double)H - h / 2.0);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + (long)w > (long)W) x = (long)(W - w);
    if (y + (long)h > (long)H) y = (long)(H - h);
    b->x0 = (uint32_t)x; b->y0 = (uint32_t)y; b->x1 = b->x0 + w; b->y1 = b->y0 + h;
    return w < W || h < H;
}
// the same scene part in a picture of another size (render -> output): the box scaled by the size ratio, rounded, clamped
inline Box ScaleBox(const Box& b, uint32_t fromW, uint32_t fromH, uint32_t toW, uint32_t toH) {
    Box o;
    auto sc = [](uint32_t v, uint32_t f, uint32_t t) { const uint64_t r = f ? ((uint64_t)v * t + f / 2) / f : v; return (uint32_t)(r > t ? t : r); };
    o.x0 = sc(b.x0, fromW, toW); o.x1 = sc(b.x1, fromW, toW); o.y0 = sc(b.y0, fromH, toH); o.y1 = sc(b.y1, fromH, toH);
    if (o.x1 <= o.x0) o.x1 = (o.x0 + 1 <= toW) ? o.x0 + 1 : toW;
    if (o.y1 <= o.y0) o.y1 = (o.y0 + 1 <= toH) ? o.y0 + 1 : toH;
    return o;
}
// the scale that puts the log-average luma at 0.18 (the "key" of a photographic tonemap) -- HDR planes have no exposure of their
// own (the game applies it in its tonemap); values <= 0 / NaN / Inf are skipped; clamped to 1e-3 .. 1e4; nothing usable = 1
inline float AutoExposure(const float* luma, size_t n) {
    double s = 0.0; size_t k = 0;
    for (size_t i = 0; i < n; ++i) {
        const float l = luma[i];
        if (!(l > 0.0f) || !(l < 1.0e30f)) continue;
        s += std::log(1.0e-4 + (double)l); ++k;
    }
    if (!k) return 1.0f;
    const double key = std::exp(s / (double)k);
    double e = 0.18 / key;
    if (e < 1.0e-3) e = 1.0e-3;
    if (e > 1.0e4) e = 1.0e4;
    return (float)e;
}
// the motion-vector picture's range: the 99th percentile of |mv| (px), at least 0.25 px (a still frame stays grey)
inline float MvScale99(std::vector<float> mags) {
    if (mags.empty()) return 0.25f;
    const size_t k = (size_t)((double)(mags.size() - 1) * 0.99);
    std::nth_element(mags.begin(), mags.begin() + (ptrdiff_t)k, mags.end());
    const float v = mags[k];
    return v > 0.25f ? v : 0.25f;
}
// which rewind frames are saved (0 = the oldest held, n - 1 = the last before the key): the newest `recent` all, then up to
// `older` spread evenly over the rest -- the moment before the key frame by frame + the second(s) before it, not 140 pictures
inline std::vector<int> RewindPick(int n, int recent, int older) {
    std::vector<int> v;
    if (n <= 0) return v;
    const int r = recent < n ? recent : n, rest = n - r;
    const int o = older < rest ? older : rest;
    for (int i = 0; i < o; ++i) v.push_back((int)((int64_t)i * rest / o));
    for (int i = rest; i < n; ++i) v.push_back(i);
    return v;
}
inline int PlanFrames(int level) {
    int n = 0, t = 0;
    const CapSet* p = Plan(level, &n);
    for (int i = 0; i < n; ++i) t += p[i].settle + p[i].frames;
    return t;
}

// ---- rewind length: seconds at the display rate, clamped -----------------------------------------------------------------------
constexpr float kRewindSteps[] = { 0.0f, 0.5f, 1.0f, 2.0f, 3.0f };
constexpr int kRewindNSteps = (int)(sizeof(kRewindSteps) / sizeof(kRewindSteps[0]));
inline float ClampRewindSecs(double v) {                  // the nearest step at or above v (0 = off; above 3 = 3)
    if (!(v > 0.0)) return 0.0f;
    for (float s : kRewindSteps) if (s >= (float)v - 1e-4f) return s;
    return kRewindSteps[kRewindNSteps - 1];
}
inline int RewindFramesFor(float secs, double hz, int maxFrames = 240) {
    if (!(secs > 0.0f)) return 0;
    if (!(hz > 1.0)) hz = 60.0;
    int n = (int)(secs * hz + 0.5);
    if (n < 2) n = 2;
    return n > maxFrames ? maxFrames : n;
}

// ---- keep the newest `keep` captures (shimmer_<date>_<time> folders and their .zip), 0 = keep all -----------------------------
inline bool DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(p); else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(dir.c_str()) != 0;
}
// phase 89g: the capture a zip belongs to: shimmer_<date>_<time>[_main|_full|_rewind][_partNN].zip -> shimmer_<date>_<time>
inline std::wstring CaptureStem(std::wstring n) {
    if (n.size() > 4 && !_wcsicmp(n.c_str() + n.size() - 4, L".zip")) n.resize(n.size() - 4);
    const size_t p = n.rfind(L"_part");
    if (p != std::wstring::npos && p + 5 < n.size()) {
        bool digits = true;
        for (size_t i = p + 5; i < n.size(); ++i) digits = digits && n[i] >= L'0' && n[i] <= L'9';
        if (digits) n.resize(p);
    }
    for (const wchar_t* k : { L"_main", L"_full", L"_rewind" }) {   // phase 89h: the zip's kind
        const size_t l = wcslen(k);
        if (n.size() > l && !_wcsicmp(n.c_str() + n.size() - l, k)) { n.resize(n.size() - l); break; }
    }
    return n;
}

// ---- phase 89g: the capture in GitHub-sized parts -------------------------------------------------------------------------
// GitHub issue attachments: at most 25 MB per file (many per comment; .zip allowed, .001 not). So the capture is packed into
// INDEPENDENT stored zips of at most kPartBytes each (every part opens alone; not a split archive), in priority order: part 1 gets
// the text files first, then on_0 / off_0, the rest of on / off / final / full, the rewind newest first, the rw_* planes, the
// other sets, the raw .bin last; first fit by size. A file bigger than one part (a raw 4K .bin) is cut into pieces
// name.part01, name.part02 ... (each its own entry in some part; README.txt: copy /b a.part01 + a.part02 a).
constexpr uint64_t kPartBytes = 24ull << 20;
constexpr uint64_t kEntryOverhead = 30 + 46 + 2 * 128;   // local + central header + the name twice (generous)
struct PackFile { std::string name; uint64_t size = 0; };
struct PackItem { size_t file = 0; uint64_t off = 0, len = 0; std::string entry; };   // entry = the name inside the zip
// lower = sooner; ties keep the name order (rewind_00 is the newest: its number is the frames before the key)
inline int PackPriority(const std::string& n) {
    auto ends = [&](const char* e) { const size_t l = strlen(e); return n.size() >= l && !_stricmp(n.c_str() + n.size() - l, e); };
    auto starts = [&](const char* p) { return !_strnicmp(n.c_str(), p, strlen(p)); };
    if (ends(".txt") || ends(".log") || ends(".ini")) return 0;
    if (n == "on_0.png" || n == "off_0.png") return 1;
    if (starts("on_") || starts("off_") || starts("final_") || starts("full_")) return 2;
    if (starts("rewind_")) return 3;
    if (starts("rw_") && ends(".png")) return 4;
    if (ends(".bin")) return 6;
    return 5;
}
// phase 89h (user 2026-11-01: "make it as easy as possible"): the zips are split BY KIND, so a player attaches one zip and the
// rest only when asked -- 0 main (the text files + your picture with DLAA on / off: always attach), 1 full (the capture_mode 2
// views: DLSS input / output, motion vectors, depth, jitter only ...), 2 rewind (rewind_* frames and rw_* planes, rewind on only).
// Each kind is packed on its own (PackParts) into shimmer_<time>_<kind>.zip, or _<kind>_part01.zip ... when it needs more.
enum { kGroupMain = 0, kGroupFull = 1, kGroupRewind = 2, kGroupCount = 3 };
inline int PackGroup(const std::string& n) {
    const int pr = PackPriority(n);
    if (pr <= 2) return kGroupMain;
    if (pr == 3 || (n.size() >= 3 && !_strnicmp(n.c_str(), "rw_", 3))) return kGroupRewind;
    return kGroupFull;
}
inline const wchar_t* PackGroupName(int g) { return g == kGroupMain ? L"main" : (g == kGroupFull ? L"full" : L"rewind"); }
// the zip's file name: <stem>_<kind>.zip (one part) or <stem>_<kind>_partNN.zip (index 0-based)
inline std::wstring PartZipName(const std::wstring& stem, int group, size_t index, size_t count) {
    wchar_t sfx[40];
    if (count <= 1) swprintf_s(sfx, L"_%ls.zip", PackGroupName(group));
    else swprintf_s(sfx, L"_%ls_part%02zu.zip", PackGroupName(group), index + 1);
    return stem + sfx;
}
// the parts: each a list of items; every byte of every file is in exactly one item; sum of (len + overhead) + 22 <= limit
inline std::vector<std::vector<PackItem>> PackParts(const std::vector<PackFile>& files, uint64_t limit = kPartBytes) {
    std::vector<size_t> order(files.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const int pa = PackPriority(files[a].name), pb = PackPriority(files[b].name);
        if (pa != pb) return pa < pb;
        return files[a].name < files[b].name;
    });
    const uint64_t room = limit > kEntryOverhead + 22 + 1024 ? limit - kEntryOverhead - 22 - 1024 : limit / 2;   // max piece
    std::vector<PackItem> items;
    for (size_t i : order) {
        const uint64_t sz = files[i].size;
        if (sz <= room) { items.push_back(PackItem{ i, 0, sz, files[i].name }); continue; }
        int k = 1;
        for (uint64_t off = 0; off < sz; off += room, ++k) {
            char sfx[16]; snprintf(sfx, sizeof(sfx), ".part%02d", k);
            items.push_back(PackItem{ i, off, (sz - off) < room ? (sz - off) : room, files[i].name + sfx });
        }
    }
    std::vector<std::vector<PackItem>> parts;
    std::vector<uint64_t> used;
    for (const PackItem& it : items) {
        const uint64_t need = it.len + kEntryOverhead;
        size_t p = 0;
        while (p < parts.size() && (used[p] + need + 22 > limit || parts[p].size() >= 65000)) ++p;   // first fit
        if (p == parts.size()) { parts.emplace_back(); used.push_back(0); }
        parts[p].push_back(it);
        used[p] += need;
    }
    return parts;
}

// `root` = the dlaa_selftest folder (no trailing slash). Returns the captures removed.
inline int Prune(const std::wstring& root, int keep) {
    if (keep <= 0) return 0;
    std::vector<std::wstring> stems;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\shimmer_*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        std::wstring n = fd.cFileName;
        if (n.size() > 4 && !_wcsicmp(n.c_str() + n.size() - 4, L".zip")) n = CaptureStem(n);   // phase 89g: _partNN.zip too
        else if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (std::find(stems.begin(), stems.end(), n) == stems.end()) stems.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(stems.begin(), stems.end());                // shimmer_YYYYMMDD_HHMMSS: name order = time order
    int removed = 0;
    for (size_t i = 0; i + (size_t)keep < stems.size(); ++i) {
        const std::wstring p = root + L"\\" + stems[i];
        DeleteTree(p);
        DeleteFileW((p + L".zip").c_str());
        WIN32_FIND_DATAW zd;                              // phase 89g / 89h: every part / kind zip of that capture
        HANDLE zh = FindFirstFileW((p + L"_*.zip").c_str(), &zd);
        if (zh != INVALID_HANDLE_VALUE) {
            do { DeleteFileW((root + L"\\" + zd.cFileName).c_str()); } while (FindNextFileW(zh, &zd));
            FindClose(zh);
        }
        ++removed;
    }
    return removed;
}
inline uint64_t DirBytes(const std::wstring& dir) {
    uint64_t t = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) t += DirBytes(dir + L"\\" + n);
        else t += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return t;
}

// ---- the capture key's accidental-press guard: two presses within `windowMs` ---------------------------------------------------
struct DoublePress {
    uint64_t armedMs = 0;
    // true = this press is the second one (capture); false = armed (the first one) -- *armed tells which
    bool Press(uint64_t nowMs, bool* armed, uint64_t windowMs = 1000) {
        if (armedMs && nowMs - armedMs <= windowMs) { armedMs = 0; if (armed) *armed = false; return true; }
        armedMs = nowMs ? nowMs : 1;
        if (armed) *armed = true;
        return false;
    }
};

// ---- dlaa.ini capture_keep steps ---------------------------------------------------------------------------------------------
constexpr int kKeepSteps[] = { 3, 5, 10, 0 };              // 0 = all
inline int ClampKeep(long v) {                            // review fix: above 10 = 10 (was: 0 = keep all)
    if (v <= 0) return 0;
    for (int s : { 3, 5, 10 }) if (v <= s) return s;
    return 10;
}

}  // namespace BugReport
