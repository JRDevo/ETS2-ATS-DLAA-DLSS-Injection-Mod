// dinput8.dll proxy: forwards the 5 real exports to the system dinput8.dll and
// bootstraps the DXGI hook. This is the injection vector only -- the actual
// frame interception lives in inject.cpp.
#include <windows.h>
#include <unknwn.h>
#include "log.h"

void StartInjection();                         // inject.cpp
void OnProcessDetach(bool processTerminating); // inject.cpp (v0.7.10 exit log line)

namespace {
HMODULE g_real = nullptr;

using PFN_DI8Create        = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using PFN_DllGetClassObject = HRESULT (WINAPI*)(REFCLSID, REFIID, LPVOID*);
using PFN_Void             = HRESULT (WINAPI*)();

PFN_DI8Create         p_DirectInput8Create  = nullptr;
PFN_DllGetClassObject p_DllGetClassObject   = nullptr;
PFN_Void              p_DllCanUnloadNow      = nullptr;
PFN_Void              p_DllRegisterServer    = nullptr;
PFN_Void              p_DllUnregisterServer  = nullptr;

void LoadReal() {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);         // C:\Windows\System32
    wcscpy_s(path + n, MAX_PATH - n, L"\\dinput8.dll");
    g_real = LoadLibraryW(path);
    if (!g_real) { Log("FATAL: LoadLibrary(real dinput8) failed: %lu", GetLastError()); return; }
    p_DirectInput8Create  = (PFN_DI8Create)        GetProcAddress(g_real, "DirectInput8Create");
    p_DllGetClassObject   = (PFN_DllGetClassObject) GetProcAddress(g_real, "DllGetClassObject");
    p_DllCanUnloadNow     = (PFN_Void)              GetProcAddress(g_real, "DllCanUnloadNow");
    p_DllRegisterServer   = (PFN_Void)              GetProcAddress(g_real, "DllRegisterServer");
    p_DllUnregisterServer = (PFN_Void)              GetProcAddress(g_real, "DllUnregisterServer");
    Log("real dinput8 loaded @ %p (DirectInput8Create=%p)", (void*)g_real, (void*)p_DirectInput8Create);
}
} // namespace

extern "C" {

HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID* o, LPUNKNOWN u) {
    return p_DirectInput8Create ? p_DirectInput8Create(h, v, r, o, u) : E_FAIL;
}
HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID r, LPVOID* o) {
    return p_DllGetClassObject ? p_DllGetClassObject(c, r, o) : CLASS_E_CLASSNOTAVAILABLE;
}
HRESULT WINAPI DllCanUnloadNow()      { return p_DllCanUnloadNow     ? p_DllCanUnloadNow()     : S_FALSE; }
HRESULT WINAPI DllRegisterServer()    { return p_DllRegisterServer   ? p_DllRegisterServer()   : E_FAIL; }
HRESULT WINAPI DllUnregisterServer()  { return p_DllUnregisterServer ? p_DllUnregisterServer() : E_FAIL; }

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        LoadReal();
        StartInjection();     // spawns a worker thread; never do D3D work under loader lock
    } else if (reason == DLL_PROCESS_DETACH) {
        // v0.7.10: final log line only. lpReserved != null = the process is terminating; either way we just log
        // (plain reads + Log, loader-lock safe) and do nothing else.
        OnProcessDetach(lpReserved != nullptr);
    }
    return TRUE;
}
