#include "d3d8_proxy.h"
#include "fps_limiter.h"
#include "logger.h"
#include "config.h"

PatchConfig g_config;

static HMODULE g_realD3D8Module = nullptr;

typedef IDirect3D8* (WINAPI *Direct3DCreate8_t)(UINT SDKVersion);
static Direct3DCreate8_t g_realDirect3DCreate8 = nullptr;

// Forwarded exports
typedef BOOL (WINAPI *ValidatePixelShader_t)(const DWORD* tokens, const void* extOps, BOOL returnErrors, char** errorString);
typedef BOOL (WINAPI *ValidateVertexShader_t)(const DWORD* tokens, const void* extOps, BOOL returnErrors, char** errorString);
typedef void (WINAPI *DebugSetMute_t)();
typedef int (WINAPI *Direct3D8EnableMaximizedWindowedModeShim_t)(int enable);

static ValidatePixelShader_t g_realValidatePixelShader = nullptr;
static ValidateVertexShader_t g_realValidateVertexShader = nullptr;
static DebugSetMute_t g_realDebugSetMute = nullptr;
static Direct3D8EnableMaximizedWindowedModeShim_t g_realDirect3D8EnableMaximizedWindowedModeShim = nullptr;

// Sleep hook: Bypass the game's internal Sleep(1) loop so it doesn't cause 18ms lag
typedef void (WINAPI *Sleep_t)(DWORD dwMilliseconds);
static Sleep_t g_origSleep = nullptr;

static FpsLimiter g_fpsLimiter;

static void WINAPI Hooked_Sleep(DWORD dwMilliseconds) {
    // When the game calls Sleep(1) in its wait loop, do NOT sleep!
    // Simply yield with Sleep(0) so Windows doesn't oversleep by 2ms each time.
    // The true 60FPS precision timing is handled entirely in Hooked_Present!
    if (g_origSleep) {
        g_origSleep(0);
    }
}

// Original D3D8 method function pointer types
typedef HRESULT (__stdcall *CreateDevice_t)(
    void* This,
    UINT Adapter,
    D3DDEVTYPE DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice8** ppReturnedDeviceInterface
);

typedef HRESULT (__stdcall *Present_t)(
    void* This,
    const RECT* pSourceRect,
    const RECT* pDestRect,
    HWND hDestWindowOverride,
    const void* pDirtyRegion
);

typedef HRESULT (__stdcall *Reset_t)(
    void* This,
    D3DPRESENT_PARAMETERS* pPresentationParameters
);

static CreateDevice_t g_origCreateDevice = nullptr;
static Present_t g_origPresent = nullptr;
static Reset_t g_origReset = nullptr;

static void* g_hookedDevice = nullptr;
static void* g_hookedD3D8 = nullptr;

static HRESULT __stdcall Hooked_CreateDevice(
    void* This,
    UINT Adapter,
    D3DDEVTYPE DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice8** ppReturnedDeviceInterface
);

static HRESULT __stdcall Hooked_Present(
    void* This,
    const RECT* pSourceRect,
    const RECT* pDestRect,
    HWND hDestWindowOverride,
    const void* pDirtyRegion
);

static HRESULT __stdcall Hooked_Reset(
    void* This,
    D3DPRESENT_PARAMETERS* pPresentationParameters
);

static void HookD3D8Object(void* pD3D8);
static void HookDeviceObject(void* pDevice);
static void HookSleepInIAT();

bool InitD3D8Proxy() {
    if (g_realD3D8Module) return true;

    wchar_t iniPath[MAX_PATH];
    wcscpy_s(iniPath, g_logFilePath);
    wchar_t* lastSlash = wcsrchr(iniPath, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
        wcscat_s(iniPath, L"d3d8_patch.ini");
    } else {
        wcscpy_s(iniPath, L"d3d8_patch.ini");
    }
    g_config.Load(iniPath);

    LogMessage("==============================================");
    LogMessage("  Touhou Danmakufu 0.12m FPS Patch (d3d8.dll)");
    LogMessage("==============================================");
    LogMessage("[Config] TargetFPS: %.2f", g_config.targetFps);
    LogMessage("[Config] EnableFpsLimit: %d", g_config.enableFpsLimit);
    LogMessage("[Config] ForceImmediate: %d", g_config.forceImmediate);

    timeBeginPeriod(1);
    g_fpsLimiter.Init(g_config.targetFps);

    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    wcscat_s(sysDir, L"\\d3d8.dll");

    g_realD3D8Module = LoadLibraryW(sysDir);
    if (!g_realD3D8Module) {
        LogMessage("[Error] Failed to load original d3d8.dll from: %ls", sysDir);
        return false;
    }

    g_realDirect3DCreate8 = (Direct3DCreate8_t)GetProcAddress(g_realD3D8Module, "Direct3DCreate8");
    g_realValidatePixelShader = (ValidatePixelShader_t)GetProcAddress(g_realD3D8Module, "ValidatePixelShader");
    g_realValidateVertexShader = (ValidateVertexShader_t)GetProcAddress(g_realD3D8Module, "ValidateVertexShader");
    g_realDebugSetMute = (DebugSetMute_t)GetProcAddress(g_realD3D8Module, "DebugSetMute");
    g_realDirect3D8EnableMaximizedWindowedModeShim = (Direct3D8EnableMaximizedWindowedModeShim_t)GetProcAddress(g_realD3D8Module, "Direct3D8EnableMaximizedWindowedModeShim");

    if (!g_realDirect3DCreate8) {
        LogMessage("[Error] Failed to get Direct3DCreate8 address.");
        return false;
    }

    LogMessage("[Success] Original d3d8.dll loaded successfully.");
    return true;
}

void ShutdownD3D8Proxy() {
    g_fpsLimiter.Shutdown();
    timeEndPeriod(1);

    if (g_realD3D8Module) {
        FreeLibrary(g_realD3D8Module);
        g_realD3D8Module = nullptr;
    }
    LogMessage("[Patch] Shutdown completed.");
}

static void HookSleepInIAT() {
    static bool hooked = false;
    if (hooked) return;
    hooked = true;

    // Address 0x00522148 is the IAT slot for KERNEL32!Sleep in Touhou Danmakufu 0.12m
    void** pIatEntry = (void**)0x00522148;

    DWORD oldProtect;
    if (VirtualProtect(pIatEntry, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        g_origSleep = (Sleep_t)*pIatEntry;
        *pIatEntry = (void*)Hooked_Sleep;
        VirtualProtect(pIatEntry, sizeof(void*), oldProtect, &oldProtect);
        LogMessage("[Success] Hooked KERNEL32!Sleep at IAT [0x00522148] -> Redirected to Sleep(0)");
    } else {
        LogMessage("[Error] Failed to VirtualProtect IAT entry for Sleep.");
    }
}

extern "C" IDirect3D8* WINAPI Direct3DCreate8(UINT SDKVersion) {
    LogMessage("[Patch] Direct3DCreate8 called (SDKVersion: %u)", SDKVersion);

    if (!g_realDirect3DCreate8) {
        if (!InitD3D8Proxy()) {
            return nullptr;
        }
    }

    // Hook game's Sleep now that UPX has finished unpacking
    HookSleepInIAT();

    IDirect3D8* pD3D8 = g_realDirect3DCreate8(SDKVersion);
    if (!pD3D8) {
        LogMessage("[Error] Real Direct3DCreate8 returned null.");
        return nullptr;
    }

    HookD3D8Object(pD3D8);
    return pD3D8;
}

extern "C" BOOL WINAPI ValidatePixelShader(const DWORD* tokens, const void* extOps, BOOL returnErrors, char** errorString) {
    if (!g_realValidatePixelShader) InitD3D8Proxy();
    if (g_realValidatePixelShader) return g_realValidatePixelShader(tokens, extOps, returnErrors, errorString);
    return FALSE;
}

extern "C" BOOL WINAPI ValidateVertexShader(const DWORD* tokens, const void* extOps, BOOL returnErrors, char** errorString) {
    if (!g_realValidateVertexShader) InitD3D8Proxy();
    if (g_realValidateVertexShader) return g_realValidateVertexShader(tokens, extOps, returnErrors, errorString);
    return FALSE;
}

extern "C" void WINAPI DebugSetMute() {
    if (!g_realDebugSetMute) InitD3D8Proxy();
    if (g_realDebugSetMute) g_realDebugSetMute();
}

extern "C" int WINAPI Direct3D8EnableMaximizedWindowedModeShim(int enable) {
    if (!g_realDirect3D8EnableMaximizedWindowedModeShim) InitD3D8Proxy();
    if (g_realDirect3D8EnableMaximizedWindowedModeShim) return g_realDirect3D8EnableMaximizedWindowedModeShim(enable);
    return 0;
}

static void HookD3D8Object(void* pD3D8) {
    if (!pD3D8 || g_hookedD3D8 == pD3D8) return;

    void** vtable = *(void***)pD3D8;
    if (!vtable) return;

    if (vtable[15] != (void*)Hooked_CreateDevice) {
        DWORD oldProtect;
        if (VirtualProtect(&vtable[15], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            g_origCreateDevice = (CreateDevice_t)vtable[15];
            vtable[15] = (void*)Hooked_CreateDevice;
            VirtualProtect(&vtable[15], sizeof(void*), oldProtect, &oldProtect);
            LogMessage("[Success] Hooked IDirect3D8::CreateDevice (vtable[15])");
        } else {
            LogMessage("[Error] VirtualProtect failed for CreateDevice hook.");
        }
    }
    g_hookedD3D8 = pD3D8;
}

static void HookDeviceObject(void* pDevice) {
    if (!pDevice || g_hookedDevice == pDevice) return;

    void** vtable = *(void***)pDevice;
    if (!vtable) return;

    DWORD oldProtect;

    if (vtable[15] != (void*)Hooked_Present) {
        if (VirtualProtect(&vtable[15], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            g_origPresent = (Present_t)vtable[15];
            vtable[15] = (void*)Hooked_Present;
            VirtualProtect(&vtable[15], sizeof(void*), oldProtect, &oldProtect);
            LogMessage("[Success] Hooked IDirect3DDevice8::Present (vtable[15])");
        } else {
            LogMessage("[Error] VirtualProtect failed for Present hook.");
        }
    }

    if (vtable[14] != (void*)Hooked_Reset) {
        if (VirtualProtect(&vtable[14], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            g_origReset = (Reset_t)vtable[14];
            vtable[14] = (void*)Hooked_Reset;
            VirtualProtect(&vtable[14], sizeof(void*), oldProtect, &oldProtect);
            LogMessage("[Success] Hooked IDirect3DDevice8::Reset (vtable[14])");
        } else {
            LogMessage("[Error] VirtualProtect failed for Reset hook.");
        }
    }

    g_hookedDevice = pDevice;
    LogMessage("[Patch] Device object hooked successfully.");
}

static void AdjustPresentationParameters(D3DPRESENT_PARAMETERS* p) {
    if (!p) return;

    LogMessage("[Params] Before: Windowed=%d, %ux%u, SwapEffect=%d, Interval=0x%08X, BackBufferCount=%u",
        p->Windowed, p->BackBufferWidth, p->BackBufferHeight, p->SwapEffect,
        p->FullScreen_PresentationInterval, p->BackBufferCount);

    if (p->Windowed) {
        p->SwapEffect = D3DSWAPEFFECT_DISCARD;
        p->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
        if (p->BackBufferCount == 0) {
            p->BackBufferCount = 1;
        }
    } else {
        p->SwapEffect = D3DSWAPEFFECT_DISCARD;
        if (g_config.forceImmediate) {
            p->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        }
    }

    LogMessage("[Params] After: Windowed=%d, SwapEffect=%d, Interval=0x%08X, BackBufferCount=%u",
        p->Windowed, p->SwapEffect, p->FullScreen_PresentationInterval, p->BackBufferCount);
}

static HRESULT __stdcall Hooked_CreateDevice(
    void* This,
    UINT Adapter,
    D3DDEVTYPE DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice8** ppReturnedDeviceInterface)
{
    LogMessage("[Patch] Hooked_CreateDevice invoked.");
    AdjustPresentationParameters(pPresentationParameters);

    HRESULT hr = g_origCreateDevice(This, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        LogMessage("[Success] Real CreateDevice succeeded! Hooking Device object...");
        HookDeviceObject(*ppReturnedDeviceInterface);
    } else {
        LogMessage("[Error] Real CreateDevice returned hr = 0x%08X", hr);
    }

    return hr;
}

static HRESULT __stdcall Hooked_Reset(
    void* This,
    D3DPRESENT_PARAMETERS* pPresentationParameters)
{
    LogMessage("[Patch] Hooked_Reset invoked.");
    AdjustPresentationParameters(pPresentationParameters);
    return g_origReset(This, pPresentationParameters);
}

static HRESULT __stdcall Hooked_Present(
    void* This,
    const RECT* pSourceRect,
    const RECT* pDestRect,
    HWND hDestWindowOverride,
    const void* pDirtyRegion)
{
    if (g_config.enableFpsLimit) {
        g_fpsLimiter.WaitNextFrame();
    }

    static int frameCounter = 0;
    if (++frameCounter % 300 == 0) {
        LogMessage("[Status] Current FPS: %.2f", g_fpsLimiter.GetCurrentFps());
    }

    return g_origPresent(This, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}
