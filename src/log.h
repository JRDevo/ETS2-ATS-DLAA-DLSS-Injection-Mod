#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <share.h>

// Tiny file logger. Writes "dlaa_inject.log" next to the game executable
// (i.e. inside bin\win_x64\). If that dir is read-only, change the path below.
// Opened with _SH_DENYNO so other processes can read it while the game runs.
// v0.7.5: the log is only written when dlaa.ini (same folder) has `debug = 1` (also true / yes / on). Checked
// once, at the first Log call; without it no file is created and every Log call returns at once.
inline bool LogDebugEnabled(const char* exeDirSlash) {
    char path[MAX_PATH];
    strcpy_s(path, exeDirSlash);
    strcat_s(path, "dlaa.ini");
    FILE* ini = _fsopen(path, "r", _SH_DENYNO);
    if (!ini) return false;
    bool on = false;
    char line[256];
    while (fgets(line, sizeof(line), ini)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (_strnicmp(p, "debug", 5) != 0) continue;
        p += 5;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '=') continue;
        ++p;
        while (*p == ' ' || *p == '\t') ++p;
        on = *p == '1' || !_strnicmp(p, "true", 4) || !_strnicmp(p, "yes", 3) || !_strnicmp(p, "on", 2);
    }
    fclose(ini);
    return on;
}

inline void LogImpl(const char* fmt, ...) {
    static std::mutex m;
    static FILE* f = nullptr;
    static int enabled = -1;                              // -1 = not checked yet
    std::lock_guard<std::mutex> lk(m);
    if (enabled == 0) return;
    if (!f) {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);      // full path to the game .exe
        char* slash = strrchr(path, '\\');
        if (!slash) { enabled = 0; return; }
        slash[1] = 0;
        if (enabled < 0) enabled = LogDebugEnabled(path) ? 1 : 0;
        if (!enabled) return;
        strcat_s(path, "dlaa_inject.log");
        f = _fsopen(path, "w", _SH_DENYNO);
        if (!f) return;
    }
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fflush(f);
}
#define Log(...) LogImpl(__VA_ARGS__)
