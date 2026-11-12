// phase 89b-d unit test: src/bug_report.h (no game, no GPU) -- CRC-32, the stored ZIP writer (read back: headers, CRCs, sizes,
// the central directory), the privacy scrubber, TailLines, the capture plan, the rewind length, capture_keep pruning, the
// capture key's double press. Exit code 0 = all passed.
#include "bug_report.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static uint32_t Get32(const std::string& s, size_t o) { return (uint8_t)s[o] | ((uint8_t)s[o + 1] << 8) | ((uint8_t)s[o + 2] << 16) | ((uint32_t)(uint8_t)s[o + 3] << 24); }
static uint16_t Get16(const std::string& s, size_t o) { return (uint16_t)((uint8_t)s[o] | ((uint8_t)s[o + 1] << 8)); }

int main() {
    CHECK(BugReport::Crc32("123456789", 9) == 0xCBF43926u, "CRC-32 check value");
    CHECK(BugReport::Crc32("6789", 4, BugReport::Crc32("12345", 5)) == 0xCBF43926u, "CRC-32 in pieces");

    // ---- zip ----
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = std::wstring(tmp) + L"bug_report_test";
    BugReport::DeleteTree(root);
    CreateDirectoryW(root.c_str(), nullptr);
    const std::wstring src = root + L"\\a.txt", zip = root + L"\\t.zip";
    std::string big(200000, 'x');
    for (size_t i = 0; i < big.size(); ++i) big[i] = (char)('a' + (i * 7) % 26);
    CHECK(BugReport::WriteFileText(src.c_str(), big), "write a.txt");
    {
        BugReport::Zip z;
        CHECK(z.Open(zip.c_str()), "zip open");
        CHECK(z.AddFile("dir/a.txt", src.c_str()), "zip add file");
        CHECK(z.AddData("dir/b.txt", "hello", 5), "zip add data");
        CHECK(!z.AddFile("dir/missing.txt", (root + L"\\nope").c_str()), "a missing file is refused");
        CHECK(z.Close() && z.Count() == 2, "zip close, 2 entries");
    }
    std::string zs;
    CHECK(BugReport::ReadFileText(zip.c_str(), zs), "read zip");
    CHECK(zs.size() > 22 && Get32(zs, 0) == 0x04034b50u, "local header signature");
    const size_t eocd = zs.size() - 22;
    CHECK(Get32(zs, eocd) == 0x06054b50u && Get16(zs, eocd + 10) == 2, "end of central directory, 2 entries");
    size_t cd = Get32(zs, eocd + 16);
    const char* names[2] = { "dir/a.txt", "dir/b.txt" };
    const std::string datas[2] = { big, "hello" };
    for (int i = 0; i < 2; ++i) {
        CHECK(Get32(zs, cd) == 0x02014b50u, "central entry %d signature", i);
        const uint32_t crc = Get32(zs, cd + 16), size = Get32(zs, cd + 20), off = Get32(zs, cd + 42);
        const uint16_t nl = Get16(zs, cd + 28);
        CHECK(zs.substr(cd + 46, nl) == names[i], "entry %d name", i);
        CHECK(size == datas[i].size() && crc == BugReport::Crc32(datas[i].data(), datas[i].size()), "entry %d size / crc", i);
        CHECK(Get32(zs, off) == 0x04034b50u && Get32(zs, off + 14) == crc && Get32(zs, off + 18) == size, "entry %d local header patched", i);
        const uint16_t lnl = Get16(zs, off + 26);
        CHECK(zs.substr(off + 30 + lnl, size) == datas[i], "entry %d data", i);
        cd += 46 + nl;
    }

    // ---- scrubber ----
    BugReport::Scrubber sc;
    sc.Add("C:\\Users\\Jane Doe", "%USERPROFILE%");
    sc.Add("Jane Doe", "<user>");
    sc.Add("D:\\SteamLibrary", "<steam>");
    sc.Add("ab", "<x>");                                  // too short: ignored
    const std::string in = "log c:\\users\\jane doe\\Documents\\x C:/Users/Jane Doe/y C:\\\\Users\\\\Jane Doe\\\\z jane doe "
                           "D:\\SteamLibrary\\steamapps\\common\\ab";
    const std::string out = sc.Apply(in);
    CHECK(out.find("Jane") == std::string::npos && out.find("jane") == std::string::npos, "user name gone: %s", out.c_str());
    CHECK(out.find("%USERPROFILE%\\Documents") != std::string::npos && out.find("%USERPROFILE%/y") != std::string::npos &&
          out.find("%USERPROFILE%\\\\z") != std::string::npos, "home folder in all three spellings: %s", out.c_str());
    CHECK(out.find("<user>") != std::string::npos && out.find("<steam>\\steamapps") != std::string::npos && out.find("common\\ab") != std::string::npos,
          "user / steam / short needle: %s", out.c_str());
    {
        const BugReport::Scrubber real = BugReport::MakeScrubber(L"D:\\Games\\SteamLib\\steamapps\\common\\x\\bin\\win_x64\\eurotrucks2.exe");
        wchar_t up[MAX_PATH]; GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
        char u8[MAX_PATH]; WideCharToMultiByte(CP_UTF8, 0, up, -1, u8, MAX_PATH, nullptr, nullptr);
        const std::string t = real.Apply(std::string("path ") + u8 + "\\x and D:\\Games\\SteamLib\\steamapps");
        CHECK(t.find(u8) == std::string::npos && t.find("<steam>\\steamapps") != std::string::npos, "MakeScrubber: %s", t.c_str());
        wchar_t un[256]; DWORD n = 256; GetUserNameW(un, &n);
        char un8[256]; WideCharToMultiByte(CP_UTF8, 0, un, -1, un8, 256, nullptr, nullptr);
        const std::string t2 = real.Apply(std::string("E:\\Other\\") + un8 + "\\file and /" + un8 + "/x and " + un8 + "imum");
        CHECK(t2.find(std::string("\\") + un8) == std::string::npos && t2.find(std::string("/") + un8) == std::string::npos &&
              t2.find(std::string(un8) + "imum") != std::string::npos, "MakeScrubber: user name as a path component only: %s", t2.c_str());
    }
    CHECK(BugReport::TailLines("a\nb\nc\nd\n", 2) == "c\nd\n", "TailLines 2");
    CHECK(BugReport::TailLines("a\nb\n", 5) == "a\nb\n", "TailLines all");

    // ---- plan ----
    for (int level : { 1, 2 }) {
        int n = 0;
        const BugReport::CapSet* p = BugReport::Plan(level, &n);
        CHECK(n >= 2 && p[0].settle == 0 && p[0].dlaa == BugReport::kKeep && !p[0].jitOnly && !p[0].mvOff && !p[0].partsOff && !p[0].mvDebug, "level %d: set 0 = the player's picture, no settle", level);
        bool off = false;
        for (int i = 0; i < n; ++i) { CHECK(p[i].frames > 0, "level %d set %d frames", level, i); off |= p[i].dlaa == 0; }
        CHECK(off, "level %d has a DLAA-off set", level);
        CHECK(p[0].diag == (level >= 2), "diag planes only in Full");
        // phase 89e: DLAA OFF is the SECOND set (a capture that stops early never loses it)
        CHECK(!strcmp(p[0].name, "on") && !strcmp(p[1].name, "off") && p[1].dlaa == 0, "level %d: on, then off", level);
    }
    CHECK(BugReport::PlanFrames(1) <= 20, "quick plan is short: %d frames", BugReport::PlanFrames(1));
    CHECK(BugReport::PlanFrames(2) <= 120, "full plan: %d frames (< ~1.7 s at 72 fps)", BugReport::PlanFrames(2));

    // ---- phase 89e: crops, exposure, MV range, the rewind pick ----
    {
        BugReport::Box b;
        CHECK(BugReport::CentreCrop(3840, 2160, 1920, 1080, 0.5f, 0.5f, &b) && b.x0 == 960 && b.y0 == 540 && b.W() == 1920 && b.H() == 1080,
              "4K centre crop: (%u,%u) %ux%u", b.x0, b.y0, b.W(), b.H());
        CHECK(!BugReport::CentreCrop(1920, 1080, 1920, 1080, 0.5f, 0.5f, &b) && b.x0 == 0 && b.W() == 1920, "1080p: no crop");
        CHECK(BugReport::CentreCrop(2761, 2928, 1440, 1440, 0.5f, 0.5f, &b) && b.W() == 1440 && b.H() == 1440 && b.x0 == 661 && b.y0 == 744,
              "VR eye crop: (%u,%u)", b.x0, b.y0);
        CHECK(BugReport::CentreCrop(3840, 2160, 1920, 1080, 0.99f, 0.0f, &b) && b.x1 == 3840 && b.y0 == 0, "an off-centre crop stays inside");
        CHECK(BugReport::CentreCrop(800, 600, 1920, 1080, 0.5f, 0.5f, &b) == false && b.W() == 800 && b.H() == 600, "small picture: whole");
        BugReport::Box r{ 960, 540, 2880, 1620 };
        const BugReport::Box o = BugReport::ScaleBox(r, 3840, 2160, 2880, 1620);     // render 4K -> 0.75 x
        CHECK(o.x0 == 720 && o.y0 == 405 && o.x1 == 2160 && o.y1 == 1215, "ScaleBox: (%u,%u)-(%u,%u)", o.x0, o.y0, o.x1, o.y1);
        std::vector<float> l(1000, 0.045f);
        CHECK(std::fabs(BugReport::AutoExposure(l.data(), l.size()) - 4.0f) < 0.05f, "exposure of a 0.045 scene: x%.3f (4)",
              (double)BugReport::AutoExposure(l.data(), l.size()));
        l.assign(1000, 0.0f);
        CHECK(BugReport::AutoExposure(l.data(), l.size()) == 1.0f, "an all-black plane: exposure 1");
        std::vector<float> m(1000, 0.0f);
        CHECK(BugReport::MvScale99(m) == 0.25f, "a still frame: MV range 0.25 px (grey)");
        for (int i = 0; i < 1000; ++i) m[(size_t)i] = (float)i / 100.0f;          // 0 .. 9.99 px
        CHECK(std::fabs(BugReport::MvScale99(m) - 9.89f) < 0.02f, "MV range = the 99th percentile: %.2f", (double)BugReport::MvScale99(m));
        const std::vector<int> pk = BugReport::RewindPick(140, 12, 12);
        CHECK(pk.size() == 24 && pk.back() == 139 && pk[12] == 128 && pk[0] == 0, "rewind pick of 140: %zu frames", pk.size());
        bool sorted = true;
        for (size_t i = 1; i < pk.size(); ++i) sorted &= pk[i] > pk[i - 1];
        CHECK(sorted, "rewind pick: increasing, no duplicates");
        CHECK(BugReport::RewindPick(10, 12, 12).size() == 10, "rewind pick of 10: all");
    }

    // ---- rewind / keep / guard ----
    CHECK(BugReport::RewindFramesFor(0.0f, 72) == 0 && BugReport::RewindFramesFor(0.5f, 72) == 36 && BugReport::RewindFramesFor(3.0f, 90) == 240 &&
          BugReport::RewindFramesFor(1.0f, 0) == 60, "RewindFramesFor");
    CHECK(BugReport::ClampRewindSecs(0.7) == 1.0f && BugReport::ClampRewindSecs(-1) == 0.0f && BugReport::ClampRewindSecs(9) == 3.0f &&
          BugReport::ClampRewindSecs(0.5) == 0.5f, "ClampRewindSecs");
    CHECK(BugReport::ClampKeep(4) == 5 && BugReport::ClampKeep(0) == 0 && BugReport::ClampKeep(3) == 3 && BugReport::ClampKeep(50) == 10 && BugReport::ClampKeep(7) == 10, "ClampKeep");
    CHECK(BugReport::Zip::FitsZip32(100ull << 20, 300) && !BugReport::Zip::FitsZip32(5ull << 30, 10) && !BugReport::Zip::FitsZip32(1000, 70000),
          "FitsZip32: 100 MB ok, 5 GB / 70000 files refused");
    {
        BugReport::DoublePress g;
        bool armed = false;
        CHECK(!g.Press(10000, &armed) && armed, "first press arms");
        CHECK(g.Press(10900, &armed) && !armed, "second within 1 s fires");
        CHECK(!g.Press(20000, &armed) && armed && !g.Press(21500, &armed) && armed, "too slow: re-arms");
        CHECK(g.Press(22000, &armed), "then fires");
    }
    {   // prune: 6 captures (folder + zip, one folder alone, one zip alone), keep 3 -> the 3 newest remain
        const wchar_t* stems[] = { L"shimmer_20261031_100000", L"shimmer_20261031_100100", L"shimmer_20261031_100200",
                                   L"shimmer_20261031_100300", L"shimmer_20261031_100400", L"shimmer_20261031_100500" };
        for (int i = 0; i < 6; ++i) {
            const std::wstring p = root + L"\\" + stems[i];
            if (i != 2) { CreateDirectoryW(p.c_str(), nullptr); BugReport::WriteFileText((p + L"\\f.png").c_str(), "x"); }
            if (i != 4) BugReport::WriteFileText((p + L".zip").c_str(), "z");
        }
        CHECK(BugReport::Prune(root, 3) == 3, "prune removes 3");
        for (int i = 0; i < 6; ++i) {
            const std::wstring p = root + L"\\" + stems[i];
            const bool any = GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES || GetFileAttributesW((p + L".zip").c_str()) != INVALID_FILE_ATTRIBUTES;
            CHECK(any == (i >= 3), "capture %d %s", i, any ? "kept" : "removed");
        }
        CHECK(BugReport::Prune(root, 0) == 0, "keep all");
        CHECK(GetFileAttributesW(src.c_str()) != INVALID_FILE_ATTRIBUTES, "other files untouched");
    }
    // ---- phase 89g: GitHub-sized parts (PackParts) ----
    {
        const uint64_t MB = 1ull << 20;
        std::vector<BugReport::PackFile> fl = {
            { "rewind_05.png", 9 * MB }, { "mv_0.bin", 66 * MB }, { "mvoff_0.png", 9 * MB }, { "sysinfo.txt", 4000 },
            { "off_1.png", 9 * MB }, { "on_0.png", 9 * MB }, { "rw_mv_00.png", 5 * MB }, { "dlaa_inject.log", 900000 },
            { "rewind_00.png", 9 * MB }, { "off_0.png", 9 * MB }, { "README.txt", 3000 }, { "info.txt", 9000 }, { "dlaa.ini", 20000 },
            { "on_1.png", 9 * MB }, { "full_on_0.png", 30 * MB } };
        const auto parts = BugReport::PackParts(fl);
        bool sizeOk = true, cover = true;
        std::vector<uint64_t> got(fl.size(), 0);
        for (const auto& p : parts) {
            uint64_t sum = 22;
            for (const auto& it : p) { sum += it.len + BugReport::kEntryOverhead; got[it.file] += it.len; }
            sizeOk &= sum <= BugReport::kPartBytes;
        }
        for (size_t i = 0; i < fl.size(); ++i) cover &= got[i] == fl[i].size;
        CHECK(sizeOk, "every part <= 24 MB");
        CHECK(cover, "every byte of every file is in exactly one piece");
        bool textIn1 = true;
        for (size_t i = 0; i < fl.size(); ++i) {
            const bool text = BugReport::PackPriority(fl[i].name) == 0;
            bool in1 = false;
            for (const auto& it : parts[0]) in1 |= it.file == i;
            if (text) textIn1 &= in1;
        }
        CHECK(textIn1, "part 1 has every text file");
        CHECK(parts[0].size() >= 7 && fl[parts[0][5].file].name == "off_0.png" && fl[parts[0][6].file].name == "on_0.png",
              "part 1: the 5 text files, then off_0 / on_0 (%s, %s)", fl[parts[0][5].file].name.c_str(), fl[parts[0][6].file].name.c_str());
        int bigPieces = 0; bool bigNames = true;
        for (const auto& p : parts) for (const auto& it : p) if (fl[it.file].name == "mv_0.bin") { ++bigPieces; bigNames &= it.entry.find("mv_0.bin.part0") == 0; }
        CHECK(bigPieces == 3 && bigNames, "the 66 MB .bin is cut into 3 pieces mv_0.bin.part01.. (%d)", bigPieces);
        int fullPieces = 0;
        for (const auto& p : parts) for (const auto& it : p) fullPieces += fl[it.file].name == "full_on_0.png";
        CHECK(fullPieces == 2, "the 30 MB picture: 2 pieces (%d)", fullPieces);
        // rewind newest first: rewind_00 is placed in an earlier or the same part as rewind_05, before the other sets / .bin
        auto partOf = [&](const char* n) { for (size_t p = 0; p < parts.size(); ++p) for (const auto& it : parts[p]) if (fl[it.file].name == n) return (int)p; return -1; };
        CHECK(partOf("rewind_00.png") <= partOf("rewind_05.png") && partOf("rewind_05.png") <= partOf("mvoff_0.png"), "rewind newest first");
        CHECK(BugReport::PackParts(std::vector<BugReport::PackFile>{ { "a.txt", 10 } }).size() == 1, "a tiny capture: one part");
        CHECK(BugReport::CaptureStem(L"shimmer_20261031_211900_part03.zip") == L"shimmer_20261031_211900" &&
              BugReport::CaptureStem(L"shimmer_20261031_211900.zip") == L"shimmer_20261031_211900", "CaptureStem");
        // phase 89h: zips by kind
        CHECK(BugReport::CaptureStem(L"shimmer_20261031_211900_main.zip") == L"shimmer_20261031_211900" &&
              BugReport::CaptureStem(L"shimmer_20261031_211900_rewind_part02.zip") == L"shimmer_20261031_211900" &&
              BugReport::CaptureStem(L"shimmer_20261031_211900_full_part11.zip") == L"shimmer_20261031_211900", "CaptureStem by kind");
        CHECK(BugReport::PackGroup("dlaa_inject.log") == BugReport::kGroupMain && BugReport::PackGroup("on_3.png") == BugReport::kGroupMain &&
              BugReport::PackGroup("full_on_0.png") == BugReport::kGroupMain && BugReport::PackGroup("off_0.png") == BugReport::kGroupMain &&
              BugReport::PackGroup("rewind_00.png") == BugReport::kGroupRewind && BugReport::PackGroup("rw_mv_00.bin") == BugReport::kGroupRewind &&
              BugReport::PackGroup("rw_depth_01.png") == BugReport::kGroupRewind && BugReport::PackGroup("mv_0.bin") == BugReport::kGroupFull &&
              BugReport::PackGroup("ngx_out_1.png") == BugReport::kGroupFull && BugReport::PackGroup("jitonly_0.png") == BugReport::kGroupFull,
              "PackGroup");
        CHECK(BugReport::PartZipName(L"d/s", BugReport::kGroupMain, 0, 1) == L"d/s_main.zip" &&
              BugReport::PartZipName(L"d/s", BugReport::kGroupRewind, 1, 3) == L"d/s_rewind_part02.zip", "PartZipName");
    }
    {   // the same, really written: small limit, files cut, every zip <= the limit, every piece read back = the file's bytes
        const std::wstring d = root + L"\\pack\\";
        CreateDirectoryW(root.c_str(), nullptr);
        CreateDirectoryW(d.c_str(), nullptr);
        std::vector<BugReport::PackFile> fl;
        std::vector<std::string> data;
        const size_t sizes[] = { 300, 70000, 150000, 9000, 260000 };
        const char* names[] = { "info.txt", "on_0.png", "mv_0.bin", "off_0.png", "rewind_00.png" };
        for (int i = 0; i < 5; ++i) {
            std::string b(sizes[i], 0);
            for (size_t k = 0; k < b.size(); ++k) b[k] = (char)((k * 31 + i * 7) & 0xFF);
            BugReport::WriteFileText((d + std::wstring(names[i], names[i] + strlen(names[i]))).c_str(), b);
            fl.push_back({ names[i], b.size() });
            data.push_back(b);
        }
        const uint64_t lim = 100000;
        const auto parts = BugReport::PackParts(fl, lim);
        std::vector<std::string> joined(fl.size());
        bool ok = true;
        for (size_t p = 0; p < parts.size(); ++p) {
            const std::wstring zp = root + L"\\pack_part" + std::to_wstring(p + 1) + L".zip";
            BugReport::Zip z;
            ok &= z.Open(zp.c_str());
            for (const auto& it : parts[p]) {
                const std::wstring src = d + std::wstring(fl[it.file].name.begin(), fl[it.file].name.end());
                ok &= (it.off == 0 && it.len == fl[it.file].size) ? z.AddFile(it.entry.c_str(), src.c_str())
                                                                  : z.AddFileRange(it.entry.c_str(), src.c_str(), it.off, it.len);
            }
            ok &= z.Close();
            std::string zs;
            BugReport::ReadFileText(zp.c_str(), zs);
            CHECK(zs.size() <= lim, "part %zu: %zu bytes <= %llu", p + 1, zs.size(), (unsigned long long)lim);
            size_t cd = Get32(zs, zs.size() - 22 + 16);
            for (size_t e = 0; e < parts[p].size(); ++e) {
                const uint32_t crc = Get32(zs, cd + 16), size = Get32(zs, cd + 20), off = Get32(zs, cd + 42);
                const uint16_t nl = Get16(zs, cd + 28), lnl = Get16(zs, off + 26);
                const std::string body = zs.substr(off + 30 + lnl, size);
                ok &= BugReport::Crc32(body.data(), body.size()) == crc && zs.substr(cd + 46, nl) == parts[p][e].entry;
                {   // a piece goes back at its offset (first fit may put a later, smaller piece into an earlier part)
                    std::string& j = joined[parts[p][e].file];
                    j.resize((size_t)fl[parts[p][e].file].size);
                    j.replace((size_t)parts[p][e].off, body.size(), body);
                }
                cd += 46 + nl;
            }
        }
        CHECK(ok, "the parts were written and read back (headers, CRCs, names)");
        bool same = true;
        for (size_t i = 0; i < fl.size(); ++i) same &= joined[i] == data[i];
        CHECK(same, "every file joined from its pieces = the original bytes");
        {   // capture_keep with part zips: a capture = its folder + every _partNN.zip
            const std::wstring pr = root + L"\\prune";
            CreateDirectoryW(pr.c_str(), nullptr);
            for (const wchar_t* st : { L"shimmer_20261101_100000", L"shimmer_20261101_110000" }) {
                CreateDirectoryW((pr + L"\\" + st).c_str(), nullptr);
                for (const wchar_t* pt : { L"_part01.zip", L"_part02.zip", L"_part03.zip" })
                    BugReport::WriteFileText((pr + L"\\" + st + pt).c_str(), "z");
            }
            CHECK(BugReport::Prune(pr, 1) == 1, "prune with part zips: 1 capture removed");
            CHECK(GetFileAttributesW((pr + L"\\shimmer_20261101_100000_part02.zip").c_str()) == INVALID_FILE_ATTRIBUTES &&
                  GetFileAttributesW((pr + L"\\shimmer_20261101_100000").c_str()) == INVALID_FILE_ATTRIBUTES &&
                  GetFileAttributesW((pr + L"\\shimmer_20261101_110000_part03.zip").c_str()) != INVALID_FILE_ATTRIBUTES,
                  "the old capture's folder + all parts gone, the new one kept");
        }
        CHECK(parts.size() >= 5, "a 489 KB capture at a 100 KB limit: %zu parts", parts.size());
    }
    BugReport::DeleteTree(root);
    std::printf("bug_report_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
