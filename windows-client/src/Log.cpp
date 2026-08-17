#include "Log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace awc {

namespace {
HANDLE g_file = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_lock;
bool g_ready = false;
} // namespace

void logInit(const wchar_t* folder, const wchar_t* name) {
    if (g_ready) return;
    InitializeCriticalSection(&g_lock);
    g_ready = true;

    CreateDirectoryW(folder, nullptr);
    const std::wstring path = std::wstring(folder) + L"\\" + name;
    g_file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    logf("---- started ----");
}

void logf(const char* format, ...) {
    if (!g_ready || g_file == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME t{};
    GetLocalTime(&t);
    char line[1024];
    int n = sprintf_s(line, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    va_list args;
    va_start(args, format);
    n += vsnprintf(line + n, sizeof(line) - n - 2, format, args);
    va_end(args);
    if (n < 0 || n > int(sizeof(line)) - 2) n = int(sizeof(line)) - 2;
    line[n++] = '\n';

    EnterCriticalSection(&g_lock);
    DWORD written = 0;
    WriteFile(g_file, line, DWORD(n), &written, nullptr);
    LeaveCriticalSection(&g_lock);
}

void logClose() {
    if (g_file != INVALID_HANDLE_VALUE) { CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; }
}

} // namespace awc
