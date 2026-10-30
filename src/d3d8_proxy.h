#pragma once
#include <windows.h>
#include "d3d8_types.h"

// Direct3D 8 exported function
extern "C" IDirect3D8* WINAPI Direct3DCreate8(UINT SDKVersion);

// Proxy lifecycle
bool InitD3D8Proxy();
void ShutdownD3D8Proxy();
