#pragma once
#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <atomic>
#include <share.h>
#include <string>
#include <vector>
#include "cpu_prof.h"                                // phase 31: the "log" row of the CPU profiler

// Tiny file logger. Writes "dlaa_inject.log" next to the game executable
// (i.e. inside bin\win_x64\). If that dir is read-only, change the path below.
// Opened with _SH_DENYNO so other processes can read it while the game runs.
// v0.7.5: the log was only written when dlaa.ini (same folder) had `debug = 1` (also true / yes / on).
// phase 39: LOG LEVELS. dlaa.ini log_level: 0 off, 1 errors (the default), 2 warnings, 3 info, 4 debug (a number, or the
// level's name). Each line has a level -- LogErr (1), LogWarn (2), Log (3, info), LogDbg (4) -- and is written only when it
// is <= the current level. Compatibility: `debug = 1` (true / yes / on) WITHOUT a log_level key = level 4 (what debug = 1
// logged until phase 38); `debug = 0` has no effect (log_level decides). dlaa.ini is read ONCE, at the first Log call
// (LogLevelFromIni; inject.cpp's LoadConfig parses the same two keys with the same LogParseLevel / LogParseOn, so both agree);
// the tuning menu's Logging row changes the level live (LogSetLevel). The file is created at the first line that passes the
// level: an "errors" user without an error gets no file at all.
inline const char* LogLevelName(int l) {
    static const char* const kN[5] = { "off", "errors", "warnings", "info", "debug" };
    return l >= 0 && l <= 4 ? kN[l] : "?";
}
// The value of a log_level line: a number (clamped to 0..4) or a name (off / none, error(s), warn(ing(s)), info, debug);
// -1 = not recognised (the key is then ignored).
inline int LogParseLevel(const char* p) {
    while (*p == ' ' || *p == '\t') ++p;
    if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+') {
        const long v = strtol(p, nullptr, 10);
        return v < 0 ? 0 : (v > 4 ? 4 : (int)v);
    }
    if (!_strnicmp(p, "off", 3) || !_strnicmp(p, "none", 4)) return 0;
    if (!_strnicmp(p, "err", 3))   return 1;
    if (!_strnicmp(p, "warn", 4))  return 2;
    if (!_strnicmp(p, "info", 4))  return 3;
    if (!_strnicmp(p, "debug", 5)) return 4;
    return -1;
}
// The value of a debug line: 1 / true / yes / on (the v0.7.5 rule).
inline bool LogParseOn(const char* p) {
    while (*p == ' ' || *p == '\t') ++p;
    return *p == '1' || !_strnicmp(p, "true", 4) || !_strnicmp(p, "yes", 3) || !_strnicmp(p, "on", 2);
}
// dlaa.ini in `exeDirSlash` -> the level: log_level when it has one, else 4 with debug = 1, else 1 (also without a dlaa.ini).
inline int LogLevelFromIni(const char* exeDirSlash) {
    char path[MAX_PATH];
    strcpy_s(path, exeDirSlash);
    strcat_s(path, "dlaa.ini");
    FILE* ini = _fsopen(path, "r", _SH_DENYNO);
    if (!ini) return 1;
    int lvl = -1;
    bool dbg = false;
    char line[256];
    while (fgets(line, sizeof(line), ini)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        const bool isLvl = !_strnicmp(p, "log_level", 9);
        if (!isLvl && _strnicmp(p, "debug", 5) != 0) continue;
        p += isLvl ? 9 : 5;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '=') continue;
        ++p;
        if (isLvl) { const int v = LogParseLevel(p); if (v >= 0) lvl = v; }
        else       dbg = LogParseOn(p);
    }
    fclose(ini);
    return lvl >= 0 ? lvl : (dbg ? 4 : 1);
}

// v0.7.10: set by the DLL_PROCESS_DETACH exit line. At process exit the other threads are already gone; one of them
// may have died inside Log with the mutex held, so the exit line only try-locks (a blocked lock = the game hangs on quit).
inline std::atomic<bool>& LogExiting() { static std::atomic<bool> b{false}; return b; }

// phase 31: the file write moved OFF the calling thread. Until v0.10.3 every Log call took one mutex, formatted straight into
// the FILE and fflush'ed it = one WriteFile system call per line ON THE CALLER'S THREAD -- mostly the game's render thread
// (the 600-blit statistics windows write ~20 long lines in one frame: a periodic stall of the frame that wrote them, and
// every Log call took the mutex even with debug = 0). Now:
//  - a line above the current level (phase 39; was: debug = 0), or the file cannot be opened: ONE atomic load and return --
//    no lock, no formatting, no timer read;
//  - a line that passes: formatted on the calling thread (the arguments are only valid there) into a std::string and
//    queued under a slim reader/writer lock (a push_back, no I/O); a writer thread (LogWriterThread, started with the file at
//    the first line that passes) wakes on an auto-reset event, takes the whole queue and writes it with ONE fflush per batch.
//    The file stays readable live (_SH_DENYNO, flushed within microseconds of the line).
//  - the timestamp is taken under the queue lock, so lines keep their order and their times stay monotonic;
//  - the exit line (LogExiting: DLL_PROCESS_DETACH) and the static drainer (a test exe's normal exit) DRAIN: while the writer
//    thread is alive they queue + wait for it (<= 0.5 s); once it is gone (at process exit Windows kills every other thread
//    before DLL_PROCESS_DETACH) they write the rest themselves, but only with TRY-locks: a writer killed in the middle of a
//    batch holds the file lock (and the CRT's own FILE lock) for ever = those lines are dropped, never a hang on quit (the
//    v0.7.10 rule of the old mutex).
// Win32 primitives only (SRWLOCK / event / CreateThread): no destructors at unload, safe to start under the loader lock (the
// first line may be written from DllMain; the writer thread then starts running once DllMain returns).
// phase 39: lowering the level only filters (the file, the queue and the writer thread stay); raising it writes one line
// "log level -> N (name)" (the file is created then if it was not yet). The lines that pass are timed (two TSC reads per
// line, none for a filtered call): costTicks / lines feed the tuning menu's Logging row (LogCost).
namespace LogDetail {
struct Item { SYSTEMTIME st; std::string text; };
struct State {
    std::atomic<int>      level{-1};                       // phase 39: -1 = dlaa.ini not read yet, else 0..4 (was `enabled`)
    std::atomic<int>      file{0};                         // phase 39: 0 = not opened yet, 1 = open, -1 = could not be opened
    SRWLOCK               initLock = SRWLOCK_INIT;
    SRWLOCK               qLock = SRWLOCK_INIT;            // the queue (+ queued)
    SRWLOCK               fLock = SRWLOCK_INIT;            // the FILE (a writer batch / a synchronous write)
    std::vector<Item>*    q = nullptr;                     // heap, never freed (no destructor at unload)
    HANDLE                ev = nullptr;                    // auto-reset: lines queued
    HANDLE                th = nullptr;                    // the writer thread (null = synchronous writes)
    FILE*                 f = nullptr;
    uint64_t              queued = 0;                      // lines queued so far (under qLock)
    std::atomic<uint64_t> written{0};                      // lines the writer thread has written + flushed
    std::atomic<uint64_t> costTicks{0};                    // phase 39: TSC ticks inside LogImpl of the lines that passed
    std::atomic<uint64_t> lines{0};                        // phase 39: lines that passed the level and were queued / written
};
inline State& S() { static State s; return s; }            // constant-initialised (no dynamic init / guard)

inline void WriteItems(FILE* f, std::vector<Item>& items) {
    for (const Item& it : items) {
        fprintf(f, "[%02d:%02d:%02d.%03d] ", it.st.wHour, it.st.wMinute, it.st.wSecond, it.st.wMilliseconds);
        fwrite(it.text.data(), 1, it.text.size(), f);
        fputc('\n', f);
    }
    fflush(f);
    items.clear();
}

inline DWORD WINAPI LogWriterThread(LPVOID) {
    State& s = S();
    std::vector<Item> batch;
    for (;;) {
        WaitForSingleObject(s.ev, INFINITE);
        AcquireSRWLockExclusive(&s.fLock);                // file lock first: a drain sees "written" only after the flush
        AcquireSRWLockExclusive(&s.qLock);
        batch.swap(*s.q);
        ReleaseSRWLockExclusive(&s.qLock);
        const uint64_t n = batch.size();
        if (n) WriteItems(s.f, batch);
        ReleaseSRWLockExclusive(&s.fLock);
        if (n) s.written.fetch_add(n, std::memory_order_release);
    }
}

// phase 39: the level from dlaa.ini (LogLevelFromIni), once -- at the first Log call / LogLevel / LogSetLevel. No file yet.
inline int InitLevel() {
    State& s = S();
    AcquireSRWLockExclusive(&s.initLock);
    int l = s.level.load(std::memory_order_relaxed);
    if (l < 0) {
        l = 1;
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);      // full path to the game .exe
        char* slash = strrchr(path, '\\');
        if (slash) {
            slash[1] = 0;
            l = LogLevelFromIni(path);
        }
        s.level.store(l, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&s.initLock);
    return l;
}

// The first line that passes the level: the file is created, the queue + writer thread start (until phase 38: at the first
// Log call with debug = 1). False = the file cannot be opened (every later line is dropped at once).
inline bool OpenFile() {
    State& s = S();
    const int st0 = s.file.load(std::memory_order_acquire);
    if (st0 != 0) return st0 > 0;
    AcquireSRWLockExclusive(&s.initLock);
    if (s.file.load(std::memory_order_relaxed) == 0) {
        int st = -1;
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);      // full path to the game .exe
        char* slash = strrchr(path, '\\');
        if (slash) {
            slash[1] = 0;
            strcat_s(path, "dlaa_inject.log");
            s.f = _fsopen(path, "w", _SH_DENYNO);
            if (s.f) {
                s.q = new std::vector<Item>();
                s.q->reserve(256);
                s.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (s.ev) {
                    s.th = CreateThread(nullptr, 0, LogWriterThread, nullptr, 0, nullptr);
                    if (!s.th) { CloseHandle(s.ev); s.ev = nullptr; }   // no writer thread: synchronous writes
                }
                st = 1;
            }
        }
        s.file.store(st, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&s.initLock);
    return s.file.load(std::memory_order_acquire) > 0;
}

inline bool WriterAlive() {
    const State& s = S();
    return s.th && WaitForSingleObject(s.th, 0) == WAIT_TIMEOUT;
}

// Synchronous write of the queue + one more line. tryOnly (exit / dead writer): try-locks, see the header.
inline void WriteNow(Item* extra, bool tryOnly) {
    State& s = S();
    if (tryOnly) { if (!TryAcquireSRWLockExclusive(&s.fLock)) return; }
    else AcquireSRWLockExclusive(&s.fLock);
    std::vector<Item> pend;
    bool q = true;
    if (tryOnly) q = TryAcquireSRWLockExclusive(&s.qLock) != 0;
    else AcquireSRWLockExclusive(&s.qLock);
    if (q) {
        if (s.q) pend.swap(*s.q);
        if (extra) GetLocalTime(&extra->st);
        ReleaseSRWLockExclusive(&s.qLock);
    } else if (extra) {
        GetLocalTime(&extra->st);
    }
    if (extra) pend.push_back(std::move(*extra));
    if (!pend.empty() && s.f) WriteItems(s.f, pend);
    ReleaseSRWLockExclusive(&s.fLock);
}

// Queue one line (timestamp under the lock) and wake the writer -- only when the queue was empty (SetEvent is a system call;
// a non-empty queue has not been taken yet, so the writer's coming swap takes this line too).
inline void Enqueue(Item&& it) {
    State& s = S();
    AcquireSRWLockExclusive(&s.qLock);
    GetLocalTime(&it.st);                                 // under the queue lock: queue order = time order
    const bool wake = s.q->empty();
    s.q->push_back(std::move(it));
    ++s.queued;
    ReleaseSRWLockExclusive(&s.qLock);
    if (wake) SetEvent(s.ev);
}

// Everything queued so far reaches the file (+ `extra` last): the live writer is waited for (<= 0.5 s), a dead one is replaced.
inline void Drain(Item* extra) {
    State& s = S();
    if (WriterAlive()) {
        if (extra) Enqueue(std::move(*extra));
        uint64_t target = 0;
        if (TryAcquireSRWLockExclusive(&s.qLock)) { target = s.queued; ReleaseSRWLockExclusive(&s.qLock); }
        else return;
        SetEvent(s.ev);
        for (int i = 0; i < 500 && s.written.load(std::memory_order_acquire) < target; ++i) {
            if (!WriterAlive()) { WriteNow(nullptr, true); return; }
            Sleep(1);
        }
        return;
    }
    WriteNow(extra, true);
}

// Static drainer: whatever is still queued when the module's static objects are destroyed (DLL detach after the exit line,
// a test exe's normal exit) is written.
struct Drainer { ~Drainer() { if (S().file.load(std::memory_order_acquire) == 1) Drain(nullptr); } };
inline Drainer g_drainer;                                  // odr-used by Emit (an unreferenced inline variable is never emitted)

// One line that passed the level: formatted here (phase 39: "ERROR " / "WARN  " after the timestamp for levels 1 / 2; info and
// debug lines unchanged), then queued / drained / written. False = no file (nothing written).
inline bool Emit(int lvl, const char* fmt, va_list ap) {
    State& s = S();
    if (s.file.load(std::memory_order_acquire) <= 0) {
        static Drainer* volatile s_keep = &g_drainer;     // odr-use: the drainer is emitted + destroyed
        (void)s_keep;
        if (!OpenFile()) return false;
    }
    CpuProf::Block prof(CpuProf::kBLog);                  // phase 31: the CPU profiler's "log" row
    Item it;
    {
        const char* const tag = lvl == 1 ? "ERROR " : (lvl == 2 ? "WARN  " : "");
        const size_t tl = strlen(tag);
        char buf[1024];
        memcpy(buf, tag, tl);
        va_list ap2;
        va_copy(ap2, ap);
        const int n = vsnprintf(buf + tl, sizeof(buf) - tl, fmt, ap);
        if (n < 0) { va_end(ap2); return false; }
        if ((size_t)n < sizeof(buf) - tl) it.text.assign(buf, tl + (size_t)n);
        else {                                            // a long statistics line: format again at its full size
            it.text.resize(tl + (size_t)n + 1);
            memcpy(&it.text[0], tag, tl);
            vsnprintf(&it.text[tl], (size_t)n + 1, fmt, ap2);
            it.text.resize(tl + (size_t)n);
        }
        va_end(ap2);
    }
    if (LogExiting().load(std::memory_order_relaxed)) { Drain(&it); return true; }   // the exit line
    if (!s.ev) { WriteNow(&it, false); return true; }     // no writer thread: written here (the old way)
    Enqueue(std::move(it));
    return true;
}
// Emit + the phase 39 cost accounting (two TSC reads per line that passed; atomics: Log runs on several threads).
inline void Timed(int lvl, const char* fmt, va_list ap) {
    State& s = S();
    const uint64_t t0 = __rdtsc();
    const bool ok = Emit(lvl, fmt, ap);
    const uint64_t t1 = __rdtsc();
    if (!ok) return;
    s.costTicks.fetch_add(t1 - t0, std::memory_order_relaxed);
    s.lines.fetch_add(1, std::memory_order_relaxed);
}
inline void TimedF(int lvl, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    Timed(lvl, fmt, ap);
    va_end(ap);
}
} // namespace LogDetail

// lvl: 1 error, 2 warning, 3 info, 4 debug (the macros below). A line above the current level returns after ONE atomic load.
inline void LogImpl(int lvl, const char* fmt, ...) {
    LogDetail::State& s = LogDetail::S();
    int cur = s.level.load(std::memory_order_acquire);
    if (lvl > cur) {
        if (cur >= 0) return;                             // filtered: nothing at all (no lock, no formatting, no timer)
        cur = LogDetail::InitLevel();                     // the first Log call: dlaa.ini decides
        if (lvl > cur) return;
    }
    va_list ap; va_start(ap, fmt);
    LogDetail::Timed(lvl, fmt, ap);
    va_end(ap);
}
#define LogErr(...)  LogImpl(1, __VA_ARGS__)               // phase 39: failures and errors ("ERROR " in the file)
#define LogWarn(...) LogImpl(2, __VA_ARGS__)               // phase 39: degraded but running ("WARN  " in the file)
#define Log(...)     LogImpl(3, __VA_ARGS__)               // info: one-time state lines (startup, a change, the exit line)
#define LogDbg(...)  LogImpl(4, __VA_ARGS__)               // phase 39: periodic statistics, verbose tracing, dumps
// phase 39: the current level (0..4; reads dlaa.ini first when nothing has logged yet).
inline int LogLevel() {
    const int l = LogDetail::S().level.load(std::memory_order_acquire);
    return l >= 0 ? l : LogDetail::InitLevel();
}
// phase 39: the level live (the tuning menu's Logging row). Raising it writes "log level -> N (name)" (the file is created
// then if needed); lowering it only filters from the next line on.
inline void LogSetLevel(int n) {
    n = n < 0 ? 0 : (n > 4 ? 4 : n);
    LogDetail::State& s = LogDetail::S();
    if (s.level.load(std::memory_order_acquire) < 0) LogDetail::InitLevel();   // raised = compared with the dlaa.ini level
    const int old = s.level.exchange(n, std::memory_order_acq_rel);
    if (n > old && n >= 1) LogDetail::TimedF(3, "log level -> %d (%s)", n, LogLevelName(n));
}
// phase 31: the log file is being written. phase 39: level >= 1 and the file is open (created at the first line that passed).
inline bool LogOn() {
    const LogDetail::State& s = LogDetail::S();
    return s.level.load(std::memory_order_relaxed) >= 1 && s.file.load(std::memory_order_relaxed) == 1;
}
// phase 39: cumulative cost of the lines that passed (TSC ticks inside LogImpl: format + queue) and their count.
struct LogCostSnap { uint64_t ticks, lines; };
inline LogCostSnap LogCost() {
    const LogDetail::State& s = LogDetail::S();
    return { s.costTicks.load(std::memory_order_relaxed), s.lines.load(std::memory_order_relaxed) };
}
