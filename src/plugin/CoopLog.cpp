// CoopLog implementation. See CoopLog.h for rationale.
//
// VS2010 (v100) compatible: Win32 CRITICAL_SECTION + GetLocalTime, plain stdio.

#define _CRT_SECURE_NO_WARNINGS 1

#include "CoopLog.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace coop {
namespace {

FILE*            g_fp   = 0;
CRITICAL_SECTION g_cs;
bool             g_init = false;
char             g_tag[16] = { 0 };
volatile long    g_fakeSkewMs = 0;

void writeLine(const char* level, const char* msg) {
    if (!g_init) return;
    EnterCriticalSection(&g_cs);
    if (g_fp) {
        // Derive the stamp from wallClockMs() (real clock + injected skew) so
        // log timestamps and the wire time-sync share one clock.
        unsigned long ms = wallClockMs();
        unsigned long hh = (ms / 3600000ul) % 24ul;
        unsigned long mm = (ms / 60000ul) % 60ul;
        unsigned long ss = (ms / 1000ul) % 60ul;
        unsigned long mmm = ms % 1000ul;
        std::fprintf(g_fp, "[%02lu:%02lu:%02lu.%03lu] [%s] %s: %s\n",
                     hh, mm, ss, mmm,
                     g_tag, level, msg ? msg : "");
        std::fflush(g_fp);
    }
    LeaveCriticalSection(&g_cs);
}

const int LOG_KEEP = 6; // ~40-90 MB each after block5d's quieter telemetry

// Rename an existing log to "<path>.<stamp>.log" and prune the oldest archives.
void rotateOld(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return; // no old log
    if (fa.nFileSizeHigh == 0 && fa.nFileSizeLow == 0) return;
    FILETIME lt; SYSTEMTIME st;
    FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt);
    FileTimeToSystemTime(&lt, &st);
    char dst[MAX_PATH + 32];
    _snprintf(dst, sizeof(dst) - 1, "%s.%04u%02u%02u_%02u%02u%02u.log", path,
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    dst[sizeof(dst) - 1] = '\0';
    MoveFileExA(path, dst, MOVEFILE_REPLACE_EXISTING);
    // Prune: archives sort by name == by time; delete all but the newest LOG_KEEP.
    char pat[MAX_PATH + 16];
    _snprintf(pat, sizeof(pat) - 1, "%s.*.log", path); pat[sizeof(pat) - 1] = '\0';
    const char* slash = strrchr(path, '\\');
    const char* slash2 = strrchr(path, '/');
    if (slash2 > slash) slash = slash2;
    size_t dirLen = slash ? (size_t)(slash - path + 1) : 0;
    char names[64][MAX_PATH];
    int n = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (n < 64) { strncpy(names[n], fd.cFileName, MAX_PATH - 1); names[n][MAX_PATH - 1] = '\0'; ++n; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    // simple insertion sort, ascending
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; --j) {
            char t[MAX_PATH]; strcpy(t, names[j]); strcpy(names[j], names[j - 1]); strcpy(names[j - 1], t);
        }
    for (int i = 0; i < n - LOG_KEEP; ++i) {
        char full[MAX_PATH * 2];
        _snprintf(full, sizeof(full) - 1, "%.*s%s", (int)dirLen, path, names[i]);
        full[sizeof(full) - 1] = '\0';
        DeleteFileA(full);
    }
}

} // namespace

unsigned long wallClockMs() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    long ms = (long)((((unsigned long)st.wHour * 60ul + st.wMinute) * 60ul + st.wSecond) * 1000ul
                     + st.wMilliseconds);
    ms += g_fakeSkewMs;
    // Wrap into [0, 24h) so a skew across midnight still formats sanely.
    const long DAY = 24l * 3600l * 1000l;
    ms %= DAY;
    if (ms < 0) ms += DAY;
    return (unsigned long)ms;
}

void logSetFakeSkewMs(long skewMs) { g_fakeSkewMs = skewMs; }

void logInit(const char* path, const char* modeTag) {
    if (g_init) return;
    InitializeCriticalSection(&g_cs);
    g_init = true;

    if (modeTag) {
        size_t i = 0;
        for (; modeTag[i] && i < sizeof(g_tag) - 1; ++i) g_tag[i] = modeTag[i];
        g_tag[i] = '\0';
    }

    // FORK 2026-11-07 (review D4): the previous run's log is no longer lost.
    // Before truncating, the old file is renamed to "<path>.<yyyymmdd_hhmmss>.log"
    // (its last-write time); only the newest LOG_KEEP archives stay. The current
    // file keeps its fixed name - the lab scripts and the kit's 'after' step read it.
    // KENSHICOOP_LOG_ROTATE=0 restores the old "fresh file, no archive" behaviour.
    if (path && path[0]) {
        const char* rot = getenv("KENSHICOOP_LOG_ROTATE");
        if (!(rot && rot[0] == '0')) rotateOld(path);
        g_fp = std::fopen(path, "w"); // fresh file each run
    }
    writeLine("INFO", "log opened");
}

void logSetTag(const char* modeTag) {
    if (!g_init || !modeTag) return;
    EnterCriticalSection(&g_cs);
    size_t i = 0;
    for (; modeTag[i] && i < sizeof(g_tag) - 1; ++i) g_tag[i] = modeTag[i];
    g_tag[i] = '\0';
    LeaveCriticalSection(&g_cs);
}

void logLine(const char* msg)    { writeLine("INFO",  msg); }
void logErrLine(const char* msg) { writeLine("ERROR", msg); }

void logClose() {
    if (!g_init) return;
    EnterCriticalSection(&g_cs);
    if (g_fp) {
        std::fflush(g_fp);
        std::fclose(g_fp);
        g_fp = 0;
    }
    LeaveCriticalSection(&g_cs);
}

} // namespace coop
