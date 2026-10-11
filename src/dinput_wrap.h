// KeySwallow (v0.10.0) -- while the tuning menu is open, the game must not see the keyboard (ETS2's default keyboard
// layout steers / accelerates / brakes with the arrow keys, and v0.10.2's hold-to-bind holds a key for 2 s: that must not
// honk the horn). v0.10.2: ONE rule -- while the menu is open (MenuHidesInput, inject.cpp) NO key and NO controller button
// reaches the game (v0.10.0 / v0.10.1 hid only the 5 bound menu keys / their controller parts). Escape / pause and every
// game key are off until the menu is closed. Three keyboard paths, each active only while the menu is open:
//   (v0.10.1: + a 4th, the CONTROLLER TAP, see below)
//   1. DirectInput: proxy.cpp's DirectInput8Create goes through CreateDirectInput8, which wraps the returned
//      IDirectInput8A/W; CreateDevice of a KEYBOARD returns a wrapped IDirectInputDevice8A/W (every method forwarded)
//      whose GetDeviceState clears all 256 DIK bytes and whose GetDeviceData drops every key-DOWN entry.
//   2. Window messages: HookWindow subclasses the game window once (the swapchain's OutputWindow, first top-level
//      Present); every WM_KEYDOWN / WM_SYSKEYDOWN / WM_CHAR / WM_SYSCHAR and every WM_INPUT keyboard make event is
//      dropped (WM_INPUT mouse events pass). Everything else goes to the original window procedure.
//   3. The menu itself reads the keys with GetAsyncKeyState (PollKeys, the hold-to-bind scan), which none of this touches.
// Key RELEASES are never dropped (WM_KEYUP / WM_SYSKEYUP, WM_INPUT break events, buffered DirectInput up entries): a key
// that was already held when the menu opened (the accelerator) or the Delete that opened it must reach the game as
// released, or the game would keep it held. A release of a key the game saw as up is harmless.
//
// v0.10.1 CONTROLLER TAP: every hotkey (dlaa.ini key_*) may also be bound to a button or the POV hat (D-pad) of a
// DirectInput wheel / gamepad / joystick / shifter ("Wheel7", "WheelUp", inject.cpp ParseBinding). CreateDevice of any
// device that is not a keyboard and not a mouse returns a wrapped device too (every method forwarded). Its SetDataFormat
// learns where the buttons and the first POV sit in the game's data format; every GetDeviceState / GetDeviceData the game
// makes (on its input thread) copies the button / POV state into an atomic per-device slot, read by ControllerState (render
// thread, PollKeys). v0.10.2: while the menu is open EVERY button and the POV are hidden from the game: all button bytes
// cleared / the POV centred in the state, every button-down entry and every non-centred POV entry dropped from the buffer.
// Axes, sliders (wheel, pedals) and everything else pass untouched. Releases (button up, POV centred) always pass. Only
// DirectInput devices the game itself opens are seen; XInput gamepads are not. A device the game drives with an action
// map (SetActionMap) has no known layout: it is neither tapped nor filtered.
#pragma once
#include <windows.h>
#include <unknwn.h>
#include <cstdint>

namespace KeySwallow {
using PFN_DI8Create = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
// Calls `real` and wraps the IDirectInput8A / IDirectInput8W it returns (other IIDs / failures: untouched).
HRESULT CreateDirectInput8(PFN_DI8Create real, HINSTANCE h, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer);
// Subclasses `hwnd` once per process (later calls do nothing). Any thread of this process.
void HookWindow(HWND hwnd);
// DLL unload: puts the original window procedure back if the window still exists and still points at ours.
void UnhookWindow();

// v0.10.1 controller binding codes (KeyBind::joy): 0 = none, 1..128 = button N (DirectInput button index
// N-1 in the game's data format), kJoyPovUp..kJoyPovLeft = POV hat 1 directions (a diagonal counts for both neighbours).
constexpr int kJoyMaxButton = 128;
constexpr int kJoyPovUp = 129, kJoyPovRight = 130, kJoyPovDown = 131, kJoyPovLeft = 132;
// POV direction bit of a code kJoyPovUp..kJoyPovLeft (1 up, 2 right, 4 down, 8 left); 0 for a button code.
inline unsigned JoyPovBit(int joy) { return (joy >= kJoyPovUp && joy <= kJoyPovLeft) ? 1u << (joy - kJoyPovUp) : 0u; }
// Render thread (PollKeys, once per top-level Present): the OR of every tapped controller -- 128-bit button mask
// (buttons[0] bit 0 = button 1) and the POV direction bits. A press seen in the buffered data since the previous call counts
// as down once even if released already. Devices not read by the game for 0.5 s count as all up. Also writes the
// "controller: button N pressed" discovery log lines (first press of each button / direction per device, 24 per run).
void ControllerState(uint32_t buttons[4], unsigned* povDirs);
}

// v0.10.2 inject.cpp: true while the tuning menu is open = no key / controller button reaches the game (see above). Any
// thread (the game's input / window threads; one relaxed atomic load).
bool MenuHidesInput();
// v0.10.1 inject.cpp: dlaa.ini controller_name filter -- true = this controller (product name) is tapped / filtered. Empty
// controller_name = every controller; else the name must contain the text (ASCII case-insensitive). Game's thread.
bool ControllerNameAllowed(const char* product);
const char* ControllerNameFilter();   // the controller_name text as matched (lower case; "" = no filter), for log lines
