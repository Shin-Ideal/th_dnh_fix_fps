#pragma once
#include <windows.h>
#include <stdio.h>
#include "config.h"

inline wchar_t g_logFilePath[MAX_PATH] = L"d3d8_patch.log";

inline void InitLoggerPath(HMODULE hModule) {
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(hModule, path, MAX_PATH)) {
        wchar_t* lastSlash = wcsrchr(path, L'\\');
        if (lastSlash) {
            *(lastSlash + 1) = L'\0';
            wcscpy_s(g_logFilePath, path);
            wcscat_s(g_logFilePath, L"d3d8_patch.log");
        }
    }
}

inline void LogMessage(const char* format, ...) {
    if (!g_config.enableLogging) return;

    static bool firstRun = true;
    FILE* fp = nullptr;
    if (firstRun) {
        _wfopen_s(&fp, g_logFilePath, L"w");
        firstRun = false;
    } else {
        _wfopen_s(&fp, g_logFilePath, L"a");
    }
    if (!fp) return;

    va_list args;
    va_start(args, format);
    vfprintf(fp, format, args);
    va_end(args);

    fprintf(fp, "\n");
    fclose(fp);
}
