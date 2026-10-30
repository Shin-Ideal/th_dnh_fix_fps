#pragma once
#include <windows.h>

struct PatchConfig {
    double targetFps = 60.0;
    bool enableFpsLimit = true;
    bool forceImmediate = true;
    bool enableLogging = true;

    void Load(const wchar_t* iniPath) {
        enableLogging = GetPrivateProfileIntW(L"Settings", L"EnableLogging", 1, iniPath) != 0;
        enableFpsLimit = GetPrivateProfileIntW(L"Settings", L"EnableFpsLimit", 1, iniPath) != 0;
        forceImmediate = GetPrivateProfileIntW(L"Settings", L"ForceImmediate", 1, iniPath) != 0;

        wchar_t fpsStr[32];
        if (GetPrivateProfileStringW(L"Settings", L"TargetFPS", L"60.0", fpsStr, 32, iniPath) > 0) {
            double val = _wtof(fpsStr);
            if (val >= 10.0 && val <= 360.0) {
                targetFps = val;
            }
        }
    }
};

extern PatchConfig g_config;
