// KeySwallow implementation (v0.10.0; v0.10.1 + the controller tap; v0.10.2: the open menu hides ALL keys / buttons).
// See dinput_wrap.h.
#define DIRECTINPUT_VERSION 0x0800
#include "dinput_wrap.h"
#include <dinput.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>
#include "log.h"

namespace {

// ---- v0.10.1 controller tap: per-device state slots (input thread writes, render thread reads) ---------------------
// One slot per wrapped controller (max 8; a 9th is still wrapped for the menu's hiding, its presses are not seen). Every field
// is an atomic: the game's input thread publishes in GetDeviceState / GetDeviceData, PollKeys reads via ControllerState
// on the render thread. No locks, nothing blocks in the device methods (the discovery log lines are written by the render
// thread). The slot is claimed when the device is wrapped and freed at the wrapper's last Release.
constexpr int       kMaxJoyDevs      = 8;
constexpr DWORD     kNoOfs           = 0xFFFFFFFFu;
constexpr ULONGLONG kJoyStaleMs      = 500;   // a device the game has not read for this long counts as all up
constexpr int       kJoyDiscoveryMax = 24;    // "controller: button N pressed" lines per run
constexpr int       kJoySetupLogMax  = 24;    // wrap / data-format lines per run (a game re-creating devices cannot spam)

struct JoySlot {
    std::atomic<int>       state{0};          // 0 free, 1 being set up / torn down, 2 live (ControllerState reads only 2)
    std::atomic<uint32_t>  btn[4];            // buttons down now: bit (i & 31) of word (i >> 5) = button i + 1
    std::atomic<uint32_t>  latch[4];          // presses since the last ControllerState (a tap between two Presents counts)
    std::atomic<unsigned>  pov{0};            // POV direction bits now (1 up, 2 right, 4 down, 8 left)
    std::atomic<unsigned>  povLatch{0};
    std::atomic<uint32_t>  seen[4];           // pressed at least once on this device (discovery log)
    std::atomic<unsigned>  seenPov{0};
    std::atomic<uint32_t>  fresh[4];          // first presses not logged yet (the render thread logs + clears them)
    std::atomic<unsigned>  freshPov{0};
    std::atomic<ULONGLONG> stamp{0};          // GetTickCount64 of the game's last successful read, 0 = none / unacquired
    char                   name[64];          // product name (written before state 2; logged with %.63s)
};
JoySlot g_joy[kMaxJoyDevs];
std::atomic<int> g_joyDiscLogs{0};
std::atomic<int> g_joySetupLogs{0};
std::atomic<int> g_joyWraps{0};

// The live part of a slot -> all up, not read (a failed read, Unacquire, a new data format).
void JoyClearLive(JoySlot& s) {
    for (int w = 0; w < 4; ++w) { s.btn[w].store(0, std::memory_order_relaxed); s.latch[w].store(0, std::memory_order_relaxed); }
    s.pov.store(0, std::memory_order_relaxed);
    s.povLatch.store(0, std::memory_order_relaxed);
    s.stamp.store(0, std::memory_order_release);
}
JoySlot* JoyClaim(const char* name) {
    for (JoySlot& s : g_joy) {
        int expect = 0;
        if (!s.state.compare_exchange_strong(expect, 1, std::memory_order_acq_rel)) continue;
        JoyClearLive(s);
        for (int w = 0; w < 4; ++w) { s.seen[w].store(0, std::memory_order_relaxed); s.fresh[w].store(0, std::memory_order_relaxed); }
        s.seenPov.store(0, std::memory_order_relaxed);
        s.freshPov.store(0, std::memory_order_relaxed);
        snprintf(s.name, sizeof(s.name), "%s", name);
        s.state.store(2, std::memory_order_release);
        return &s;
    }
    return nullptr;
}
void JoyFree(JoySlot* s) {
    if (!s) return;
    s->state.store(1, std::memory_order_release);
    JoyClearLive(*s);
    s->state.store(0, std::memory_order_release);
}

// POV value -> direction bits (1 up, 2 right, 4 down, 8 left). Centred = LOWORD 0xFFFF (DirectInput: 0xFFFFFFFF), else
// hundredths of a degree clockwise from up. 8-way sectors: a cardinal (0 / 9000 / 18000 / 27000) is only itself, a diagonal
// (4500 = up-right ...) sets both neighbours; a continuous hat within 22.5 deg of a cardinal is only that cardinal.
unsigned PovDirs(DWORD v) {
    if ((v & 0xFFFFu) == 0xFFFFu) return 0;
    const DWORD a = v % 36000u;
    unsigned d = 0;
    if (a <= 6750u || a >= 29250u)  d |= 1u;
    if (a >= 2250u && a <= 15750u)  d |= 2u;
    if (a >= 11250u && a <= 24750u) d |= 4u;
    if (a >= 20250u && a <= 33750u) d |= 8u;
    return d;
}

const char* DevTypeName(DWORD t) {
    switch (t) {
    case DI8DEVTYPE_DEVICE:        return "device";
    case DI8DEVTYPE_JOYSTICK:      return "joystick";
    case DI8DEVTYPE_GAMEPAD:       return "gamepad";
    case DI8DEVTYPE_DRIVING:       return "driving (wheel)";
    case DI8DEVTYPE_FLIGHT:        return "flight";
    case DI8DEVTYPE_1STPERSON:     return "1st-person";
    case DI8DEVTYPE_DEVICECTRL:    return "device control";
    case DI8DEVTYPE_SCREENPOINTER: return "screen pointer";
    case DI8DEVTYPE_REMOTE:        return "remote";
    case DI8DEVTYPE_SUPPLEMENTAL:  return "supplemental (pedals / shifter)";
    default:                       return "other";
    }
}
void ProductName(const DIDEVICEINSTANCEA& di, char* out, size_t cap) { snprintf(out, cap, "%s", di.tszProductName); }
void ProductName(const DIDEVICEINSTANCEW& di, char* out, size_t cap) {
    char u8[MAX_PATH * 3];
    if (!WideCharToMultiByte(CP_UTF8, 0, di.tszProductName, -1, u8, (int)sizeof(u8), nullptr, nullptr)) u8[0] = 0;
    snprintf(out, cap, "%s", u8[0] ? u8 : "?");
}

// ---- device read filters (the Tap policy of DevWrap) ---------------------------------------------------------------
// Keyboard: v0.10.2 while the menu is open GetDeviceState clears ALL 256 key bytes and GetDeviceData drops EVERY key-DOWN
// entry (v0.10.0 / v0.10.1: only the 5 menu keys). Key-up entries always pass.
struct KbTap {
    void OnFormat(LPCDIDATAFORMAT) {}
    void OnActionMap() {}
    void OnUnacquire() {}
    void OnState(HRESULT hr, DWORD cb, LPVOID data) {
        if (SUCCEEDED(hr) && data && cb >= 256 && MenuHidesInput())   // c_dfDIKeyboard: 256 key bytes
            memset(data, 0, 256);
    }
    void OnData(HRESULT hr, DWORD cbObj, LPDIDEVICEOBJECTDATA rg, LPDWORD inOut) {
        if (SUCCEEDED(hr) && rg && inOut && *inOut && cbObj >= sizeof(DIDEVICEOBJECTDATA_DX3) && MenuHidesInput()) {
            BYTE* const base = reinterpret_cast<BYTE*>(rg);
            DWORD kept = 0;
            for (DWORD i = 0; i < *inOut; ++i) {
                const DIDEVICEOBJECTDATA_DX3* e = reinterpret_cast<const DIDEVICEOBJECTDATA_DX3*>(base + (size_t)i * cbObj);
                if (e->dwData & 0x80u) continue;              // key DOWN entry: dropped (releases always pass)
                if (kept != i) memmove(base + (size_t)kept * cbObj, base + (size_t)i * cbObj, cbObj);
                ++kept;
            }
            *inOut = kept;
        }
    }
};

// v0.10.1 controller (wheel / gamepad / joystick / shifter): taps the button / POV state into its JoySlot, then (v0.10.2)
// hides EVERY button and the POV from the game while the menu is open (v0.10.1: only the menu actions' controller parts);
// axes and sliders are never touched. The layout comes from the game's SetDataFormat:
// the i-th button object in rgodf order is button i (GUID_Button, or a NULL guid with a DIDFT_BUTTON type, which is how
// c_dfDIJoystick / c_dfDIJoystick2 list their buttons), the first GUID_POV / DIDFT_POV object is the hat. Until a
// SetDataFormat is seen the c_dfDIJoystick2 layout is assumed (buttons at DIJOFS_BUTTON(0..127), POV 1 at DIJOFS_POV(0)).
// The layout fields are touched only on the game's input thread(s) (SetDataFormat is legal only while unacquired).
struct JoyTap {
    JoySlot* slot = nullptr;
    char     name[64] = {};
    int      nBtn = KeySwallow::kJoyMaxButton;
    DWORD    btnOfs[KeySwallow::kJoyMaxButton];
    DWORD    povOfs = DIJOFS_POV(0);

    explicit JoyTap(const char* product) {
        snprintf(name, sizeof(name), "%s", product);
        for (int i = 0; i < KeySwallow::kJoyMaxButton; ++i) btnOfs[i] = (DWORD)DIJOFS_BUTTON(i);
        slot = JoyClaim(name);
    }
    ~JoyTap() { JoyFree(slot); }
    JoyTap(const JoyTap&) = delete;
    JoyTap& operator=(const JoyTap&) = delete;

    int ButtonAt(DWORD ofs) const {
        for (int i = 0; i < nBtn; ++i) if (btnOfs[i] == ofs) return i;
        return -1;
    }
    // Publishes the new live state (+ presses for the latch / discovery log) and stamps the read.
    void Publish(const uint32_t b[4], unsigned pd, const uint32_t pressed[4], unsigned povPressed) {
        JoySlot& s = *slot;
        const bool discover = g_joyDiscLogs.load(std::memory_order_relaxed) < kJoyDiscoveryMax;
        for (int w = 0; w < 4; ++w) {
            s.btn[w].store(b[w], std::memory_order_relaxed);
            if (!pressed[w]) continue;
            s.latch[w].fetch_or(pressed[w], std::memory_order_relaxed);
            if (discover) {
                const uint32_t nw = pressed[w] & ~s.seen[w].load(std::memory_order_relaxed);
                if (nw) {
                    const uint32_t first = nw & ~s.seen[w].fetch_or(nw, std::memory_order_relaxed);
                    if (first) s.fresh[w].fetch_or(first, std::memory_order_relaxed);
                }
            }
        }
        s.pov.store(pd, std::memory_order_relaxed);
        if (povPressed) {
            s.povLatch.fetch_or(povPressed, std::memory_order_relaxed);
            if (discover) {
                const unsigned nw = povPressed & ~s.seenPov.load(std::memory_order_relaxed);
                if (nw) {
                    const unsigned first = nw & ~s.seenPov.fetch_or(nw, std::memory_order_relaxed);
                    if (first) s.freshPov.fetch_or(first, std::memory_order_relaxed);
                }
            }
        }
        s.stamp.store(GetTickCount64(), std::memory_order_release);
    }

    void OnFormat(LPCDIDATAFORMAT f) {
        if (!f || !f->rgodf || f->dwObjSize < sizeof(DIOBJECTDATAFORMAT)) return;   // keep the previous layout
        DWORD ofs[KeySwallow::kJoyMaxButton];
        int nb = 0, total = 0;
        DWORD pov = kNoOfs;
        const BYTE* const base = reinterpret_cast<const BYTE*>(f->rgodf);
        for (DWORD i = 0; i < f->dwNumObjs; ++i) {
            const DIOBJECTDATAFORMAT* o = reinterpret_cast<const DIOBJECTDATAFORMAT*>(base + (size_t)i * f->dwObjSize);
            const DWORD ty = DIDFT_GETTYPE(o->dwType);
            const bool isBtn = o->pguid ? (*o->pguid == GUID_Button) : (ty & DIDFT_BUTTON) != 0;
            const bool isPov = o->pguid ? (*o->pguid == GUID_POV)    : (ty & DIDFT_POV) != 0;
            if (isBtn) { if (nb < KeySwallow::kJoyMaxButton) ofs[nb++] = o->dwOfs; ++total; }
            else if (isPov && pov == kNoOfs) pov = o->dwOfs;
        }
        nBtn = nb;
        for (int i = 0; i < nb; ++i) btnOfs[i] = ofs[i];
        povOfs = pov;
        if (slot) JoyClearLive(*slot);                     // new layout: the old state means nothing
        if (g_joySetupLogs.fetch_add(1, std::memory_order_relaxed) < kJoySetupLogMax) {
            char povTxt[48];
            if (pov != kNoOfs) snprintf(povTxt, sizeof(povTxt), "POV hat at offset %lu", (unsigned long)pov);
            else               snprintf(povTxt, sizeof(povTxt), "no POV hat (WheelUp..WheelLeft never fire)");
            Log("controller '%s': data format set -- %d button(s)%s (Wheel1..Wheel%d), %s, %lu-byte state", name, nb,
                total > nb ? " (more than 128: the rest ignored)" : "", nb, povTxt, (unsigned long)f->dwDataSize);
        }
    }
    // An action map (SetActionMap) replaces the data format: buffered dwOfs values are then the game's own action ids, not
    // offsets, so the layout is unknown -- this device is no longer tapped or filtered (its buttons pass to the game as is).
    void OnActionMap() {
        nBtn = 0;
        povOfs = kNoOfs;
        if (slot) JoyClearLive(*slot);
        if (g_joySetupLogs.fetch_add(1, std::memory_order_relaxed) < kJoySetupLogMax)
            Log("controller '%s': the game uses a DirectInput action map -- its buttons cannot be bound (Wheel* bindings "
                "ignore this device)", name);
    }
    void OnUnacquire() { if (slot) JoyClearLive(*slot); }

    void OnState(HRESULT hr, DWORD cb, LPVOID data) {
        if (FAILED(hr)) { if (slot) JoyClearLive(*slot); return; }      // lost / not acquired: count as all up
        if (!data) return;
        BYTE* const d = static_cast<BYTE*>(data);
        const bool havePov = povOfs != kNoOfs && povOfs <= cb && cb - povOfs >= sizeof(DWORD);
        DWORD pv = 0xFFFFFFFFu;
        if (havePov) memcpy(&pv, d + povOfs, sizeof(pv));
        if (slot) {                                         // tap BEFORE the hiding: PollKeys needs the real state
            uint32_t b[4] = {};
            for (int i = 0; i < nBtn; ++i) {
                const DWORD o = btnOfs[i];
                if (o < cb && (d[o] & 0x80u)) b[i >> 5] |= 1u << (i & 31);
            }
            const unsigned pd = havePov ? PovDirs(pv) : 0u;
            Publish(b, pd, b, pd);                          // held = pressed for the latch (a short tap between Presents)
        }
        if (!MenuHidesInput()) return;
        for (int i = 0; i < nBtn; ++i) {                    // v0.10.2: every button
            const DWORD o = btnOfs[i];
            if (o < cb) d[o] = 0;                           // the game sees the button up (a held one: released)
        }
        if (havePov && PovDirs(pv)) {                       // the POV centred (an already centred one is left as it is)
            const DWORD centred = 0xFFFFFFFFu;
            memcpy(d + povOfs, &centred, sizeof(centred));
        }
    }

    void OnData(HRESULT hr, DWORD cbObj, LPDIDEVICEOBJECTDATA rg, LPDWORD inOut) {
        if (FAILED(hr)) { if (slot) JoyClearLive(*slot); return; }
        const bool entries = rg && inOut && *inOut && cbObj >= sizeof(DIDEVICEOBJECTDATA_DX3);
        uint32_t b[4] = {}, pressed[4] = {};
        unsigned pd = 0, povPressed = 0;
        if (slot) {
            for (int w = 0; w < 4; ++w) b[w] = slot->btn[w].load(std::memory_order_relaxed);
            pd = slot->pov.load(std::memory_order_relaxed);
        }
        const bool hide = entries && MenuHidesInput();     // v0.10.2: every button-down / non-centred POV entry
        if (entries) {
            BYTE* const base = reinterpret_cast<BYTE*>(rg);
            DWORD kept = 0;
            for (DWORD i = 0; i < *inOut; ++i) {
                const DIDEVICEOBJECTDATA_DX3* e = reinterpret_cast<const DIDEVICEOBJECTDATA_DX3*>(base + (size_t)i * cbObj);
                bool drop = false;
                const int bi = ButtonAt(e->dwOfs);
                if (bi >= 0) {
                    const uint32_t bit = 1u << (bi & 31);
                    if (e->dwData & 0x80u) {
                        b[bi >> 5] |= bit; pressed[bi >> 5] |= bit;
                        drop = hide;                                    // button DOWN entries only (releases always pass)
                    } else {
                        b[bi >> 5] &= ~bit;
                    }
                } else if (povOfs != kNoOfs && e->dwOfs == povOfs) {
                    const unsigned dirs = PovDirs(e->dwData);
                    pd = dirs; povPressed |= dirs;
                    drop = hide && dirs != 0;                           // a centred entry (dirs 0) = release, passes
                }
                if (drop) continue;
                if (kept != i) memmove(base + (size_t)kept * cbObj, base + (size_t)i * cbObj, cbObj);
                ++kept;
            }
            *inOut = kept;
        }
        if (slot) Publish(b, pd, pressed, povPressed);      // also with no entries: the device is being read (fresh stamp)
    }
};

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

// A wrapped device: every method forwarded to the real one; the Tap policy (KbTap for a keyboard, v0.10.1 JoyTap for a
// controller) sees SetDataFormat / Unacquire / GetDeviceState / GetDeviceData after the real call and may filter the reads.
// Owns one reference on the real device (released with the wrapper's last reference; the Tap is destroyed with it).
template <class T, class Tap>
class DevWrap final : public T::Dev {
public:
    template <class... A>
    explicit DevWrap(typename T::Dev* real, A&&... a) : real_(real), tap_(std::forward<A>(a)...) {}

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

    // the two filtered reads (+ the two calls the Tap must see)
    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD cb, LPVOID data) override {
        const HRESULT hr = real_->GetDeviceState(cb, data);
        tap_.OnState(hr, cb, data);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD cbObj, LPDIDEVICEOBJECTDATA rg, LPDWORD inOut, DWORD flags) override {
        const HRESULT hr = real_->GetDeviceData(cbObj, rg, inOut, flags);
        tap_.OnData(hr, cbObj, rg, inOut);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT f) override {
        const HRESULT hr = real_->SetDataFormat(f);
        if (SUCCEEDED(hr)) tap_.OnFormat(f);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Unacquire() override {
        const HRESULT hr = real_->Unacquire();
        tap_.OnUnacquire();
        return hr;
    }

    // everything else: forwarded untouched
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS c) override { return real_->GetCapabilities(c); }
    HRESULT STDMETHODCALLTYPE EnumObjects(typename T::EnumObjCb cb, LPVOID ref, DWORD fl) override { return real_->EnumObjects(cb, ref, fl); }
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID g, LPDIPROPHEADER p) override { return real_->GetProperty(g, p); }
    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID g, LPCDIPROPHEADER p) override { return real_->SetProperty(g, p); }
    HRESULT STDMETHODCALLTYPE Acquire() override { return real_->Acquire(); }
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
        const HRESULT hr = real_->SetActionMap(a, u, fl);
        if (SUCCEEDED(hr)) tap_.OnActionMap();       // v0.10.1 (controller: layout unknown from now on; keyboard: nothing)
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetImageInfo(typename T::ImgInfo i) override { return real_->GetImageInfo(i); }

    const Tap& tap() const { return tap_; }

private:
    ~DevWrap() = default;
    typename T::Dev* real_;
    Tap tap_;
    std::atomic<long> refs_{1};
};

std::atomic<int> g_kbWraps{0};

// The DirectInput8 object: CreateDevice of a keyboard returns a DevWrap<KbTap>, of a controller (v0.10.1: any device that
// is not a keyboard and not a mouse) a DevWrap<JoyTap>; everything else forwarded untouched.
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
        typename T::DevInstT di{};
        bool haveInfo = false;
        if (!keyboard) {
            di.dwSize = sizeof(di);
            haveInfo = SUCCEEDED((*out)->GetDeviceInfo(&di));
            if (haveInfo) keyboard = GET_DIDEVICE_TYPE(di.dwDevType) == DI8DEVTYPE_KEYBOARD;
        }
        if (!keyboard) {
            // v0.10.1 controller tap: anything with a known type that is not a mouse (wheel, gamepad, joystick, shifter ...)
            if (g == GUID_SysMouse || !haveInfo || GET_DIDEVICE_TYPE(di.dwDevType) == DI8DEVTYPE_MOUSE) return hr;
            char product[64];
            ProductName(di, product, sizeof(product));
            if (!ControllerNameAllowed(product)) {         // dlaa.ini controller_name set and not part of this name
                if (g_joySetupLogs.fetch_add(1, std::memory_order_relaxed) < kJoySetupLogMax)
                    Log("DirectInput: controller '%s' not tapped (controller_name = \"%s\" does not match) -- passed to the "
                        "game unwrapped", product, ControllerNameFilter());
                return hr;
            }
            auto* jw = new (std::nothrow) DevWrap<T, JoyTap>(*out, product);   // takes over the reference CreateDevice returned
            if (!jw) return hr;
            *out = jw;
            const int k = g_joyWraps.fetch_add(1, std::memory_order_relaxed);
            if (g_joySetupLogs.fetch_add(1, std::memory_order_relaxed) < kJoySetupLogMax)
                Log("DirectInput: controller device wrapped (%s, #%d): '%s', type %s (0x%02lX) -- its buttons / D-pad can be bound "
                    "to the mod's hotkeys (dlaa.ini key_* = WheelN / WheelUp ...); its buttons / D-pad are hidden from the game while "
                    "the menu is open (axes pass)%s",
                    T::DevName(), k + 1, product, DevTypeName(GET_DIDEVICE_TYPE(di.dwDevType)),
                    (unsigned long)GET_DIDEVICE_TYPE(di.dwDevType),
                    jw->tap().slot ? "" : " -- NO STATE SLOT LEFT (8 controllers): its presses are not seen by the mod");
            return hr;
        }
        DevWrap<T, KbTap>* w = new (std::nothrow) DevWrap<T, KbTap>(*out);   // takes over the reference CreateDevice returned
        if (!w) return hr;
        *out = w;
        const int k = g_kbWraps.fetch_add(1);
        if (k < 4)
            Log("DirectInput: keyboard device wrapped (%s, #%d) -- no key reaches the game while the tuning menu is open "
                "(releases pass)", T::DevName(), k + 1);
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

// v0.10.2: while the menu is open EVERY key-down / character message and every WM_INPUT keyboard make event is dropped
// (v0.10.0 / v0.10.1: only those of the 5 menu keys). Key-up messages, WM_INPUT break events and WM_INPUT mouse pass.
LRESULT CALLBACK SubProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    const WNDPROC orig = reinterpret_cast<WNDPROC>(g_origProc.load(std::memory_order_acquire));
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_CHAR || m == WM_SYSCHAR || m == WM_INPUT) && MenuHidesInput()) {
        if (m == WM_SYSKEYDOWN && w == VK_F4) return orig ? CallWindowProcW(orig, h, m, w, l) : DefWindowProcW(h, m, w, l);   // Alt+F4 (close the game) always passes
        if (m != WM_INPUT) return 0;
        alignas(8) BYTE buf[sizeof(RAWINPUT)];                 // WM_INPUT: keyboard make events only
        UINT size = sizeof(buf);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
            const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buf);
            if (ri->header.dwType == RIM_TYPEKEYBOARD && !(ri->data.keyboard.Flags & RI_KEY_BREAK))
                return DefWindowProcW(h, m, w, l);            // the system's raw-input cleanup, the game never sees it
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
    Log("tuning menu: game window %p subclassed (every key-down / character message is dropped while the menu is open; "
        "key releases always pass)", (void*)hwnd);
}

void UnhookWindow() {
    HWND hwnd = g_hwnd.load(std::memory_order_acquire);
    if (!hwnd || !IsWindow(hwnd)) return;
    if (GetWindowLongPtrW(hwnd, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&SubProc)) return;   // someone chained after us
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, g_origProc.load(std::memory_order_acquire));
    g_hwnd.store(nullptr, std::memory_order_release);
}

// v0.10.1: see dinput_wrap.h. Render thread only (the latches and the discovery bits are consumed here).
void ControllerState(uint32_t buttons[4], unsigned* povDirs) {
    for (int w = 0; w < 4; ++w) buttons[w] = 0;
    unsigned pov = 0;
    const ULONGLONG now = GetTickCount64();
    for (JoySlot& s : g_joy) {
        if (s.state.load(std::memory_order_acquire) != 2) continue;
        uint32_t lat[4];
        for (int w = 0; w < 4; ++w) lat[w] = s.latch[w].exchange(0, std::memory_order_relaxed);
        const unsigned povLat = s.povLatch.exchange(0, std::memory_order_relaxed);
        const ULONGLONG st = s.stamp.load(std::memory_order_acquire);
        if (st && st + kJoyStaleMs >= now) {               // read by the game recently (the input thread may stamp after `now`)
            for (int w = 0; w < 4; ++w) buttons[w] |= s.btn[w].load(std::memory_order_relaxed) | lat[w];
            pov |= s.pov.load(std::memory_order_relaxed) | povLat;
        }
        // discovery aid: the first press of each button / direction of this device, so the user can find the number to bind
        for (int w = 0; w < 4; ++w) {
            uint32_t f = s.fresh[w].exchange(0, std::memory_order_relaxed);
            for (int b = 0; f && b < 32; ++b) {
                if (!(f & (1u << b))) continue;
                f &= ~(1u << b);
                const int n = g_joyDiscLogs.fetch_add(1, std::memory_order_relaxed);
                if (n >= kJoyDiscoveryMax) break;
                const int btn = w * 32 + b + 1;
                Log("controller '%.63s': button %d pressed -- bind it in dlaa.ini as Wheel%d (first press of each button is "
                    "logged once%s)", s.name, btn, btn, n == kJoyDiscoveryMax - 1 ? "; this was the last such line this run" : "");
            }
        }
        unsigned fp = s.freshPov.exchange(0, std::memory_order_relaxed);
        static const char* const kDirName[4] = { "up", "right", "down", "left" };
        static const char* const kDirTok[4]  = { "WheelUp", "WheelRight", "WheelDown", "WheelLeft" };
        for (int d = 0; fp && d < 4; ++d) {
            if (!(fp & (1u << d))) continue;
            fp &= ~(1u << d);
            const int n = g_joyDiscLogs.fetch_add(1, std::memory_order_relaxed);
            if (n >= kJoyDiscoveryMax) break;
            Log("controller '%.63s': D-pad %s -- bind it in dlaa.ini as %s%s", s.name, kDirName[d], kDirTok[d],
                n == kJoyDiscoveryMax - 1 ? " (the last such line this run)" : "");
        }
    }
    *povDirs = pov;
}

}  // namespace KeySwallow
