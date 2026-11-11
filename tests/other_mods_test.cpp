// Phase 94 unit test of src/other_mods.h: which game.log.txt lines name a mod or a plugin, and the once-per-run filter.
// Build: tests\build_other_mods_test.bat   Run: build\tests\other_mods_test.exe   (exit code 0 = all checks passed)
#include "other_mods.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    const std::string log =
        "00:00:04.013 : [mod_package_manager] Requesting Steam Workshop content..\r\n"
        "00:00:04.082 : uset r_taa_modulated_drr_strength \"0.0\"\r\n"
        "00:00:04.083 : uset g_plugin_thing \"1\"\r\n"
        "00:00:05.100 : Loading telemetry plugin 'C:/Games/ATS/bin/win_x64/plugins/simhub.dll'\r\n"
        "00:00:14.132 : [mods] Active 1 mods (local: 0, workshop: 1)\r\n"
        "00:00:14.132 : [mods] Active workshop mod ID 1942968500 (name: GTA V Traffic Pack, version: 4.6, author: x)\r\n"
        "00:00:14.405 : [fs] device D:/Games/Steam/steamapps/workshop/content/270880/1942968500/universal.zip mounted to mod pool.\r\n"
        "[mods] no time stamp\n"
        "\n";
    const std::vector<std::string> v = OtherModsText::ModLines(log);
    CHECK(v.size() == 4);
    if (v.size() == 4) {
        CHECK(v[0] == "Loading telemetry plugin 'C:/Games/ATS/bin/win_x64/plugins/simhub.dll'");
        CHECK(v[1] == "[mods] Active 1 mods (local: 0, workshop: 1)");
        CHECK(v[2].find("1942968500") != std::string::npos && v[2].compare(0, 6, "[mods]") == 0);
        CHECK(v[3] == "[mods] no time stamp");
    }
    CHECK(OtherModsText::ModLines("").empty());
    CHECK(OtherModsText::ModLines(std::string(700, 'x') + "PLUGIN").size() == 1);
    CHECK(OtherModsText::ModLines(std::string(700, 'x') + "PLUGIN")[0].size() == 603);   // cut at 600 + "..."
    std::vector<std::string> seen;
    CHECK(OtherModsText::FirstTime(seen, "a"));
    CHECK(!OtherModsText::FirstTime(seen, "a"));
    CHECK(OtherModsText::FirstTime(seen, "b"));
    printf(g_fail ? "other_mods_test: %d FAILED\n" : "other_mods_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
