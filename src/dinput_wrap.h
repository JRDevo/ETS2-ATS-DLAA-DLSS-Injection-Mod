// KeySwallow (v0.10.0) -- while the tuning menu is open, the game must not see the menu's keys (ETS2's default keyboard
// layout steers / accelerates / brakes with the arrow keys). Three paths, each active only while the menu is open and
// only for the 5 bound menu keys (MenuSwallowKeys, inject.cpp):
//   1. DirectInput: proxy.cpp's DirectInput8Create goes through CreateDirectInput8, which wraps the returned
//      IDirectInput8A/W; CreateDevice of a KEYBOARD returns a wrapped IDirectInputDevice8A/W (every method forwarded)
//      whose GetDeviceState clears the DIK bytes of those keys and whose GetDeviceData drops their key-DOWN entries.
//   2. Window messages: HookWindow subclasses the game window once (the swapchain's OutputWindow, first top-level
//      Present); WM_KEYDOWN / WM_SYSKEYDOWN / WM_CHAR / WM_SYSCHAR and WM_INPUT keyboard make events of those keys are
//      dropped. Everything else goes to the original window procedure.
//   3. The menu itself reads the keys with GetAsyncKeyState (PollKeys), which none of this touches.
// Key RELEASES are never dropped (WM_KEYUP / WM_SYSKEYUP, WM_INPUT break events, buffered DirectInput up entries): a key
// that was already held when the menu opened (the accelerator) or the Delete that opened it must reach the game as
// released, or the game would keep it held. A release of a key the game saw as up is harmless.
#pragma once
#include <windows.h>
#include <unknwn.h>

namespace KeySwallow {
using PFN_DI8Create = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
// Calls `real` and wraps the IDirectInput8A / IDirectInput8W it returns (other IIDs / failures: untouched).
HRESULT CreateDirectInput8(PFN_DI8Create real, HINSTANCE h, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer);
// Subclasses `hwnd` once per process (later calls do nothing). Any thread of this process.
void HookWindow(HWND hwnd);
// DLL unload: puts the original window procedure back if the window still exists and still points at ours.
void UnhookWindow();
}

// inject.cpp: the virtual-key codes to swallow right now (the bound menu keys while the menu is open), count returned;
// 0 = nothing (menu closed / not compiled in). Any thread (input / window threads).
int MenuSwallowKeys(int* vks, int cap);
