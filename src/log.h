#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
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

// v0.7.10: set by the DLL_PROCESS_DETACH exit line. At process exit the other threads are already gone; one of them
// may have died inside Log with the mutex held, so the exit line only try-locks (a blocked lock = the game hangs on quit).
inline std::atomic<bool>& LogExiting() { static std::atomic<bool> b{false}; return b; }

// phase 31: the file write moved OFF the calling thread. Until v0.10.3 every Log call took one mutex, formatted straight into
// the FILE and fflush'ed it = one WriteFile system call per line ON THE CALLER'S THREAD -- mostly the game's render thread
// (the 600-blit statistics windows write ~20 long lines in one frame: a periodic stall of the frame that wrote them, and
// every Log call took the mutex even with debug = 0). Now:
//  - debug = 0 (or the file cannot be opened): ONE atomic load and return -- no lock, no formatting;
//  - debug = 1: the line is formatted on the calling thread (the arguments are only valid there) into a std::string and
//    queued under a slim reader/writer lock (a push_back, no I/O); a writer thread (LogWriterThread, started at the first
//    line) wakes on an auto-reset event, takes the whole queue and writes it with ONE fflush per batch. The file stays
//    readable live (_SH_DENYNO, flushed within microseconds of the line).
//  - the timestamp is taken under the queue lock, so lines keep their order and their times stay monotonic;
//  - the exit line (LogExiting: DLL_PROCESS_DETACH) and the static drainer (a test exe's normal exit) DRAIN: while the writer
//    thread is alive they queue + wait for it (<= 0.5 s); once it is gone (at process exit Windows kills every other thread
//    before DLL_PROCESS_DETACH) they write the rest themselves, but only with TRY-locks: a writer killed in the middle of a
//    batch holds the file lock (and the CRT's own FILE lock) for ever = those lines are dropped, never a hang on quit (the
//    v0.7.10 rule of the old mutex).
// Win32 primitives only (SRWLOCK / event / CreateThread): no destructors at unload, safe to start under the loader lock (the
// first line is written from DllMain; the writer thread then starts running once DllMain returns).
namespace LogDetail {
struct Item { SYSTEMTIME st; std::string text; };
struct State {
    std::atomic<int>      enabled{-1};                     // -1 = not checked yet, 0 = off (no file), 1 = on
    SRWLOCK               initLock = SRWLOCK_INIT;
    SRWLOCK               qLock = SRWLOCK_INIT;            // the queue (+ queued)
    SRWLOCK               fLock = SRWLOCK_INIT;            // the FILE (a writer batch / a synchronous write)
    std::vector<Item>*    q = nullptr;                     // heap, never freed (no destructor at unload)
    HANDLE                ev = nullptr;                    // auto-reset: lines queued
    HANDLE                th = nullptr;                    // the writer thread (null = synchronous writes)
    FILE*                 f = nullptr;
    uint64_t              queued = 0;                      // lines queued so far (under qLock)
    std::atomic<uint64_t> written{0};                      // lines the writer thread has written + flushed
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

// First call: dlaa.ini debug decides (LogDebugEnabled), the file is created, the queue + writer thread start.
inline void Init() {
    State& s = S();
    AcquireSRWLockExclusive(&s.initLock);
    if (s.enabled.load(std::memory_order_relaxed) < 0) {
        int en = 0;
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);      // full path to the game .exe
        char* slash = strrchr(path, '\\');
        if (slash) {
            slash[1] = 0;
            if (LogDebugEnabled(path)) {
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
                    en = 1;
                }
            }
        }
        s.enabled.store(en, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&s.initLock);
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
struct Drainer { ~Drainer() { if (S().enabled.load(std::memory_order_acquire) == 1) Drain(nullptr); } };
inline Drainer g_drainer;                                  // odr-used by Init (an unreferenced inline variable is never emitted)
} // namespace LogDetail

inline void LogImpl(const char* fmt, ...) {
    LogDetail::State& s = LogDetail::S();
    int en = s.enabled.load(std::memory_order_acquire);
    if (en < 0) {
        LogDetail::Init();
        en = s.enabled.load(std::memory_order_acquire);
        static LogDetail::Drainer* volatile s_keep = &LogDetail::g_drainer;   // odr-use: the drainer is emitted + destroyed
        (void)s_keep;
    }
    if (en == 0) return;                                  // debug = 0: nothing at all (no lock, no formatting)
    CpuProf::Block prof(CpuProf::kBLog);                  // phase 31: the CPU profiler's "log" row
    LogDetail::Item it;
    {
        char buf[1024];
        va_list ap; va_start(ap, fmt);
        const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if (n < (int)sizeof(buf)) it.text.assign(buf, (size_t)n);
        else {                                            // a long statistics line: format again at its full size
            it.text.resize((size_t)n + 1);
            va_list ap2; va_start(ap2, fmt);
            vsnprintf(&it.text[0], (size_t)n + 1, fmt, ap2);
            va_end(ap2);
            it.text.resize((size_t)n);
        }
    }
    if (LogExiting().load(std::memory_order_relaxed)) { LogDetail::Drain(&it); return; }   // the exit line
    if (!s.ev) { LogDetail::WriteNow(&it, false); return; }   // no writer thread: written here (the old way)
    LogDetail::Enqueue(std::move(it));
}
#define Log(...) LogImpl(__VA_ARGS__)
// phase 31: the log file is being written (dlaa.ini debug = 1 and the file opened). False before the first Log call.
inline bool LogOn() { return LogDetail::S().enabled.load(std::memory_order_relaxed) == 1; }
