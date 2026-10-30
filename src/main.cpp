#include <windows.h>
#include "d3d8_proxy.h"
#include "logger.h"

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        InitLoggerPath(hinstDLL);
        InitD3D8Proxy();
        break;
    case DLL_PROCESS_DETACH:
        ShutdownD3D8Proxy();
        break;
    }
    return TRUE;
}
