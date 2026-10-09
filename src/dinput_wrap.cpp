// KeySwallow implementation (v0.10.0). See dinput_wrap.h.
#define DIRECTINPUT_VERSION 0x0800
#include "dinput_wrap.h"
#include <dinput.h>
#include <atomic>
#include <cstring>
#include <new>
#include "log.h"

namespace {

// ---- which keys --------------------------------------------------------------------------------------------------
constexpr int kMaxSwallow = 8;

inline bool VkSwallowed(int vk, const int* vks, int n) {
    for (int i = 0; i < n; ++i) if (vks[i] == vk) return true;
    return false;
}
// DirectInput key code (DIK_*) of a virtual key: the scan code, + 0x80 for an extended key. The navigation block keys
// map to their numpad twins without the extended flag, so they are listed (DIK_UP 0xC8 ... DIK_DELETE 0xD3).
DWORD DikOfVk(int vk) {
    switch (vk) {
    case VK_UP:     return 0xC8;
    case VK_DOWN:   return 0xD0;
    case VK_LEFT:   return 0xCB;
    case VK_RIGHT:  return 0xCD;
    case VK_DELETE: return 0xD3;
    case VK_INSERT: return 0xD2;
    case VK_HOME:   return 0xC7;
    case VK_END:    return 0xCF;
    case VK_PRIOR:  return 0xC9;
    case VK_NEXT:   return 0xD1;
    case VK_DIVIDE: return 0xB5;
    default: break;
    }
    const UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC_EX);
    if (!sc) return 0;
    DWORD d = sc & 0xFFu;
    if ((sc & 0xFF00u) == 0xE000u || (sc & 0xFF00u) == 0xE100u) d |= 0x80u;
    return d;
}
// The DIK codes to swallow now (0 = none).
int SwallowDiks(DWORD* diks) {
    int vks[kMaxSwallow];
    const int n = MenuSwallowKeys(vks, kMaxSwallow);
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const DWORD d = DikOfVk(vks[i]);
        if (d && d < 256) diks[m++] = d;
    }
    return m;
}

// ---- DirectInput wrappers ----------------------------------------------------------------------------------------
struct TraitsA {
    using DI = IDirectInput8A;            using Dev = IDirectInputDevice8A;
    using Str = LPCSTR;                   using EnumDevCb = LPDIENUMDEVICESCALLBACKA;
    using ActionFmt = LPDIACTIONFORMATA;  using EnumSemCb = LPDIENUMDEVICESBYSEMANTICSCBA;
    using CfgParams = LPDICONFIGUREDEVICESPARAMSA;
    using EnumObjCb = LPDIENUMDEVICEOBJECTSCALLBACKA; using ObjInst = LPDIDEVICEOBJECTINSTANCEA;
    using DevInst = LPDIDEVICEINSTANCEA;  using DevInstT = DIDEVICEINSTANCEA;
    using EnumEffCb = LPDIENUMEFFECTSCALLBACKA; using EffInfo = LPDIEFFECTINFOA;
    using ImgInfo = LPDIDEVICEIMAGEINFOHEADERA;
    static const IID& DiIid()  { return IID_IDirectInput8A; }
    static const IID& DevIid() { return IID_IDirectInputDevice8A; }
    static const char* DevName() { return "IDirectInputDevice8A"; }
};
struct TraitsW {
    using DI = IDirectInput8W;            using Dev = IDirectInputDevice8W;
    using Str = LPCWSTR;                  using EnumDevCb = LPDIENUMDEVICESCALLBACKW;
    using ActionFmt = LPDIACTIONFORMATW;  using EnumSemCb = LPDIENUMDEVICESBYSEMANTICSCBW;
    using CfgParams = LPDICONFIGUREDEVICESPARAMSW;
    using EnumObjCb = LPDIENUMDEVICEOBJECTSCALLBACKW; using ObjInst = LPDIDEVICEOBJECTINSTANCEW;
    using DevInst = LPDIDEVICEINSTANCEW;  using DevInstT = DIDEVICEINSTANCEW;
    using EnumEffCb = LPDIENUMEFFECTSCALLBACKW; using EffInfo = LPDIEFFECTINFOW;
    using ImgInfo = LPDIDEVICEIMAGEINFOHEADERW;
    static const IID& DiIid()  { return IID_IDirectInput8W; }
    static const IID& DevIid() { return IID_IDirectInputDevice8W; }
    static const char* DevName() { return "IDirectInputDevice8W"; }
};

// A keyboard device: every method forwarded to the real one; GetDeviceState / GetDeviceData filter the menu keys.
// Owns one reference on the real device (released with the wrapper's last reference).
template <class T>
class DevWrap final : public T::Dev {
public:
    explicit DevWrap(typename T::Dev* real) : real_(real) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == T::DevIid()) { *out = this; AddRef(); return S_OK; }
        return real_->QueryInterface(riid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const long r = --refs_;
        if (r == 0) { real_->Release(); delete this; }
        return (ULONG)r;
    }

    // the two filtered reads
    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD cb, LPVOID data) override {
        const HRESULT hr = real_->GetDeviceState(cb, data);
        if (SUCCEEDED(hr) && data && cb >= 256) {          // c_dfDIKeyboard: 256 key bytes
            DWORD diks[kMaxSwallow];
            const int n = SwallowDiks(diks);
            for (int i = 0; i < n; ++i) static_cast<BYTE*>(data)[diks[i]] = 0;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD cbObj, LPDIDEVICEOBJECTDATA rg, LPDWORD inOut, DWORD flags) override {
        const HRESULT hr = real_->GetDeviceData(cbObj, rg, inOut, flags);
        if (SUCCEEDED(hr) && rg && inOut && *inOut && cbObj >= sizeof(DIDEVICEOBJECTDATA_DX3)) {
            DWORD diks[kMaxSwallow];
            const int n = SwallowDiks(diks);
            if (n) {
                BYTE* const base = reinterpret_cast<BYTE*>(rg);
                DWORD kept = 0;
                for (DWORD i = 0; i < *inOut; ++i) {
                    const DIDEVICEOBJECTDATA_DX3* e = reinterpret_cast<const DIDEVICEOBJECTDATA_DX3*>(base + (size_t)i * cbObj);
                    bool drop = false;
                    if (e->dwData & 0x80u)                    // key DOWN entries only (releases always pass)
                        for (int k = 0; k < n; ++k) if (e->dwOfs == diks[k]) { drop = true; break; }
                    if (drop) continue;
                    if (kept != i) memmove(base + (size_t)kept * cbObj, base + (size_t)i * cbObj, cbObj);
                    ++kept;
                }
                *inOut = kept;
            }
        }
        return hr;
    }

    // everything else: forwarded untouched
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS c) override { return real_->GetCapabilities(c); }
    HRESULT STDMETHODCALLTYPE EnumObjects(typename T::EnumObjCb cb, LPVOID ref, DWORD fl) override { return real_->EnumObjects(cb, ref, fl); }
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID g, LPDIPROPHEADER p) override { return real_->GetProperty(g, p); }
    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID g, LPCDIPROPHEADER p) override { return real_->SetProperty(g, p); }
    HRESULT STDMETHODCALLTYPE Acquire() override { return real_->Acquire(); }
    HRESULT STDMETHODCALLTYPE Unacquire() override { return real_->Unacquire(); }
    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT f) override { return real_->SetDataFormat(f); }
    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE h) override { return real_->SetEventNotification(h); }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND w, DWORD fl) override { return real_->SetCooperativeLevel(w, fl); }
    HRESULT STDMETHODCALLTYPE GetObjectInfo(typename T::ObjInst o, DWORD obj, DWORD how) override { return real_->GetObjectInfo(o, obj, how); }
    HRESULT STDMETHODCALLTYPE GetDeviceInfo(typename T::DevInst d) override { return real_->GetDeviceInfo(d); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND w, DWORD fl) override { return real_->RunControlPanel(w, fl); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE h, DWORD v, REFGUID g) override { return real_->Initialize(h, v, g); }
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* o, LPUNKNOWN u) override {
        return real_->CreateEffect(g, e, o, u);
    }
    HRESULT STDMETHODCALLTYPE EnumEffects(typename T::EnumEffCb cb, LPVOID ref, DWORD t) override { return real_->EnumEffects(cb, ref, t); }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(typename T::EffInfo i, REFGUID g) override { return real_->GetEffectInfo(i, g); }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD s) override { return real_->GetForceFeedbackState(s); }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD c) override { return real_->SendForceFeedbackCommand(c); }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ref, DWORD fl) override {
        return real_->EnumCreatedEffectObjects(cb, ref, fl);
    }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE e) override { return real_->Escape(e); }
    HRESULT STDMETHODCALLTYPE Poll() override { return real_->Poll(); }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD cb, LPCDIDEVICEOBJECTDATA d, LPDWORD io, DWORD fl) override {
        return real_->SendDeviceData(cb, d, io, fl);
    }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(typename T::Str f, LPDIENUMEFFECTSINFILECALLBACK cb, LPVOID ref, DWORD fl) override {
        return real_->EnumEffectsInFile(f, cb, ref, fl);
    }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(typename T::Str f, DWORD n, LPDIFILEEFFECT e, DWORD fl) override {
        return real_->WriteEffectToFile(f, n, e, fl);
    }
    HRESULT STDMETHODCALLTYPE BuildActionMap(typename T::ActionFmt a, typename T::Str u, DWORD fl) override {
        return real_->BuildActionMap(a, u, fl);
    }
    HRESULT STDMETHODCALLTYPE SetActionMap(typename T::ActionFmt a, typename T::Str u, DWORD fl) override {
        return real_->SetActionMap(a, u, fl);
    }
    HRESULT STDMETHODCALLTYPE GetImageInfo(typename T::ImgInfo i) override { return real_->GetImageInfo(i); }

private:
    ~DevWrap() = default;
    typename T::Dev* real_;
    std::atomic<long> refs_{1};
};

std::atomic<int> g_kbWraps{0};

// The DirectInput8 object: CreateDevice of a keyboard returns a DevWrap; everything else forwarded untouched.
template <class T>
class DiWrap final : public T::DI {
public:
    explicit DiWrap(typename T::DI* real) : real_(real) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == T::DiIid()) { *out = this; AddRef(); return S_OK; }
        return real_->QueryInterface(riid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const long r = --refs_;
        if (r == 0) { real_->Release(); delete this; }
        return (ULONG)r;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID g, typename T::Dev** out, LPUNKNOWN outer) override {
        const HRESULT hr = real_->CreateDevice(g, out, outer);
        if (FAILED(hr) || !out || !*out || outer) return hr;
        bool keyboard = g == GUID_SysKeyboard;
        if (!keyboard) {
            typename T::DevInstT di{};
            di.dwSize = sizeof(di);
            if (SUCCEEDED((*out)->GetDeviceInfo(&di))) keyboard = GET_DIDEVICE_TYPE(di.dwDevType) == DI8DEVTYPE_KEYBOARD;
        }
        if (!keyboard) return hr;
        DevWrap<T>* w = new (std::nothrow) DevWrap<T>(*out);   // takes over the reference CreateDevice returned
        if (!w) return hr;
        *out = w;
        const int k = g_kbWraps.fetch_add(1);
        if (k < 4)
            Log("DirectInput: keyboard device wrapped (%s, #%d) -- the tuning menu keys are hidden from the game while the "
                "menu is open", T::DevName(), k + 1);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD t, typename T::EnumDevCb cb, LPVOID ref, DWORD fl) override {
        return real_->EnumDevices(t, cb, ref, fl);
    }
    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID g) override { return real_->GetDeviceStatus(g); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND w, DWORD fl) override { return real_->RunControlPanel(w, fl); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE h, DWORD v) override { return real_->Initialize(h, v); }
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID g, typename T::Str name, LPGUID out) override { return real_->FindDevice(g, name, out); }
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(typename T::Str user, typename T::ActionFmt a, typename T::EnumSemCb cb,
                                                     LPVOID ref, DWORD fl) override {
        return real_->EnumDevicesBySemantics(user, a, cb, ref, fl);
    }
    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK cb, typename T::CfgParams p, DWORD fl, LPVOID ref) override {
        return real_->ConfigureDevices(cb, p, fl, ref);
    }

private:
    ~DiWrap() = default;
    typename T::DI* real_;
    std::atomic<long> refs_{1};
};

// ---- window subclass ---------------------------------------------------------------------------------------------
std::atomic<HWND>     g_hwnd{nullptr};
std::atomic<LONG_PTR> g_origProc{0};
std::atomic<bool>     g_hookTried{false};

// The virtual key of a WM_CHAR / WM_SYSCHAR (its scan code + extended bit -> VK).
int VkOfCharMsg(LPARAM l) {
    const UINT sc = (UINT)((l >> 16) & 0xFF) | (((l >> 24) & 1) ? 0xE000u : 0u);
    return (int)MapVirtualKeyW(sc, MAPVK_VSC_TO_VK_EX);
}

LRESULT CALLBACK SubProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    const WNDPROC orig = reinterpret_cast<WNDPROC>(g_origProc.load(std::memory_order_acquire));
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_CHAR || m == WM_SYSCHAR || m == WM_INPUT) {
        int vks[kMaxSwallow];
        const int n = MenuSwallowKeys(vks, kMaxSwallow);
        if (n) {
            if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) {
                if (VkSwallowed((int)w, vks, n)) return 0;
            } else if (m == WM_CHAR || m == WM_SYSCHAR) {
                if (VkSwallowed(VkOfCharMsg(l), vks, n)) return 0;
            } else {                                           // WM_INPUT: keyboard make events of the menu keys
                alignas(8) BYTE buf[sizeof(RAWINPUT)];
                UINT size = sizeof(buf);
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
                    const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buf);
                    if (ri->header.dwType == RIM_TYPEKEYBOARD && !(ri->data.keyboard.Flags & RI_KEY_BREAK) &&
                        VkSwallowed((int)ri->data.keyboard.VKey, vks, n))
                        return DefWindowProcW(h, m, w, l);        // the system's raw-input cleanup, the game never sees it
                }
            }
        }
    }
    return orig ? CallWindowProcW(orig, h, m, w, l) : DefWindowProcW(h, m, w, l);
}

} // namespace

namespace KeySwallow {

HRESULT CreateDirectInput8(PFN_DI8Create real, HINSTANCE h, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    if (!real) return E_FAIL;
    const HRESULT hr = real(h, ver, riid, out, outer);
    if (FAILED(hr) || !out || !*out || outer) return hr;
    if (riid == IID_IDirectInput8A) {
        auto* w = new (std::nothrow) DiWrap<TraitsA>(static_cast<IDirectInput8A*>(*out));
        if (w) *out = static_cast<IDirectInput8A*>(w);
    } else if (riid == IID_IDirectInput8W) {
        auto* w = new (std::nothrow) DiWrap<TraitsW>(static_cast<IDirectInput8W*>(*out));
        if (w) *out = static_cast<IDirectInput8W*>(w);
    }
    return hr;
}

void HookWindow(HWND hwnd) {
    if (!hwnd || g_hookTried.exchange(true)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) { Log("tuning menu: window %p belongs to another process -- key messages not filtered", (void*)hwnd); return; }
    const LONG_PTR cur = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    if (!cur) { Log("tuning menu: window %p has no window procedure (%lu) -- key messages not filtered", (void*)hwnd, GetLastError()); return; }
    g_origProc.store(cur, std::memory_order_release);          // before the swap: SubProc may run at once
    SetLastError(0);
    const LONG_PTR prev = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SubProc));
    if (!prev && GetLastError() != 0) {
        Log("tuning menu: subclassing window %p failed (%lu) -- key messages not filtered", (void*)hwnd, GetLastError());
        return;
    }
    if (prev != cur) g_origProc.store(prev, std::memory_order_release);   // changed in between: chain to what was really there
    g_hwnd.store(hwnd, std::memory_order_release);
    Log("tuning menu: game window %p subclassed (key messages of the menu keys are dropped while the menu is open; key "
        "releases always pass)", (void*)hwnd);
}

void UnhookWindow() {
    HWND hwnd = g_hwnd.load(std::memory_order_acquire);
    if (!hwnd || !IsWindow(hwnd)) return;
    if (GetWindowLongPtrW(hwnd, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&SubProc)) return;   // someone chained after us
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, g_origProc.load(std::memory_order_acquire));
    g_hwnd.store(nullptr, std::memory_order_release);
}

}  // namespace KeySwallow
