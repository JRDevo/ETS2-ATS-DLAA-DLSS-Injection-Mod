// XrHook -- see xr_hook.h. v0.10.3 phase 27: OpenXR swapchain release hook (GetProcAddress -> negotiation -> GIPA thunks).
#include "xr_hook.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <utility>
#include "MinHook.h"
#include "log.h"

namespace XrHook {

namespace {

// ---- the minimal OpenXR / loader-interface types (openxr.h + openxr_loader_negotiation.h, 1.0; x64 only) ------------
#define XRH_CALL __stdcall
typedef int32_t  XrResult;                       // >= 0 = success (XR_SUCCEEDED)
typedef uint64_t XrVersion;
typedef void*    XrInstance;                     // XR_DEFINE_HANDLE = a pointer on 64-bit
typedef void*    XrSwapchain;
typedef void (XRH_CALL* PFN_xrVoidFunction)(void);
typedef XrResult (XRH_CALL* PFN_xrGetInstanceProcAddr)(XrInstance, const char*, PFN_xrVoidFunction*);
typedef XrResult (XRH_CALL* PFN_xrReleaseSwapchainImage)(XrSwapchain, const void* releaseInfo);
typedef XrResult (XRH_CALL* PFN_xrAcquireSwapchainImage)(XrSwapchain, const void* acquireInfo, uint32_t* index);
typedef XrResult (XRH_CALL* PFN_xrEnumerateSwapchainImages)(XrSwapchain, uint32_t capacity, uint32_t* countOut, void* images);
typedef XrResult (XRH_CALL* PFN_xrDestroySwapchain)(XrSwapchain);
enum : int32_t {                                 // XrLoaderInterfaceStructs
    kLisLoaderInfo = 1, kLisApiLayerRequest = 2, kLisRuntimeRequest = 3,
};
constexpr int32_t kTypeSwapchainImageD3D11 = 1000027001;   // XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR
struct XrNegotiateLoaderInfo {
    int32_t   structType;
    uint32_t  structVersion;
    size_t    structSize;
    uint32_t  minInterfaceVersion, maxInterfaceVersion;
    XrVersion minApiVersion, maxApiVersion;
};
struct XrNegotiateRuntimeRequest {
    int32_t   structType;
    uint32_t  structVersion;
    size_t    structSize;
    uint32_t  runtimeInterfaceVersion;
    XrVersion runtimeApiVersion;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr;
};
struct XrNegotiateApiLayerRequest {
    int32_t   structType;
    uint32_t  structVersion;
    size_t    structSize;
    uint32_t  layerInterfaceVersion;
    XrVersion layerApiVersion;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr;
    void*     createApiLayerInstance;
};
typedef XrResult (XRH_CALL* PFN_NegRuntime)(const XrNegotiateLoaderInfo*, XrNegotiateRuntimeRequest*);
typedef XrResult (XRH_CALL* PFN_NegLayer)(const XrNegotiateLoaderInfo*, const char* layerName, XrNegotiateApiLayerRequest*);
struct XrSwapchainImageD3D11 {                   // XrSwapchainImageD3D11KHR
    int32_t type;
    void*   next;
    void*   texture;                             // ID3D11Texture2D*
};
// v0.10.3 phase 28: the view / frame / space types (openxr.h 1.0 layouts; natural x64 alignment matches the C headers)
typedef void*   XrSession;
typedef void*   XrSpace;
typedef int64_t XrTime;
struct XrQuaternionf { float x, y, z, w; };
struct XrVector3f    { float x, y, z; };
struct XrPosef       { XrQuaternionf orientation; XrVector3f position; };
struct XrFovf        { float angleLeft, angleRight, angleUp, angleDown; };
struct XrViewLocateInfo { int32_t type; const void* next; int32_t viewConfigurationType; XrTime displayTime; XrSpace space; };
struct XrViewState      { int32_t type; void* next; uint64_t viewStateFlags; };
struct XrView           { int32_t type; void* next; XrPosef pose; XrFovf fov; };
struct XrReferenceSpaceCreateInfo { int32_t type; const void* next; int32_t referenceSpaceType; XrPosef poseInReferenceSpace; };
struct XrCompositionLayerBaseHeader { int32_t type; const void* next; uint64_t layerFlags; XrSpace space; };
struct XrSwapchainSubImage { XrSwapchain swapchain; int32_t x, y, w, h; uint32_t imageArrayIndex; };   // imageRect flattened
struct XrCompositionLayerProjectionView { int32_t type; const void* next; XrPosef pose; XrFovf fov; XrSwapchainSubImage subImage; };
struct XrCompositionLayerProjection {
    int32_t type; const void* next; uint64_t layerFlags; XrSpace space;
    uint32_t viewCount; const XrCompositionLayerProjectionView* views;
};
struct XrFrameEndInfo {
    int32_t type; const void* next; XrTime displayTime; int32_t environmentBlendMode;
    uint32_t layerCount; const XrCompositionLayerBaseHeader* const* layers;
};
static_assert(sizeof(XrView) == 64 && sizeof(XrCompositionLayerProjectionView) == 96 && sizeof(XrFrameEndInfo) == 40 &&
              sizeof(XrCompositionLayerProjection) == 48, "OpenXR struct layout (x64)");
constexpr int32_t kTypeCompositionLayerProjection = 35;   // XR_TYPE_COMPOSITION_LAYER_PROJECTION
constexpr int32_t kViewConfigPrimaryStereo = 2;            // XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO
typedef XrResult (XRH_CALL* PFN_xrLocateViews)(XrSession, const XrViewLocateInfo*, XrViewState*, uint32_t, uint32_t*, XrView*);
typedef XrResult (XRH_CALL* PFN_xrEndFrame)(XrSession, const XrFrameEndInfo*);
typedef XrResult (XRH_CALL* PFN_xrCreateReferenceSpace)(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace*);
struct XrSpaceLocation { int32_t type; void* next; uint64_t locationFlags; XrPosef pose; };   // phase 28b (diagnostics)
typedef XrResult (XRH_CALL* PFN_xrLocateSpace)(XrSpace, XrSpace, XrTime, XrSpaceLocation*);
// v0.10.3 phase 29: the overlay quads -- swapchain / quad layer / space types, values and layouts copied from the OpenXR SDK
// header openxr.h (OpenXR-SDK 1.1.62, E:/OpenXR-EyeCapture/build/_deps/openxr_sdk-src/include/openxr/openxr.h)
typedef int64_t XrDuration;
struct XrSwapchainCreateInfo {
    int32_t type; const void* next; uint64_t createFlags; uint64_t usageFlags; int64_t format;
    uint32_t sampleCount, width, height, faceCount, arraySize, mipCount;
};
struct XrSwapchainImageAcquireInfo { int32_t type; const void* next; };
struct XrSwapchainImageWaitInfo    { int32_t type; const void* next; XrDuration timeout; };
struct XrSwapchainImageReleaseInfo { int32_t type; const void* next; };
struct XrExtent2Df { float width, height; };
struct XrCompositionLayerQuad {
    int32_t type; const void* next; uint64_t layerFlags; XrSpace space; int32_t eyeVisibility;
    XrSwapchainSubImage subImage; XrPosef pose; XrExtent2Df size;
};
static_assert(sizeof(XrSwapchainCreateInfo) == 64 && sizeof(XrSwapchainImageWaitInfo) == 24 && sizeof(XrSwapchainSubImage) == 32 &&
              sizeof(XrCompositionLayerQuad) == 112 && offsetof(XrCompositionLayerQuad, eyeVisibility) == 32 &&
              offsetof(XrCompositionLayerQuad, subImage) == 40 && offsetof(XrCompositionLayerQuad, pose) == 72 &&
              offsetof(XrCompositionLayerQuad, size) == 100 && sizeof(XrReferenceSpaceCreateInfo) == 48,
              "OpenXR struct layout (x64)");
constexpr int32_t  kTypeSwapchainCreateInfo       = 9;           // XR_TYPE_SWAPCHAIN_CREATE_INFO
constexpr int32_t  kTypeCompositionLayerQuad      = 36;          // XR_TYPE_COMPOSITION_LAYER_QUAD
constexpr int32_t  kTypeReferenceSpaceCreateInfo  = 37;          // XR_TYPE_REFERENCE_SPACE_CREATE_INFO
constexpr int32_t  kTypeSwapchainImageAcquireInfo = 55;          // XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO
constexpr int32_t  kTypeSwapchainImageWaitInfo    = 56;          // XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO
constexpr int32_t  kTypeSwapchainImageReleaseInfo = 57;          // XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO
constexpr uint64_t kUsageColorAttachment = 0x00000001;           // XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
constexpr uint64_t kUsageSampled         = 0x00000020;           // XR_SWAPCHAIN_USAGE_SAMPLED_BIT
constexpr uint64_t kLayerBlendSrcAlpha   = 0x00000002;           // XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
constexpr uint64_t kLayerUnpremultiplied = 0x00000004;           // XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT
constexpr int32_t  kEyeVisibilityBoth    = 0;                    // XR_EYE_VISIBILITY_BOTH
constexpr int32_t  kRefSpaceView = 1, kRefSpaceLocal = 2;        // XR_REFERENCE_SPACE_TYPE_VIEW / _LOCAL
constexpr XrDuration kInfiniteDuration   = 0x7fffffffffffffffLL; // XR_INFINITE_DURATION
constexpr XrResult kTimeoutExpired = 1, kErrSessionLost = -17;   // XR_TIMEOUT_EXPIRED, XR_ERROR_SESSION_LOST
typedef XrResult (XRH_CALL* PFN_xrEnumerateSwapchainFormats)(XrSession, uint32_t, uint32_t*, int64_t*);
typedef XrResult (XRH_CALL* PFN_xrCreateSwapchain)(XrSession, const XrSwapchainCreateInfo*, XrSwapchain*);
typedef XrResult (XRH_CALL* PFN_xrWaitSwapchainImage)(XrSwapchain, const XrSwapchainImageWaitInfo*);
typedef XrResult (XRH_CALL* PFN_xrDestroySession)(XrSession);
typedef XrResult (XRH_CALL* PFN_xrDestroySpace)(XrSpace);

// ---- components (one per runtime / layer / loader entry point) --------------------------------------------------------
enum { kKindRuntime = 0, kKindLayer = 1, kKindLoader = 2 };
const char* const kKindName[3] = { "runtime", "API layer", "loader DLL" };
constexpr int kSlots = 8;
struct Slot {
    std::atomic<void*> realEntry{nullptr};       // what GetProcAddress returned (negotiate fn / loader GIPA): the slot key
    std::atomic<void*> gipa{nullptr};            // the real xrGetInstanceProcAddr of this component
    std::atomic<void*> release{nullptr}, acquire{nullptr}, enumerate{nullptr}, destroy{nullptr};
    std::atomic<void*> locate{nullptr}, endFrame{nullptr}, createRefSpace{nullptr};   // v0.10.3 phase 28
    std::atomic<void*> locateSpace{nullptr};                                          // phase 28b (diagnostics)
    std::atomic<void*> destroySession{nullptr};                                       // phase 29 (the overlay's objects)
    std::atomic<void*> instance{nullptr};        // phase 29: the last non-null XrInstance this component's GIPA was asked with
    int  kind = 0;
    char module[64] = "";
};
Slot              s_slot[kSlots];
std::atomic<int>  s_slotN{0};
std::mutex        s_slotMx;                      // slot creation only (GetProcAddress of the 3 names; rare)

// ---- the swapchain -> D3D11 image map (outermost calls only = the game's swapchains) ----------------------------------
constexpr int kSc = 16, kImg = 8, kQ = 4;
struct ScEntry {
    void*    sc = nullptr;
    uint32_t n = 0;
    void*    img[kImg] = {};
    uint32_t q[kQ] = {};                         // acquired, not yet released indices (FIFO: release = the oldest)
    int      qh = 0, qn = 0;
};
ScEntry  s_sc[kSc];
SRWLOCK  s_scLock = SRWLOCK_INIT;

// ---- v0.10.3 phase 28: the game's located views (ring) + its last submitted projection layout ------------------------------
// v0.10.3 phase 28b (ATS VR test 2026-10-11 09:29, Virtual Desktop runtime + its oculus-compatibility layer: "it rotates opposite
// the player's facing direction"; the submitted view-0 pose equalled a located one in only 1736 of 11185 frames, the "lag" flipped
// 0 / 1, the first stereo locate was in a VIEW space while the projection layer is in a LOCAL space): the compositor shows each
// picture with the SUBMITTED pose (xrEndFrame, in the layer's space), not with whatever the game located. So the box is now
// projected with the located pose CORRECTED to the submitted one: at every xrEndFrame, per view and per locate space,
//   C = S (x) L^-1        (S = the submitted pose, L = that space's newest locate at the image release = the one the box used)
// and LatestViews hands out C (x) L_newest -- exact when the game submits a fixed transform of what it located (C constant: a
// copy = identity, a seat offset, another space), one frame of head motion off when it does not (the residual of the previous
// C on this frame's L against the actual S is measured at every xrEndFrame: PoseStats). The fov is the submitted one.
constexpr int kRing = 8;
struct LocRec {
    uint64_t seq = 0;                            // 0 = empty
    View     v[2] = {};
    XrSpace  space = nullptr;
    uint64_t flags = 0;
    LONGLONG qpc = 0;                            // QueryPerformanceCounter when the locate returned
};
LocRec   s_loc[kRing];                           // s_loc[seq % kRing]
uint64_t s_locSeq = 0;                           // the newest seq (0 = none yet)
struct Pose { double q[4]; double p[3]; };       // q = (x, y, z, w)
constexpr int kLocSpaces = 4;
struct LocSpace {                                // phase 28b: one per XrSpace the game locates its views in
    XrSpace  space = nullptr;
    int      type = 0;
    uint64_t count = 0;                          // stereo locates in it
    uint64_t newest = 0;                         // its newest locate seq
    uint64_t atRelease = 0;                      // ... at the last outermost xrReleaseSwapchainImage
    bool     haveC = false;
    Pose     c[2] = {};                          // the correction per view, S (x) L^-1
};
LocSpace s_lsp[kLocSpaces];
struct LayoutRec { XrSwapchain sc = nullptr; int32_t rect[4] = {}; uint32_t slice = 0; };
LayoutRec s_layout[2];                           // per view, from the last xrEndFrame's first projection layer
float    s_subFov[2][4] = {};                    // the submitted fov per view (L, R, U, D)
XrSpace  s_layoutSpace = nullptr;
bool     s_haveLayout = false;
constexpr int kSpaces = 16;
struct SpaceRec { XrSpace space = nullptr; int type = 0; };
SpaceRec s_spaces[kSpaces];                      // xrCreateReferenceSpace results (outermost = the game's)
int      s_spaceNext = 0;
double   s_resAngSum = 0.0, s_resPosSum = 0.0, s_resAngMax = 0.0;   // residual of the previous C (deg / mm), see above
uint64_t s_resN = 0;
LONGLONG s_diagUntil = 0, s_diagNext = 0;        // phase 28b: the once-a-second pose log window (PoseDiagArm)
SRWLOCK  s_poseLock = SRWLOCK_INIT;              // all of the above (short sections: copies only)
std::atomic<uint64_t> s_nLocates{0}, s_nEndFrames{0}, s_nMatched{0}, s_nUnmatched{0}, s_nLocateSpace{0};

int SpaceTypeLocked(XrSpace sp) {                // under s_poseLock
    for (const SpaceRec& r : s_spaces) if (r.space && r.space == sp) return r.type;
    return 0;
}
const char* SpaceTypeName(int t) {
    return t == 2 ? "LOCAL" : t == 3 ? "STAGE" : t == 1 ? "VIEW" : t == 1000426000 ? "LOCAL_FLOOR" : t ? "other" : "unknown";
}
// ---- phase 28b: pose maths (unit quaternions (x, y, z, w); a pose maps a point x of its own space to R x + p) ----------
void QMul(const double a[4], const double b[4], double o[4]) {
    const double x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    const double y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    const double z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    const double w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
void QRot(const double q[4], const double v[3], double o[3]) {     // R(q) v
    const double t[3] = { 2.0 * (q[1] * v[2] - q[2] * v[1]), 2.0 * (q[2] * v[0] - q[0] * v[2]), 2.0 * (q[0] * v[1] - q[1] * v[0]) };
    o[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    o[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    o[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}
void QNorm(double q[4]) {
    const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 1e-9) for (int i = 0; i < 4; ++i) q[i] /= n;
}
Pose PoseOf(const View& v) { return Pose{ { v.qx, v.qy, v.qz, v.qw }, { v.px, v.py, v.pz } }; }
Pose PoseOf(const XrPosef& v) {
    return Pose{ { v.orientation.x, v.orientation.y, v.orientation.z, v.orientation.w }, { v.position.x, v.position.y, v.position.z } };
}
Pose Compose(const Pose& a, const Pose& b) {     // a (x) b: first b, then a
    Pose o;
    QMul(a.q, b.q, o.q);
    QNorm(o.q);
    QRot(a.q, b.p, o.p);
    for (int i = 0; i < 3; ++i) o.p[i] += a.p[i];
    return o;
}
Pose Inverse(const Pose& a) {
    Pose o{ { -a.q[0], -a.q[1], -a.q[2], a.q[3] }, {} };
    QRot(o.q, a.p, o.p);
    for (int i = 0; i < 3; ++i) o.p[i] = -o.p[i];
    return o;
}
// yaw (+ = right) / pitch (+ = up) in degrees of the pose's forward (-z); and the angle between two orientations (deg)
void YawPitch(const double q[4], double* yaw, double* pitch) {
    const double f0[3] = { 0.0, 0.0, -1.0 };
    double f[3];
    QRot(q, f0, f);
    *yaw = std::atan2(f[0], -f[2]) * 57.29577951308232;
    *pitch = std::asin(f[1] < -1.0 ? -1.0 : (f[1] > 1.0 ? 1.0 : f[1])) * 57.29577951308232;
}
double AngleBetween(const double a[4], const double b[4]) {
    double d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    if (d > 1.0) d = 1.0;
    return 2.0 * std::acos(d) * 57.29577951308232;
}

std::atomic<ReleaseFn> s_cb{nullptr};
std::atomic<bool>      s_hooked{false};
std::atomic<uint64_t>  s_releases{0}, s_releasesMapped{0};
thread_local int       t_depth = 0;              // > 0 inside one of our wrapped calls (an inner layer's call: pass through)

typedef FARPROC (WINAPI* PFN_GetProcAddress)(HMODULE, LPCSTR);
PFN_GetProcAddress oGetProcAddressK32 = nullptr; // kernel32!GetProcAddress (trampoline)
PFN_GetProcAddress oGetProcAddressKb = nullptr;  // kernelbase!GetProcAddress (trampoline)

ScEntry* FindSc(void* sc) {
    for (ScEntry& e : s_sc) if (e.sc == sc) return &e;
    return nullptr;
}

// ---- v0.10.3 phase 29: the overlay quads (see xr_hook.h) ------------------------------------------------------------------
// Our objects belong to ONE session (the one at the game's xrEndFrame): two swapchains (0 = the panel, 1 = the fps box) and a
// VIEW reference space (identity pose) for the head-locked quads, made at the first xrEndFrame that wants a quad, kept for the
// session, dropped at its xrDestroySession (or forgotten when another session ends a frame: they died with the old one). The
// OpenXR functions are resolved through the real xrGetInstanceProcAddr of the component whose thunk handles the game's
// outermost xrEndFrame, with the XrInstance its GIPA was last asked with (the loader's dispatch starts there).
struct OvFns {
    PFN_xrEnumerateSwapchainFormats enumFormats = nullptr;
    PFN_xrCreateSwapchain           createSc = nullptr;
    PFN_xrDestroySwapchain          destroySc = nullptr;
    PFN_xrEnumerateSwapchainImages  enumImages = nullptr;
    PFN_xrAcquireSwapchainImage     acquire = nullptr;
    PFN_xrWaitSwapchainImage        wait = nullptr;
    PFN_xrReleaseSwapchainImage     release = nullptr;
    PFN_xrCreateReferenceSpace      createSpace = nullptr;
    PFN_xrDestroySpace              destroySpace = nullptr;
};
struct OvSc { XrSwapchain sc = nullptr; uint32_t w = 0, h = 0, n = 0; void* img[kImg] = {}; };
struct OvState {
    int       slot = -1;                         // the component the functions were resolved at
    bool      resolved = false;
    int       resolveTries = 0;
    OvFns     fn;
    XrSession ses = nullptr;                     // the session our objects belong to
    bool      ready = false;                     // swapchains + VIEW space made for `ses`
    bool      dead = false;                      // given up for `ses` (create failed / layers rejected / an image we cannot release)
    int64_t   format = 0;
    OvSc      sc[2];                             // 0 panel, 1 fps box
    XrSpace   viewSpace = nullptr;
    bool      have[2] = {};                      // quad drawn (and released) for the frame that ends at the next xrEndFrame
    uint32_t  usedW[2] = {}, usedH[2] = {};
    uint64_t  endSeq = 0, relSeq = ~0ull;        // xrEndFrame count; the interval the release draw (mode 2) ran in
    int       mode = 0;                          // 1 drawn at xrEndFrame, 2 at the game's first release of the frame
    int       acqRun = 0;                        // frames in a row with an acquire failure
    int       lastLock = 0;                      // the fps box fixed in the cab: params.fpsLock at the last xrEndFrame
    XrSpace   pivotSpace = nullptr;              // ... the layer space the pivot was captured in
    bool      pivotHave = false;
    double    pivot[3] = {}, yaw0 = 0.0;         // ... non-LOCAL layer spaces: the eyes' midpoint + head yaw at lock-on (rad)
    bool      zChecked[2] = {}, zFlip[2] = {};   // the quad's +z verified to point back (QRot; OvFacingQuat): 0 the fps box fixed
                                                 //   in the cab (at the pivot), 1 the quads facing the head (phase 29b)
    bool      firstLog[3] = {};                  // first panel / follow-head / fixed fps quad logged
    bool      fovBadLogged = false, capLogged = false;
};
OvState                    s_ov;
std::mutex                 s_ovMx;               // s_ov (xrEndFrame, the release draw, xrDestroySession); never held over the real xrEndFrame
OverlayParams              s_ovPar;
SRWLOCK                    s_ovParLock = SRWLOCK_INIT;
std::atomic<OverlayDrawFn> s_ovCb{nullptr};
std::atomic<bool>          s_ovOk[2] = {};       // OverlayActive (+ s_ovOkQpc: the last xrEndFrame that set them)
std::atomic<LONGLONG>      s_ovOkQpc{0};
std::atomic<uint64_t>      s_ovFrames{0}, s_ovPanel{0}, s_ovFps{0}, s_ovCreateFails{0}, s_ovAcqFails{0}, s_ovWaitFails{0},
                           s_ovRelFails{0}, s_ovDrawFails{0}, s_ovRejects{0};
std::atomic<int>           s_ovMode{0}, s_ovFixedHow{0}, s_ovFixedType{0};
std::atomic<int64_t>       s_ovFormat{0};
std::atomic<bool>          s_ovReady{false};
constexpr int              kOvMaxLayers = 16;    // the game's layers + ours (more: no quads this frame)

OverlayParams OvParams() {
    AcquireSRWLockShared(&s_ovParLock);
    const OverlayParams p = s_ovPar;
    ReleaseSRWLockShared(&s_ovParLock);
    return p;
}
const char* OvFormatName(int64_t f) {
    return f == 29 ? "R8G8B8A8_UNORM_SRGB" : f == 91 ? "B8G8R8A8_UNORM_SRGB" : f == 28 ? "R8G8B8A8_UNORM" :
           f == 87 ? "B8G8R8A8_UNORM" : "?";
}
void OvSetOk(bool panel, bool fps) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    s_ovOk[0].store(panel, std::memory_order_relaxed);
    s_ovOk[1].store(fps, std::memory_order_relaxed);
    s_ovOkQpc.store(now.QuadPart, std::memory_order_release);
}
// under s_ovMx: the functions through component i's real GIPA
void OvResolveLocked(int i) {
    s_ov.slot = i;
    s_ov.resolved = false;
    ++s_ov.resolveTries;
    auto gipa = reinterpret_cast<PFN_xrGetInstanceProcAddr>(s_slot[i].gipa.load(std::memory_order_acquire));
    XrInstance inst = s_slot[i].instance.load(std::memory_order_acquire);
    if (!gipa || !inst) {
        if (s_ov.resolveTries <= 3)
            Log("OpenXR overlay: cannot resolve the swapchain functions yet at the %s %s (GIPA %p, instance %p) -- the VR panel "
                "stays in the eye picture", kKindName[s_slot[i].kind], s_slot[i].module, reinterpret_cast<void*>(gipa), inst);
        return;
    }
    static const char* const kNames[9] = { "xrEnumerateSwapchainFormats", "xrCreateSwapchain", "xrDestroySwapchain",
                                           "xrEnumerateSwapchainImages", "xrAcquireSwapchainImage", "xrWaitSwapchainImage",
                                           "xrReleaseSwapchainImage", "xrCreateReferenceSpace", "xrDestroySpace" };
    void* f[9] = {};
    int missing = -1;
    for (int k = 0; k < 9; ++k) {
        PFN_xrVoidFunction fp = nullptr;
        ++t_depth;
        const XrResult r = gipa(inst, kNames[k], &fp);
        --t_depth;
        f[k] = r >= 0 ? reinterpret_cast<void*>(fp) : nullptr;
        if (!f[k] && missing < 0) missing = k;
    }
    if (missing >= 0) {
        if (s_ov.resolveTries <= 3)
            Log("OpenXR overlay: %s not resolved at the %s %s (instance %p) -- the VR panel stays in the eye picture",
                kNames[missing], kKindName[s_slot[i].kind], s_slot[i].module, inst);
        return;
    }
    OvFns& o = s_ov.fn;
    o.enumFormats  = reinterpret_cast<PFN_xrEnumerateSwapchainFormats>(f[0]);
    o.createSc     = reinterpret_cast<PFN_xrCreateSwapchain>(f[1]);
    o.destroySc    = reinterpret_cast<PFN_xrDestroySwapchain>(f[2]);
    o.enumImages   = reinterpret_cast<PFN_xrEnumerateSwapchainImages>(f[3]);
    o.acquire      = reinterpret_cast<PFN_xrAcquireSwapchainImage>(f[4]);
    o.wait         = reinterpret_cast<PFN_xrWaitSwapchainImage>(f[5]);
    o.release      = reinterpret_cast<PFN_xrReleaseSwapchainImage>(f[6]);
    o.createSpace  = reinterpret_cast<PFN_xrCreateReferenceSpace>(f[7]);
    o.destroySpace = reinterpret_cast<PFN_xrDestroySpace>(f[8]);
    s_ov.resolved = true;
    Log("OpenXR overlay: swapchain / space functions resolved at the %s %s (slot %d = the game's outermost xrEndFrame, "
        "instance %p)", kKindName[s_slot[i].kind], s_slot[i].module, i, inst);
}
// under s_ovMx: our swapchains + space go (destroy = through the runtime; false = they died with the session already)
void OvDropLocked(bool destroy) {
    const OvFns& o = s_ov.fn;
    for (OvSc& c : s_ov.sc) {
        if (destroy && c.sc && o.destroySc) { ++t_depth; o.destroySc(c.sc); --t_depth; }
        c = OvSc();
    }
    if (destroy && s_ov.viewSpace && o.destroySpace) { ++t_depth; o.destroySpace(s_ov.viewSpace); --t_depth; }
    s_ov.viewSpace = nullptr;
    s_ov.ready = false;
    s_ov.have[0] = s_ov.have[1] = false;
    s_ov.pivotHave = false;
    s_ov.pivotSpace = nullptr;
    s_ovReady.store(false, std::memory_order_relaxed);
}
// under s_ovMx: the two swapchains (+ their D3D11 images) and the VIEW space for session `ses`. False = failed (logged; the
// session is given up: dead).
bool OvCreateLocked(XrSession ses, const OverlayParams& par) {
    const OvFns& o = s_ov.fn;
    auto fail = [&](const char* what, XrResult r) {
        s_ovCreateFails.fetch_add(1, std::memory_order_relaxed);
        Log("OpenXR overlay: %s failed (result %d) -- the VR panel / fps box stay in the eye picture for this session", what, r);
        OvDropLocked(true);
        s_ov.dead = true;
        return false;
    };
    // the format: an _SRGB one first (the compositor reads a non-sRGB 8-bit image as LINEAR: the dark panel would band)
    int64_t fmts[64] = {};
    uint32_t nf = 0;
    ++t_depth;
    XrResult r = o.enumFormats(ses, 0, &nf, nullptr);
    if (r >= 0) { if (nf > 64) nf = 64; r = o.enumFormats(ses, nf, &nf, fmts); }
    --t_depth;
    if (r < 0) return fail("xrEnumerateSwapchainFormats", r);
    if (nf > 64) nf = 64;
    static const int64_t kPref[4] = { 29, 91, 28, 87 };   // R8G8B8A8_UNORM_SRGB, B8G8R8A8_UNORM_SRGB, R8G8B8A8_UNORM, B8G8R8A8_UNORM
    int64_t pick = 0;
    for (int64_t want : kPref) {
        for (uint32_t k = 0; k < nf && !pick; ++k) if (fmts[k] == want) pick = want;
        if (pick) break;
    }
    char list[256] = "";
    for (uint32_t k = 0, at = 0; k < nf && at < sizeof(list) - 24; ++k)
        at += (uint32_t)snprintf(list + at, sizeof(list) - at, "%s%lld", k ? " " : "", (long long)fmts[k]);
    if (!pick) {
        Log("OpenXR overlay: the runtime offers none of the 8-bit RGBA swapchain formats (offered: %s)", list);
        return fail("picking a swapchain format", 0);
    }
    const uint32_t sw[2] = { par.panelW, par.fpsW }, sh[2] = { par.panelH, par.fpsH };
    for (int k = 0; k < 2; ++k) {
        OvSc& c = s_ov.sc[k];
        if (sw[k] < 16 || sh[k] < 16 || sw[k] > 4096 || sh[k] > 4096) return fail("the overlay image size check", 0);
        XrSwapchainCreateInfo ci{};
        ci.type = kTypeSwapchainCreateInfo;
        ci.usageFlags = kUsageColorAttachment | kUsageSampled;
        ci.format = pick;
        ci.sampleCount = 1; ci.width = sw[k]; ci.height = sh[k]; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
        ++t_depth;
        r = o.createSc(ses, &ci, &c.sc);
        --t_depth;
        if (r < 0 || !c.sc) { c.sc = nullptr; return fail(k ? "xrCreateSwapchain (fps box)" : "xrCreateSwapchain (panel)", r); }
        c.w = sw[k]; c.h = sh[k];
        uint32_t ni = 0;
        XrSwapchainImageD3D11 im[kImg] = {};
        for (XrSwapchainImageD3D11& e : im) e.type = kTypeSwapchainImageD3D11;
        ++t_depth;
        r = o.enumImages(c.sc, 0, &ni, nullptr);
        if (r >= 0 && ni >= 1 && ni <= (uint32_t)kImg) r = o.enumImages(c.sc, ni, &ni, im);
        --t_depth;
        if (r < 0 || ni < 1 || ni > (uint32_t)kImg || !im[0].texture)
            return fail("xrEnumerateSwapchainImages (D3D11) of our swapchain", r < 0 ? r : (XrResult)ni);
        c.n = ni;
        for (uint32_t m = 0; m < ni; ++m) c.img[m] = im[m].texture;
    }
    XrReferenceSpaceCreateInfo si{};
    si.type = kTypeReferenceSpaceCreateInfo;
    si.referenceSpaceType = kRefSpaceView;
    si.poseInReferenceSpace.orientation.w = 1.0f;
    ++t_depth;
    r = o.createSpace(ses, &si, &s_ov.viewSpace);
    --t_depth;
    if (r < 0 || !s_ov.viewSpace) { s_ov.viewSpace = nullptr; return fail("xrCreateReferenceSpace (VIEW)", r); }
    s_ov.format = pick;
    s_ov.ready = true;
    s_ovFormat.store(pick, std::memory_order_relaxed);
    s_ovReady.store(true, std::memory_order_relaxed);
    Log("OpenXR overlay: session %p -- panel swapchain %p %ux%u (%u images, first %p), fps box swapchain %p %ux%u (%u images), "
        "format %lld = %s (offered: %s), our VIEW space %p; the VR panel / fps box are quad layers from now on (tid=%lu)", ses,
        s_ov.sc[0].sc, s_ov.sc[0].w, s_ov.sc[0].h, s_ov.sc[0].n, s_ov.sc[0].img[0], s_ov.sc[1].sc, s_ov.sc[1].w, s_ov.sc[1].h,
        s_ov.sc[1].n, (long long)pick, OvFormatName(pick), list, s_ov.viewSpace, GetCurrentThreadId());
    return true;
}
// under s_ovMx, t_depth > 0: one image of quad `which`: acquire, wait, the draw callback, release. True = drawn + released
// (s_ov.have / usedW / usedH set).
bool OvDrawLocked(int which) {
    const OvFns& o = s_ov.fn;
    OvSc& c = s_ov.sc[which];
    uint32_t idx = 0;
    const XrSwapchainImageAcquireInfo ai{ kTypeSwapchainImageAcquireInfo, nullptr };
    XrResult r = o.acquire(c.sc, &ai, &idx);
    if (r < 0) {
        const uint64_t n = s_ovAcqFails.fetch_add(1, std::memory_order_relaxed);
        if (n < 4) Log("OpenXR overlay: xrAcquireSwapchainImage of our %s swapchain failed (result %d)", which ? "fps box" : "panel", r);
        if (r == kErrSessionLost) s_ov.dead = true;
        return false;
    }
    const XrSwapchainImageWaitInfo wi{ kTypeSwapchainImageWaitInfo, nullptr, kInfiniteDuration };
    r = o.wait(c.sc, &wi);
    if (r < 0 || r == kTimeoutExpired) {                 // an acquired image we may not release: the session is given up
        s_ovWaitFails.fetch_add(1, std::memory_order_relaxed);
        Log("OpenXR overlay: xrWaitSwapchainImage of our %s swapchain failed (result %d) -- the quad path is off for this session",
            which ? "fps box" : "panel", r);
        s_ov.dead = true;
        return false;
    }
    OverlayDraw d;
    d.which = which;
    d.tex = idx < c.n ? c.img[idx] : nullptr;
    d.w = c.w; d.h = c.h;
    d.format = s_ov.format;
    bool drew = false;
    if (d.tex)
        if (OverlayDrawFn cb = s_ovCb.load(std::memory_order_acquire)) drew = cb(&d);
    if (drew) {
        if (d.usedW > c.w) d.usedW = c.w;
        if (d.usedH > c.h) d.usedH = c.h;
        if (d.usedW < 4 || d.usedH < 4) drew = false;
    }
    if (!drew) s_ovDrawFails.fetch_add(1, std::memory_order_relaxed);
    const XrSwapchainImageReleaseInfo ri{ kTypeSwapchainImageReleaseInfo, nullptr };
    r = o.release(c.sc, &ri);
    if (r < 0) {
        s_ovRelFails.fetch_add(1, std::memory_order_relaxed);
        Log("OpenXR overlay: xrReleaseSwapchainImage of our %s swapchain failed (result %d) -- the quad path is off for this session",
            which ? "fps box" : "panel", r);
        s_ov.dead = true;
        return false;
    }
    if (!drew) return false;
    s_ov.have[which] = true;
    s_ov.usedW[which] = d.usedW;
    s_ov.usedH[which] = d.usedH;
    return true;
}
// under s_ovMx: the wanted quads drawn now (both paths); 60 frames in a row with an acquire failure give up the session
void OvDrawWantedLocked(const OverlayParams& par) {
    const uint64_t a0 = s_ovAcqFails.load(std::memory_order_relaxed);
    ++t_depth;
    if (par.menuOpen && !s_ov.dead) OvDrawLocked(0);
    if (par.fpsShow && !s_ov.dead) OvDrawLocked(1);
    --t_depth;
    const bool acqFail = s_ovAcqFails.load(std::memory_order_relaxed) != a0;
    s_ov.acqRun = acqFail ? s_ov.acqRun + 1 : 0;
    if (s_ov.acqRun >= 60 && !s_ov.dead) {
        s_ov.dead = true;
        Log("OpenXR overlay: 60 frames in a row without an image of our swapchains -- the quad path is off for this session");
    }
}
// rotation of `ang` rad about the +y / +x axis as a quaternion (x, y, z, w)
void QAxisY(double ang, double q[4]) { q[0] = 0.0; q[1] = std::sin(0.5 * ang); q[2] = 0.0; q[3] = std::cos(0.5 * ang); }
void QAxisX(double ang, double q[4]) { q[0] = std::sin(0.5 * ang); q[1] = 0.0; q[2] = 0.0; q[3] = std::cos(0.5 * ang); }
// v0.10.3 phase 29b (factored out of the phase 29 fixed-in-cab box): the orientation that turns a quad to FACE back along `dir`
// (unit, from the point it faces -- the pivot / the VIEW-space origin = the head -- to the quad centre). The quad is seen from
// its +z side: R(q) (0,0,1) must be -dir, +y up (no roll): yaw = atan2(dir.x, -dir.z), pitch = asin(dir.y), q = rot_y(-yaw) (x)
// rot_x(pitch). Verified numerically with QRot once per kind `k` (0 the fps box fixed in the cab, 1 the quads facing the head)
// at its first use off the axis (> 0.5 deg; on the axis q is the identity) and logged; a negative result turns it 180 deg about
// its y axis from then on (a sign slip would show the back side). `what` / `to` name the quad and the point (log). Under s_ovMx.
void OvFacingQuat(int k, const char* what, const char* to, const double dir[3], double rq[4]) {
    const double kRad = 0.017453292519943295;
    const double yaw = std::atan2(dir[0], -dir[2]);
    const double pitch = std::asin(dir[1] < -1.0 ? -1.0 : (dir[1] > 1.0 ? 1.0 : dir[1]));
    double qy[4], qx[4];
    QAxisY(-yaw, qy);
    QAxisX(pitch, qx);
    QMul(qy, qx, rq);
    QNorm(rq);
    if (!s_ov.zChecked[k] && (std::fabs(yaw) > 0.5 * kRad || std::fabs(pitch) > 0.5 * kRad)) {
        const double z0[3] = { 0.0, 0.0, 1.0 }, y0[3] = { 0.0, 1.0, 0.0 };
        double zr[3], yr[3];
        QRot(rq, z0, zr);
        QRot(rq, y0, yr);
        const double dot = -(zr[0] * dir[0] + zr[1] * dir[1] + zr[2] * dir[2]);
        s_ov.zChecked[k] = true;
        s_ov.zFlip[k] = dot < 0.0;
        Log("OpenXR overlay: %s quad orientation check: yaw %+.1f pitch %+.1f deg, R(q)(0,0,1) = (%+.3f %+.3f %+.3f) vs back-to-%s "
            "(%+.3f %+.3f %+.3f): dot %+.3f -> %s; R(q)(0,1,0) = (%+.3f %+.3f %+.3f)", what, yaw / kRad, pitch / kRad, zr[0], zr[1],
            zr[2], to, -dir[0], -dir[1], -dir[2], dot,
            dot > 0.999 ? "OK (faces it)" : (dot < 0.0 ? "POINTS AWAY: turned 180 deg about its y axis from now on" : "NOT exact (kept)"),
            yr[0], yr[1], yr[2]);
    }
    if (s_ov.zFlip[k]) {
        double f180[4], t[4];
        QAxisY(3.141592653589793, f180);
        QMul(rq, f180, t);
        memcpy(rq, t, sizeof(t));
        QNorm(rq);
    }
}
// under s_ovMx: the quad layer of `which` from the frame's projection layer `proj` (its space, view 0's submitted fov, the
// submitted poses for the pivot). False = not placeable (bad fov).
bool OvBuildQuadLocked(int which, const OverlayParams& par, const XrCompositionLayerProjection* proj, XrCompositionLayerQuad& q) {
    const XrFovf& f = proj->views[0].fov;
    const double tl = std::tan((double)f.angleLeft), tr = std::tan((double)f.angleRight);
    const double tu = std::tan((double)f.angleUp), tdn = std::tan((double)f.angleDown);
    const double fw = tr - tl, fh = tu - tdn;
    if (!(fw > 0.1 && fw < 20.0) || !(fh > 0.1 && fh < 20.0)) {
        if (!s_ov.fovBadLogged) {
            s_ov.fovBadLogged = true;
            Log("OpenXR overlay: the submitted view 0 fov (L%+.1f R%+.1f U%+.1f D%+.1f deg) is not usable -- no quads",
                f.angleLeft * 57.29578, f.angleRight * 57.29578, f.angleUp * 57.29578, f.angleDown * 57.29578);
        }
        return false;
    }
    q = XrCompositionLayerQuad{};
    q.type = kTypeCompositionLayerQuad;
    q.layerFlags = kLayerBlendSrcAlpha | kLayerUnpremultiplied;
    q.eyeVisibility = kEyeVisibilityBoth;
    q.subImage.swapchain = s_ov.sc[which].sc;
    q.subImage.x = 0; q.subImage.y = 0;
    q.subImage.w = (int32_t)s_ov.usedW[which]; q.subImage.h = (int32_t)s_ov.usedH[which];
    q.subImage.imageArrayIndex = 0;
    const double aspect = (double)s_ov.usedH[which] / (double)s_ov.usedW[which];
    const double kRad = 0.017453292519943295;
    double pos[3] = { 0.0, 0.0, 0.0 }, rq[4] = { 0.0, 0.0, 0.0, 1.0 }, wM = 0.0;
    int logIdx = which;                                  // 0 panel, 1 fps follows the head, 2 fps fixed in the cab
    int how = 0, lt = 0;
    // v0.10.3 phase 29b, menu_vr_face = 1: a VIEW-space quad at `pos` (d ahead) turned to FACE the head (the VIEW origin), its
    // size times |pos| / d so it keeps the angle it has straight ahead (the phase 16 "same size from every angle"); 0 = parallel
    // to the view plane (identity: a flat picture seen at an angle off the axis, as phase 29)
    auto faceHead = [&](double d, const char* what) {
        const double r = std::sqrt(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]);
        if (!par.menuFace || !(r > 1e-6)) return;
        const double dir[3] = { pos[0] / r, pos[1] / r, pos[2] / r };
        OvFacingQuat(1, what, "head", dir, rq);
        wM *= r / d;
    };
    if (which == 0) {                                    // the panel: VIEW space, menu_vr_dist ahead (phase 29b: was 1 m)
        const double d = par.menuDist >= 0.3f && par.menuDist <= 3.0f ? (double)par.menuDist : 1.0;
        wM = d * fw * 0.54 * (double)par.menuScale;      // (scales with d: the same angle at every distance)
        pos[0] = d * fw * (double)par.menuX / 100.0;
        pos[1] = -d * fh * (double)par.menuY / 100.0;
        pos[2] = -d;
        faceHead(d, "panel facing the head");
        q.space = s_ov.viewSpace;
    } else {
        const double d = 0.8;                            // the fps box: 0.8 m (the phase 28 anchor distance)
        wM = d * fw * 0.08 * (double)par.fpsScale;
        if (par.fpsLock) {
            AcquireSRWLockShared(&s_poseLock);
            lt = SpaceTypeLocked(proj->space);
            ReleaseSRWLockShared(&s_poseLock);
        }
        if (par.fpsLock && lt != kRefSpaceView) {        // fixed in the cab: the game's projection-layer space
            if (!s_ov.lastLock || proj->space != s_ov.pivotSpace) { s_ov.pivotHave = false; s_ov.pivotSpace = proj->space; }
            double piv[3] = { 0.0, 0.0, 0.0 }, yaw0 = 0.0;
            if (lt == kRefSpaceLocal) {
                how = 1;                                 // LOCAL: its origin is the seat, its -z the seat's forward
            } else {
                how = 2;                                 // other / unknown: a pivot from the submitted views, once per lock-on
                if (!s_ov.pivotHave) {
                    const XrPosef& a = proj->views[0].pose;
                    const XrPosef& b = proj->views[1].pose;
                    s_ov.pivot[0] = 0.5 * ((double)a.position.x + (double)b.position.x);
                    s_ov.pivot[1] = 0.5 * ((double)a.position.y + (double)b.position.y);
                    s_ov.pivot[2] = 0.5 * ((double)a.position.z + (double)b.position.z);
                    const Pose pa = PoseOf(a);
                    double yd = 0.0, pd = 0.0;
                    YawPitch(pa.q, &yd, &pd);
                    s_ov.yaw0 = yd * kRad;
                    s_ov.pivotHave = true;
                    Log("OpenXR overlay: fps box fixed in the cab -- layer space %p type %d (%s): the anchor turns around a PIVOT "
                        "captured now from the submitted views: the eyes' midpoint (%+.3f %+.3f %+.3f) m, head yaw %+.1f deg",
                        proj->space, lt, SpaceTypeName(lt), s_ov.pivot[0], s_ov.pivot[1], s_ov.pivot[2], yd);
                }
                piv[0] = s_ov.pivot[0]; piv[1] = s_ov.pivot[1]; piv[2] = s_ov.pivot[2];
                yaw0 = s_ov.yaw0;
            }
            const double yaw = yaw0 + (double)par.fpsX * kRad, pitch = -(double)par.fpsY * kRad;
            const double dir[3] = { std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch) };
            for (int k = 0; k < 3; ++k) pos[k] = piv[k] + d * dir[k];
            OvFacingQuat(0, "fps box fixed-in-cab", "pivot", dir, rq);   // faces the pivot (phase 29b: the shared helper)
            q.space = proj->space;
            logIdx = 2;
        } else {                                         // follows the head (or the layer space is a VIEW space)
            how = par.fpsLock ? 3 : 0;
            pos[0] = d * fw * (double)par.fpsX / 100.0;
            pos[1] = -d * fh * (double)par.fpsY / 100.0;
            pos[2] = -d;
            faceHead(d, "fps box facing the head");     // phase 29b: menu_vr_face, as the panel
            q.space = s_ov.viewSpace;
        }
        if (par.fpsLock) { s_ovFixedHow.store(how, std::memory_order_relaxed); s_ovFixedType.store(lt, std::memory_order_relaxed); }
    }
    q.pose.orientation.x = (float)rq[0]; q.pose.orientation.y = (float)rq[1];
    q.pose.orientation.z = (float)rq[2]; q.pose.orientation.w = (float)rq[3];
    q.pose.position.x = (float)pos[0]; q.pose.position.y = (float)pos[1]; q.pose.position.z = (float)pos[2];
    q.size.width = (float)wM;
    q.size.height = (float)(wM * aspect);
    if (!s_ov.firstLog[logIdx]) {
        s_ov.firstLog[logIdx] = true;
        static const char* const kWhat[3] = { "panel (VIEW space, ours)", "fps box following the head (VIEW space, ours)",
                                              "fps box FIXED IN THE CAB (the game's projection-layer space)" };
        Log("OpenXR overlay: first %s quad -- space %p, pose q=(%+.3f %+.3f %+.3f %+.3f) p=(%+.3f %+.3f %+.3f) m, size %.3f x %.3f m, "
            "sub-image %dx%d of %ux%u px; view 0 fov tangents L%+.3f R%+.3f U%+.3f D%+.3f%s%s", kWhat[logIdx], q.space,
            rq[0], rq[1], rq[2], rq[3], pos[0], pos[1], pos[2], wM, wM * aspect, q.subImage.w, q.subImage.h, s_ov.sc[which].w,
            s_ov.sc[which].h, tl, tr, tu, tdn,
            how == 3 ? " (fixed in the cab asked, but the layer space is a VIEW space: head-locked)" : "",
            logIdx == 2 ? "" : (par.menuFace ? "; menu_vr_face=1: turned to face the head (phase 29b)"
                                             : "; menu_vr_face=0: parallel to the view plane"));
    }
    return true;
}
// The game's outermost xrEndFrame (`proj` = its first stereo projection layer; null = none this frame): the wanted quads
// (drawn here on the render thread, or at the game's first release of the frame -- mode 2) appended after the game's layers
// into outFe / outLayers / quads (the caller's arrays). True = outFe is the frame to submit.
bool OvEndFrame(int i, XrSession ses, const XrFrameEndInfo* fe, const XrCompositionLayerProjection* proj, XrFrameEndInfo* outFe,
                const XrCompositionLayerBaseHeader** outLayers, XrCompositionLayerQuad* quads) {
    const OverlayParams par = OvParams();
    std::lock_guard<std::mutex> lk(s_ovMx);
    struct EndSeq { uint64_t& s; ~EndSeq() { ++s; } } endSeq{ s_ov.endSeq };   // this frame's release-draw interval ends
    bool have[2] = { s_ov.have[0], s_ov.have[1] };
    s_ov.have[0] = s_ov.have[1] = false;
    if (!par.enabled) { OvSetOk(false, false); return false; }
    if (ses != s_ov.ses) {
        if (s_ov.ses)
            Log("OpenXR overlay: xrEndFrame of another session (%p, was %p) -- our swapchains / space of the old one are forgotten "
                "(they died with it)", ses, s_ov.ses);
        OvDropLocked(false);
        s_ov.ses = ses;
        s_ov.dead = false;
        have[0] = have[1] = false;
    }
    if (!proj) return false;                             // no picture this frame: nothing to place the quads against
    const bool want[2] = { par.menuOpen, par.fpsShow };
    if (!s_ov.resolved || s_ov.slot != i) {
        if (s_ov.slot != i) s_ov.resolveTries = 0;
        if (s_ov.resolveTries < 3 && (want[0] || want[1])) OvResolveLocked(i);
    }
    if (!s_ov.resolved || s_ov.dead) { OvSetOk(false, false); return false; }
    if (!s_ov.ready) {
        if (!(want[0] || want[1])) { OvSetOk(false, false); return false; }   // made at the first frame that wants a quad
        if (!OvCreateLocked(ses, par)) { OvSetOk(false, false); return false; }
    }
    // the thread: the draw needs the game's immediate context (the render thread, drawTid)
    const unsigned long tid = GetCurrentThreadId();
    const int mode = (par.drawTid && tid == par.drawTid) ? 1 : 2;
    if (mode != s_ov.mode) {
        static int s_logs = 0;
        if (s_logs++ < 6)
            Log("OpenXR overlay: the game's xrEndFrame runs on tid=%lu, the render thread is tid=%lu -> the quads are drawn %s", tid,
                par.drawTid, mode == 1 ? "AT xrEndFrame (same thread)" : "at the game's first xrReleaseSwapchainImage of each "
                "frame on the render thread (another thread ends the frame; the layers are appended here)");
        s_ov.mode = mode;
        s_ovMode.store(mode, std::memory_order_relaxed);
    }
    if (mode == 1) {
        OvDrawWantedLocked(par);
        have[0] = s_ov.have[0]; have[1] = s_ov.have[1];
        s_ov.have[0] = s_ov.have[1] = false;
    }
    if (s_ov.dead) { OvSetOk(false, false); return false; }
    int nq = 0;
    bool ok[2] = { !want[0], !want[1] };                 // not wanted = fine (nothing to draw into the picture either)
    const int nGame = (int)fe->layerCount;
    if (nGame + 2 > kOvMaxLayers) {
        if (!s_ov.capLogged) {
            s_ov.capLogged = true;
            Log("OpenXR overlay: the game submits %d layers (we append ours up to %d in all) -- no quads in such frames", nGame,
                kOvMaxLayers);
        }
        OvSetOk(ok[0], ok[1]);
        return false;
    }
    // phase 29b: the later layer is drawn on top -- the panel last when it sits nearer than the fps box's 0.8 m (menu_vr_dist)
    const bool panelTop = par.menuDist < 0.8f;
    for (int k = 0; k < 2; ++k) {
        const int w = panelTop ? 1 - k : k;
        if (!want[w] || !have[w]) continue;
        if (!OvBuildQuadLocked(w, par, proj, quads[nq])) continue;
        ++nq;
        ok[w] = true;
        (w ? s_ovFps : s_ovPanel).fetch_add(1, std::memory_order_relaxed);
    }
    s_ov.lastLock = par.fpsLock;
    OvSetOk(ok[0], ok[1]);
    if (!nq) return false;
    for (int k = 0; k < nGame; ++k) outLayers[k] = fe->layers[k];
    for (int k = 0; k < nq; ++k) outLayers[nGame + k] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quads[k]);
    *outFe = *fe;
    outFe->layerCount = (uint32_t)(nGame + nq);
    outFe->layers = outLayers;
    s_ovFrames.fetch_add(1, std::memory_order_relaxed);
    return true;
}
// The runtime refused the frame with our layers (result r): counted, logged, the quad path off for this session.
void OvRejected(XrResult r) {
    std::lock_guard<std::mutex> lk(s_ovMx);
    s_ovRejects.fetch_add(1, std::memory_order_relaxed);
    Log("OpenXR overlay: the runtime REJECTED xrEndFrame with our quad layers (result %d) -- the frame was submitted again without "
        "them; the VR panel / fps box go back into the eye picture for this session", r);
    s_ov.dead = true;
    OvSetOk(false, false);
}
// Mode 2 (another thread ends the frame): the game's outermost xrReleaseSwapchainImage on the render thread draws the wanted
// quads once per frame (the first release after an xrEndFrame); xrEndFrame appends them.
void OvAtRelease() {
    if (s_ovMode.load(std::memory_order_relaxed) != 2) return;
    const OverlayParams par = OvParams();
    if (!par.enabled || !(par.menuOpen || par.fpsShow) || !par.drawTid || GetCurrentThreadId() != par.drawTid) return;
    std::lock_guard<std::mutex> lk(s_ovMx);
    if (!s_ov.ready || s_ov.dead || s_ov.mode != 2 || s_ov.relSeq == s_ov.endSeq) return;
    s_ov.relSeq = s_ov.endSeq;
    OvDrawWantedLocked(par);
    if (s_ov.have[0]) s_ovOk[0].store(true, std::memory_order_relaxed);   // (this frame's in-picture draw follows: skip it)
    if (s_ov.have[1]) s_ovOk[1].store(true, std::memory_order_relaxed);
}

// ---- the per-slot implementations ---------------------------------------------------------------------------------------
XrResult EnumerateImpl(int i, XrSwapchain sc, uint32_t cap, uint32_t* cnt, void* images) {
    auto real = reinterpret_cast<PFN_xrEnumerateSwapchainImages>(s_slot[i].enumerate.load(std::memory_order_acquire));
    if (!real) return -1;                        // XR_ERROR_VALIDATION_FAILURE (never: the thunk is only handed out with it)
    const bool outer = t_depth == 0;
    ++t_depth;
    const XrResult r = real(sc, cap, cnt, images);
    --t_depth;
    if (outer && r >= 0 && cap > 0 && cnt && images) {
        const XrSwapchainImageD3D11* im = static_cast<const XrSwapchainImageD3D11*>(images);
        if (im[0].type == kTypeSwapchainImageD3D11) {
            const uint32_t n = (std::min)((std::min)(*cnt, cap), (uint32_t)kImg);
            AcquireSRWLockExclusive(&s_scLock);
            ScEntry* e = FindSc(sc);
            if (!e) e = FindSc(nullptr);
            if (!e) e = &s_sc[(uintptr_t)sc % kSc]; // table full (swapchains never destroyed through us): overwrite one
            *e = ScEntry();
            e->sc = sc;
            e->n = n;
            for (uint32_t k = 0; k < n; ++k) e->img[k] = im[k].texture;
            ReleaseSRWLockExclusive(&s_scLock);
            static std::atomic<int> s_logs{0};
            if (s_logs.fetch_add(1, std::memory_order_relaxed) < 8)
                Log("OpenXR hook: swapchain %p has %u D3D11 image(s) (first %p) -- mapped for the VR panel's release draw "
                    "(via the %s %s, tid=%lu)", sc, *cnt, im[0].texture, kKindName[s_slot[i].kind], s_slot[i].module,
                    GetCurrentThreadId());
        }
    }
    return r;
}
XrResult AcquireImpl(int i, XrSwapchain sc, const void* info, uint32_t* index) {
    auto real = reinterpret_cast<PFN_xrAcquireSwapchainImage>(s_slot[i].acquire.load(std::memory_order_acquire));
    if (!real) return -1;
    const bool outer = t_depth == 0;
    ++t_depth;
    const XrResult r = real(sc, info, index);
    --t_depth;
    if (outer && r >= 0 && index) {
        AcquireSRWLockExclusive(&s_scLock);
        if (ScEntry* e = FindSc(sc)) {
            if (e->qn == kQ) { e->qh = (e->qh + 1) % kQ; --e->qn; }   // (never: a runtime allows few acquires) drop the oldest
            e->q[(e->qh + e->qn) % kQ] = *index;
            ++e->qn;
        }
        ReleaseSRWLockExclusive(&s_scLock);
    }
    return r;
}
XrResult ReleaseImpl(int i, XrSwapchain sc, const void* info) {
    auto real = reinterpret_cast<PFN_xrReleaseSwapchainImage>(s_slot[i].release.load(std::memory_order_acquire));
    if (!real) return -1;
    if (t_depth == 0) {
        void* tex = nullptr;
        AcquireSRWLockExclusive(&s_scLock);
        if (ScEntry* e = FindSc(sc)) {
            if (e->qn > 0) {
                const uint32_t idx = e->q[e->qh];
                e->qh = (e->qh + 1) % kQ;
                --e->qn;
                if (idx < e->n) tex = e->img[idx];
            }
        }
        ReleaseSRWLockExclusive(&s_scLock);
        AcquireSRWLockExclusive(&s_poseLock);    // v0.10.3 phase 28b: each locate space's newest locate at this release
        for (LocSpace& e : s_lsp) e.atRelease = e.newest;   //   (= the L the box was drawn with: the correction's reference)
        ReleaseSRWLockExclusive(&s_poseLock);
        s_releases.fetch_add(1, std::memory_order_relaxed);
        if (tex) s_releasesMapped.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<bool> s_logged{false};
        if (!s_logged.exchange(true))
            Log("OpenXR hook: first xrReleaseSwapchainImage seen (swapchain %p, image %p%s; via the %s %s, tid=%lu)", sc, tex,
                tex ? "" : " = NOT mapped: no D3D11 xrEnumerateSwapchainImages / xrAcquireSwapchainImage seen for it",
                kKindName[s_slot[i].kind], s_slot[i].module, GetCurrentThreadId());
        OvAtRelease();                           // v0.10.3 phase 29: the quads, when another thread ends the frame (mode 2)
        if (ReleaseFn cb = s_cb.load(std::memory_order_acquire)) {
            ++t_depth;                           // (the callback draws through D3D11 only; an OpenXR call from it would pass)
            cb(sc, tex);
            --t_depth;
        }
    }
    ++t_depth;
    const XrResult r = real(sc, info);
    --t_depth;
    return r;
}
XrResult DestroyImpl(int i, XrSwapchain sc) {
    auto real = reinterpret_cast<PFN_xrDestroySwapchain>(s_slot[i].destroy.load(std::memory_order_acquire));
    if (!real) return -1;
    if (t_depth == 0) {
        AcquireSRWLockExclusive(&s_scLock);
        if (ScEntry* e = FindSc(sc)) *e = ScEntry();
        ReleaseSRWLockExclusive(&s_scLock);
    }
    ++t_depth;
    const XrResult r = real(sc);
    --t_depth;
    return r;
}
// v0.10.3 phase 28: xrLocateViews -- the game's stereo view poses + fovs go into the ring (read only; the call is untouched).
XrResult LocateImpl(int i, XrSession ses, const XrViewLocateInfo* li, XrViewState* vs, uint32_t cap, uint32_t* cnt, XrView* views) {
    auto real = reinterpret_cast<PFN_xrLocateViews>(s_slot[i].locate.load(std::memory_order_acquire));
    if (!real) return -1;
    const bool outer = t_depth == 0;
    ++t_depth;
    const XrResult r = real(ses, li, vs, cap, cnt, views);
    --t_depth;
    if (outer && r >= 0 && li && li->viewConfigurationType == kViewConfigPrimaryStereo && cap >= 2 && cnt && *cnt >= 2 && views) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        AcquireSRWLockExclusive(&s_poseLock);
        const uint64_t seq = ++s_locSeq;
        LocRec& rec = s_loc[seq % kRing];
        rec.seq = seq;
        for (int k = 0; k < 2; ++k) {
            const XrPosef& p = views[k].pose;
            const XrFovf& f = views[k].fov;
            rec.v[k] = View{ p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w, p.position.x, p.position.y,
                             p.position.z, f.angleLeft, f.angleRight, f.angleUp, f.angleDown };
        }
        rec.space = li->space;
        rec.flags = vs ? vs->viewStateFlags : 0;
        rec.qpc = now.QuadPart;
        const int spType = SpaceTypeLocked(li->space);
        // phase 28b: per locate space (a game may locate in VIEW and LOCAL spaces; each gets its own correction)
        LocSpace* ls = nullptr;
        for (LocSpace& e : s_lsp) if (e.space == li->space) { ls = &e; break; }
        bool newSpace = false;
        if (!ls) {
            LocSpace* oldest = &s_lsp[0];
            for (LocSpace& e : s_lsp) if (e.newest < oldest->newest) oldest = &e;   // (an empty slot has newest 0)
            *oldest = LocSpace();
            ls = oldest;
            ls->space = li->space;
            newSpace = true;
        }
        ls->type = spType;
        ++ls->count;
        ls->newest = seq;
        ReleaseSRWLockExclusive(&s_poseLock);
        if (newSpace && s_nLocateSpace.fetch_add(1, std::memory_order_relaxed) < 12)
            Log("OpenXR hook: stereo xrLocateViews in a NEW space %p (%s, type %d), locate #%llu, display time %lld -- view 0 pose "
                "q=(%+.3f %+.3f %+.3f %+.3f) p=(%+.3f %+.3f %+.3f)", li->space, SpaceTypeName(spType), spType,
                (unsigned long long)seq, (long long)li->displayTime, views[0].pose.orientation.x, views[0].pose.orientation.y,
                views[0].pose.orientation.z, views[0].pose.orientation.w, views[0].pose.position.x, views[0].pose.position.y,
                views[0].pose.position.z);
        if (s_nLocates.fetch_add(1, std::memory_order_relaxed) == 0)
            Log("OpenXR hook: first stereo xrLocateViews (space %p = %s, flags 0x%llx, view 0 fov L%+.1f R%+.1f U%+.1f D%+.1f deg, "
                "view 1 fov L%+.1f R%+.1f U%+.1f D%+.1f deg; via the %s %s, tid=%lu) -- kept for the fps box fixed in the cab "
                "(fps_vr_lock)", li->space, spType == 2 ? "LOCAL" : spType == 3 ? "STAGE" : spType == 1 ? "VIEW" :
                spType == 1000426000 ? "LOCAL_FLOOR" : spType ? "other reference space" : "not created through us",
                (unsigned long long)rec.flags, views[0].fov.angleLeft * 57.29578, views[0].fov.angleRight * 57.29578,
                views[0].fov.angleUp * 57.29578, views[0].fov.angleDown * 57.29578, views[1].fov.angleLeft * 57.29578,
                views[1].fov.angleRight * 57.29578, views[1].fov.angleUp * 57.29578, views[1].fov.angleDown * 57.29578,
                kKindName[s_slot[i].kind], s_slot[i].module, GetCurrentThreadId());
    }
    return r;
}
// v0.10.3 phase 28: xrEndFrame -- the first projection layer: its per-view swapchain / rect / slice (the picture layout) and
// the submitted poses + fovs (the per-space correction C = S (x) L^-1, phase 28b). Read only.
XrResult EndFrameImpl(int i, XrSession ses, const XrFrameEndInfo* fe) {
    auto real = reinterpret_cast<PFN_xrEndFrame>(s_slot[i].endFrame.load(std::memory_order_acquire));
    if (!real) return -1;
    const bool outer = t_depth == 0;
    const XrCompositionLayerProjection* proj = nullptr;   // (phase 29: also the overlay's)
    if (outer && fe && fe->layers) {
        for (uint32_t k = 0; k < fe->layerCount && !proj; ++k) {
            const XrCompositionLayerBaseHeader* h = fe->layers[k];
            if (h && h->type == kTypeCompositionLayerProjection) {
                const XrCompositionLayerProjection* p = reinterpret_cast<const XrCompositionLayerProjection*>(h);
                if (p->viewCount >= 2 && p->views) proj = p;
            }
        }
        if (proj) {
            bool first = false;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            AcquireSRWLockExclusive(&s_poseLock);
            first = !s_haveLayout;
            for (int v = 0; v < 2; ++v) {
                const XrSwapchainSubImage& si = proj->views[v].subImage;
                s_layout[v].sc = si.swapchain;
                s_layout[v].rect[0] = si.x; s_layout[v].rect[1] = si.y; s_layout[v].rect[2] = si.w; s_layout[v].rect[3] = si.h;
                s_layout[v].slice = si.imageArrayIndex;
                const XrFovf& f = proj->views[v].fov;
                s_subFov[v][0] = f.angleLeft; s_subFov[v][1] = f.angleRight; s_subFov[v][2] = f.angleUp; s_subFov[v][3] = f.angleDown;
            }
            s_layoutSpace = proj->space;
            s_haveLayout = true;
            const int layerType = SpaceTypeLocked(proj->space);
            // the submitted pose of view 0 against the ring (exact copy?) -- statistics only since phase 28b
            const XrPosef& sp = proj->views[0].pose;
            bool found = false;
            for (const LocRec& rec : s_loc) {
                const View& w = rec.v[0];
                if (rec.seq && w.qx == sp.orientation.x && w.qy == sp.orientation.y && w.qz == sp.orientation.z &&
                    w.qw == sp.orientation.w && w.px == sp.position.x && w.py == sp.position.y && w.pz == sp.position.z)
                    found = true;
            }
            (found ? s_nMatched : s_nUnmatched).fetch_add(1, std::memory_order_relaxed);
            // phase 28b: the correction C = S (x) L^-1 per locate space and view (L = its newest locate at the image release);
            // first the residual of the OLD C of the space LatestViews uses (the one with the newest locate) on this frame
            LocSpace* best = nullptr;
            for (LocSpace& e : s_lsp) if (e.space && e.atRelease && (!best || e.atRelease > best->atRelease)) best = &e;
            const Pose S0 = PoseOf(proj->views[0].pose);
            Pose L0{}, C0{};
            bool diagHave = false;
            for (LocSpace& e : s_lsp) {
                if (!e.space || !e.atRelease) continue;
                const LocRec& rec = s_loc[e.atRelease % kRing];
                if (rec.seq != e.atRelease) continue;           // (overwritten: more than kRing locates since)
                if (&e == best && e.haveC) {
                    const Pose P = Compose(e.c[0], PoseOf(rec.v[0]));
                    const double ang = AngleBetween(P.q, S0.q);
                    const double dx = P.p[0] - S0.p[0], dy = P.p[1] - S0.p[1], dz = P.p[2] - S0.p[2];
                    s_resAngSum += ang;
                    s_resPosSum += std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0;
                    if (ang > s_resAngMax) s_resAngMax = ang;
                    ++s_resN;
                }
                for (int v = 0; v < 2; ++v)
                    e.c[v] = Compose(PoseOf(proj->views[v].pose), Inverse(PoseOf(rec.v[v])));
                e.haveC = true;
                if (&e == best) { L0 = PoseOf(rec.v[0]); C0 = e.c[0]; diagHave = true; }
            }
            const int bestType = best ? best->type : 0;
            const XrSpace bestSpace = best ? best->space : nullptr;
            bool diagNow = false;
            if (diagHave && s_diagUntil && now.QuadPart < s_diagUntil && now.QuadPart >= s_diagNext) {
                LARGE_INTEGER fq;
                QueryPerformanceFrequency(&fq);
                s_diagNext = now.QuadPart + fq.QuadPart;      // once a second
                diagNow = true;
            }
            const LayoutRec l0 = s_layout[0], l1 = s_layout[1];
            const double resA = s_resN ? s_resAngSum / (double)s_resN : 0.0;
            ReleaseSRWLockExclusive(&s_poseLock);
            s_nEndFrames.fetch_add(1, std::memory_order_relaxed);
            if (first)
                Log("OpenXR hook: first xrEndFrame projection layer (space %p = %s): view 0 = swapchain %p rect (%d,%d) %dx%d slice %u, "
                    "view 1 = swapchain %p rect (%d,%d) %dx%d slice %u; submitted pose %s", proj->space, SpaceTypeName(layerType),
                    l0.sc, l0.rect[0], l0.rect[1], l0.rect[2], l0.rect[3], l0.slice, l1.sc, l1.rect[0], l1.rect[1], l1.rect[2],
                    l1.rect[3], l1.slice, found ? "= a located one" : "matches NO located pose (corrected: S (x) L^-1)");
            if (diagNow) {
                double ly, lp, sy, spt, cy, cp;
                YawPitch(L0.q, &ly, &lp);
                YawPitch(S0.q, &sy, &spt);
                YawPitch(C0.q, &cy, &cp);
                const double cAng = AngleBetween(C0.q, Pose{ { 0, 0, 0, 1 }, {} }.q);
                Log("OpenXR hook pose diag: view 0 LOCATED (space %p %s) yaw %+.1f pitch %+.1f pos (%+.3f %+.3f %+.3f) | SUBMITTED "
                    "(layer space %p %s) yaw %+.1f pitch %+.1f pos (%+.3f %+.3f %+.3f) | correction C = S (x) L^-1: rotation %.1f deg "
                    "(yaw %+.1f pitch %+.1f) offset (%+.3f %+.3f %+.3f) m | mean residual of the previous C %.2f deg over %llu frames",
                    bestSpace, SpaceTypeName(bestType), ly, lp, L0.p[0], L0.p[1], L0.p[2], proj->space, SpaceTypeName(layerType), sy,
                    spt, S0.p[0], S0.p[1], S0.p[2], cAng, cy, cp, C0.p[0], C0.p[1], C0.p[2], resA, (unsigned long long)s_resN);
            }
        }
    }
    // v0.10.3 phase 29: our quad layers appended (a copy of the frame info; the game's own stays untouched)
    XrFrameEndInfo ovFe{};
    const XrCompositionLayerBaseHeader* ovLayers[kOvMaxLayers] = {};
    XrCompositionLayerQuad ovQuads[2] = {};
    bool ov = false;
    if (outer && fe) {
        ++t_depth;
        ov = OvEndFrame(i, ses, fe, proj, &ovFe, ovLayers, ovQuads);
        --t_depth;
    }
    ++t_depth;
    XrResult r = real(ses, ov ? &ovFe : fe);
    if (ov && r < 0 && r != kErrSessionLost) {           // refused with our layers: the game's frame again, as it was
        OvRejected(r);
        r = real(ses, fe);
    }
    --t_depth;
    return r;
}
// v0.10.3 phase 29: xrDestroySession -- our swapchains / space of that session go first (through the runtime).
XrResult DestroySessionImpl(int i, XrSession ses) {
    auto real = reinterpret_cast<PFN_xrDestroySession>(s_slot[i].destroySession.load(std::memory_order_acquire));
    if (!real) return -1;
    if (t_depth == 0) {
        std::lock_guard<std::mutex> lk(s_ovMx);
        if (ses && ses == s_ov.ses) {
            const bool had = s_ov.ready;
            OvDropLocked(true);
            s_ov.ses = nullptr;
            s_ov.dead = false;
            Log("OpenXR overlay: xrDestroySession(%p) by the game -- %s", ses,
                had ? "our swapchains + VIEW space destroyed first" : "none of our objects existed");
        }
        OvSetOk(false, false);
    }
    ++t_depth;
    const XrResult r = real(ses);
    --t_depth;
    return r;
}
// v0.10.3 phase 28: xrCreateReferenceSpace -- the type of each space the game creates (LOCAL / STAGE ...; read only).
XrResult CreateRefSpaceImpl(int i, XrSession ses, const XrReferenceSpaceCreateInfo* ci, XrSpace* out) {
    auto real = reinterpret_cast<PFN_xrCreateReferenceSpace>(s_slot[i].createRefSpace.load(std::memory_order_acquire));
    if (!real) return -1;
    const bool outer = t_depth == 0;
    ++t_depth;
    const XrResult r = real(ses, ci, out);
    --t_depth;
    if (outer && r >= 0 && ci && out && *out) {
        AcquireSRWLockExclusive(&s_poseLock);
        SpaceRec* slot = nullptr;
        for (SpaceRec& s : s_spaces) if (s.space == *out) slot = &s;   // a recycled handle value
        if (!slot) { slot = &s_spaces[s_spaceNext]; s_spaceNext = (s_spaceNext + 1) % kSpaces; }
        slot->space = *out;
        slot->type = ci->referenceSpaceType;
        ReleaseSRWLockExclusive(&s_poseLock);
        static std::atomic<int> s_logs{0};
        if (s_logs.fetch_add(1, std::memory_order_relaxed) < 8)
            Log("OpenXR hook: xrCreateReferenceSpace type %d -> space %p (offset pose pos %.2f %.2f %.2f; via the %s %s)",
                ci->referenceSpaceType, *out, ci->poseInReferenceSpace.position.x, ci->poseInReferenceSpace.position.y,
                ci->poseInReferenceSpace.position.z, kKindName[s_slot[i].kind], s_slot[i].module);
    }
    return r;
}
// v0.10.3 phase 28b: xrLocateSpace -- diagnostics only: which space the game locates against which (an engine that locates
// its views in VIEW space usually gets the head pose with xrLocateSpace(view, local)); the first 8 distinct pairs are logged.
XrResult LocateSpaceImpl(int i, XrSpace sp, XrSpace base, XrTime t, XrSpaceLocation* loc) {
    auto real = reinterpret_cast<PFN_xrLocateSpace>(s_slot[i].locateSpace.load(std::memory_order_acquire));
    if (!real) return -1;
    const bool outer = t_depth == 0;
    ++t_depth;
    const XrResult r = real(sp, base, t, loc);
    --t_depth;
    if (outer && r >= 0 && loc) {
        static SRWLOCK s_lk = SRWLOCK_INIT;
        static XrSpace s_pairs[8][2] = {};
        static int s_nPairs = 0;
        bool fresh = false;
        int st = 0, bt = 0;
        AcquireSRWLockExclusive(&s_lk);
        bool seen = false;
        for (int k = 0; k < s_nPairs; ++k) if (s_pairs[k][0] == sp && s_pairs[k][1] == base) seen = true;
        if (!seen && s_nPairs < 8) { s_pairs[s_nPairs][0] = sp; s_pairs[s_nPairs][1] = base; ++s_nPairs; fresh = true; }
        ReleaseSRWLockExclusive(&s_lk);
        if (fresh) {
            AcquireSRWLockShared(&s_poseLock);
            st = SpaceTypeLocked(sp);
            bt = SpaceTypeLocked(base);
            ReleaseSRWLockShared(&s_poseLock);
            Log("OpenXR hook: xrLocateSpace(space %p %s, base %p %s, time %lld) by the game -> flags 0x%llx q=(%+.3f %+.3f %+.3f %+.3f) "
                "p=(%+.3f %+.3f %+.3f) (first call of this pair; tid=%lu)", sp, SpaceTypeName(st), base, SpaceTypeName(bt),
                (long long)t, (unsigned long long)loc->locationFlags, loc->pose.orientation.x, loc->pose.orientation.y,
                loc->pose.orientation.z, loc->pose.orientation.w, loc->pose.position.x, loc->pose.position.y, loc->pose.position.z,
                GetCurrentThreadId());
        }
    }
    return r;
}
XrResult GipaImpl(int i, XrInstance inst, const char* name, PFN_xrVoidFunction* fn);
XrResult NegRuntimeImpl(int i, const XrNegotiateLoaderInfo* li, XrNegotiateRuntimeRequest* rq);
XrResult NegLayerImpl(int i, const XrNegotiateLoaderInfo* li, const char* layerName, XrNegotiateApiLayerRequest* rq);

template <int I> struct Thunk {
    static XrResult XRH_CALL Gipa(XrInstance a, const char* b, PFN_xrVoidFunction* c) { return GipaImpl(I, a, b, c); }
    static XrResult XRH_CALL NegRuntime(const XrNegotiateLoaderInfo* a, XrNegotiateRuntimeRequest* b) {
        return NegRuntimeImpl(I, a, b);
    }
    static XrResult XRH_CALL NegLayer(const XrNegotiateLoaderInfo* a, const char* b, XrNegotiateApiLayerRequest* c) {
        return NegLayerImpl(I, a, b, c);
    }
    static XrResult XRH_CALL Release(XrSwapchain a, const void* b) { return ReleaseImpl(I, a, b); }
    static XrResult XRH_CALL Acquire(XrSwapchain a, const void* b, uint32_t* c) { return AcquireImpl(I, a, b, c); }
    static XrResult XRH_CALL Enumerate(XrSwapchain a, uint32_t b, uint32_t* c, void* d) { return EnumerateImpl(I, a, b, c, d); }
    static XrResult XRH_CALL Destroy(XrSwapchain a) { return DestroyImpl(I, a); }
    static XrResult XRH_CALL Locate(XrSession a, const XrViewLocateInfo* b, XrViewState* c, uint32_t d, uint32_t* e, XrView* f) {
        return LocateImpl(I, a, b, c, d, e, f);           // v0.10.3 phase 28
    }
    static XrResult XRH_CALL EndFrame(XrSession a, const XrFrameEndInfo* b) { return EndFrameImpl(I, a, b); }
    static XrResult XRH_CALL CreateRefSpace(XrSession a, const XrReferenceSpaceCreateInfo* b, XrSpace* c) {
        return CreateRefSpaceImpl(I, a, b, c);
    }
    static XrResult XRH_CALL LocateSpace(XrSpace a, XrSpace b, XrTime c, XrSpaceLocation* d) { return LocateSpaceImpl(I, a, b, c, d); }
    static XrResult XRH_CALL DestroySession(XrSession a) { return DestroySessionImpl(I, a); }   // phase 29
};
struct ThunkSet { void *gipa, *negRuntime, *negLayer, *release, *acquire, *enumerate, *destroy, *locate, *endFrame, *createRefSpace,
                  *locateSpace, *destroySession; };
template <int I> ThunkSet MakeSet() {
    return ThunkSet{ reinterpret_cast<void*>(&Thunk<I>::Gipa), reinterpret_cast<void*>(&Thunk<I>::NegRuntime),
                     reinterpret_cast<void*>(&Thunk<I>::NegLayer), reinterpret_cast<void*>(&Thunk<I>::Release),
                     reinterpret_cast<void*>(&Thunk<I>::Acquire), reinterpret_cast<void*>(&Thunk<I>::Enumerate),
                     reinterpret_cast<void*>(&Thunk<I>::Destroy), reinterpret_cast<void*>(&Thunk<I>::Locate),
                     reinterpret_cast<void*>(&Thunk<I>::EndFrame), reinterpret_cast<void*>(&Thunk<I>::CreateRefSpace),
                     reinterpret_cast<void*>(&Thunk<I>::LocateSpace), reinterpret_cast<void*>(&Thunk<I>::DestroySession) };
}
template <size_t... Is> const ThunkSet* MakeTable(std::index_sequence<Is...>) {
    static const ThunkSet t[] = { MakeSet<(int)Is>()... };
    return t;
}
const ThunkSet* Sets() {
    static const ThunkSet* t = MakeTable(std::make_index_sequence<kSlots>{});
    return t;
}

XrResult GipaImpl(int i, XrInstance inst, const char* name, PFN_xrVoidFunction* fn) {
    auto real = reinterpret_cast<PFN_xrGetInstanceProcAddr>(s_slot[i].gipa.load(std::memory_order_acquire));
    if (!real) return -1;
    const XrResult r = real(inst, name, fn);
    if (inst) s_slot[i].instance.store(inst, std::memory_order_release);   // phase 29: the overlay resolves with it
    if (r < 0 || !fn || !*fn || !name) return r;
    std::atomic<void*>* store = nullptr;
    void* thunk = nullptr;
    const ThunkSet& ts = Sets()[i];
    if      (!strcmp(name, "xrReleaseSwapchainImage"))    { store = &s_slot[i].release;   thunk = ts.release; }
    else if (!strcmp(name, "xrAcquireSwapchainImage"))    { store = &s_slot[i].acquire;   thunk = ts.acquire; }
    else if (!strcmp(name, "xrEnumerateSwapchainImages")) { store = &s_slot[i].enumerate; thunk = ts.enumerate; }
    else if (!strcmp(name, "xrDestroySwapchain"))         { store = &s_slot[i].destroy;   thunk = ts.destroy; }
    else if (!strcmp(name, "xrLocateViews"))              { store = &s_slot[i].locate;    thunk = ts.locate; }     // phase 28
    else if (!strcmp(name, "xrEndFrame"))                 { store = &s_slot[i].endFrame;  thunk = ts.endFrame; }
    else if (!strcmp(name, "xrCreateReferenceSpace"))     { store = &s_slot[i].createRefSpace; thunk = ts.createRefSpace; }
    else if (!strcmp(name, "xrLocateSpace"))              { store = &s_slot[i].locateSpace; thunk = ts.locateSpace; }   // 28b
    else if (!strcmp(name, "xrDestroySession"))           { store = &s_slot[i].destroySession; thunk = ts.destroySession; }   // 29
    else if (!strcmp(name, "xrGetInstanceProcAddr") && reinterpret_cast<void*>(*fn) == s_slot[i].gipa.load()) {
        *fn = reinterpret_cast<PFN_xrVoidFunction>(ts.gipa);   // a GIPA asked for itself: stay wrapped
        return r;
    }
    if (!store) return r;
    void* const got = reinterpret_cast<void*>(*fn);
    if (got == thunk) return r;                  // (already ours)
    store->store(got, std::memory_order_release);
    *fn = reinterpret_cast<PFN_xrVoidFunction>(thunk);
    static std::atomic<int> s_logs{0};
    if (s_logs.fetch_add(1, std::memory_order_relaxed) < 16)
        Log("OpenXR hook: %s wrapped at the %s %s (real %p)", name, kKindName[s_slot[i].kind], s_slot[i].module, got);
    return r;
}
XrResult NegRuntimeImpl(int i, const XrNegotiateLoaderInfo* li, XrNegotiateRuntimeRequest* rq) {
    auto real = reinterpret_cast<PFN_NegRuntime>(s_slot[i].realEntry.load(std::memory_order_acquire));
    if (!real) return -1;
    const XrResult r = real(li, rq);
    if (r >= 0 && rq && rq->structType == kLisRuntimeRequest && rq->structSize >= sizeof(XrNegotiateRuntimeRequest) &&
        rq->getInstanceProcAddr && reinterpret_cast<void*>(rq->getInstanceProcAddr) != Sets()[i].gipa) {
        s_slot[i].gipa.store(reinterpret_cast<void*>(rq->getInstanceProcAddr), std::memory_order_release);
        rq->getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(Sets()[i].gipa);
        Log("OpenXR hook: runtime %s negotiated (interface %u, API 0x%llx) -- its xrGetInstanceProcAddr is wrapped", s_slot[i].module,
            rq->runtimeInterfaceVersion, (unsigned long long)rq->runtimeApiVersion);
    }
    return r;
}
XrResult NegLayerImpl(int i, const XrNegotiateLoaderInfo* li, const char* layerName, XrNegotiateApiLayerRequest* rq) {
    auto real = reinterpret_cast<PFN_NegLayer>(s_slot[i].realEntry.load(std::memory_order_acquire));
    if (!real) return -1;
    const XrResult r = real(li, layerName, rq);
    if (r >= 0 && rq && rq->structType == kLisApiLayerRequest && rq->structSize >= sizeof(XrNegotiateApiLayerRequest) &&
        rq->getInstanceProcAddr && reinterpret_cast<void*>(rq->getInstanceProcAddr) != Sets()[i].gipa) {
        s_slot[i].gipa.store(reinterpret_cast<void*>(rq->getInstanceProcAddr), std::memory_order_release);
        rq->getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(Sets()[i].gipa);
        Log("OpenXR hook: API layer \"%s\" (%s) negotiated -- its xrGetInstanceProcAddr is wrapped", layerName ? layerName : "?",
            s_slot[i].module);
    }
    return r;
}

// The slot for `entry` (what GetProcAddress returned) -- the same entry again gets the same slot; -1 = table full.
int SlotFor(void* entry, int kind, HMODULE mod) {
    std::lock_guard<std::mutex> lk(s_slotMx);
    const int n = s_slotN.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        if (s_slot[i].realEntry.load(std::memory_order_relaxed) == entry) return i;
    if (n >= kSlots) return -1;
    Slot& s = s_slot[n];
    s.kind = kind;
    wchar_t w[MAX_PATH] = L"";
    if (GetModuleFileNameW(mod, w, MAX_PATH)) {
        const wchar_t* b = wcsrchr(w, L'\\');
        WideCharToMultiByte(CP_UTF8, 0, b ? b + 1 : w, -1, s.module, (int)sizeof(s.module) - 1, nullptr, nullptr);
    }
    if (!s.module[0]) strcpy_s(s.module, "?");
    s.realEntry.store(entry, std::memory_order_release);
    if (kind == kKindLoader) s.gipa.store(entry, std::memory_order_release);
    s_slotN.store(n + 1, std::memory_order_release);
    return n;
}

bool IsLoaderModule(HMODULE mod) {
    wchar_t w[MAX_PATH] = L"";
    if (!GetModuleFileNameW(mod, w, MAX_PATH)) return false;
    const wchar_t* b = wcsrchr(w, L'\\');
    return _wcsicmp(b ? b + 1 : w, L"openxr_loader.dll") == 0;
}

bool IsOurThunk(FARPROC p) {
    const void* const v = reinterpret_cast<const void*>(p);
    const ThunkSet* t = Sets();
    for (int i = 0; i < kSlots; ++i)
        if (v == t[i].gipa || v == t[i].negRuntime || v == t[i].negLayer) return true;
    return false;
}
FARPROC Filter(HMODULE mod, LPCSTR name, FARPROC p) {
    // fast path: an ordinal, a failed lookup or any name not starting "xr" (every other caller in the process)
    if (!p || (uintptr_t)name <= 0xFFFF || name[0] != 'x' || name[1] != 'r') return p;
    if (IsOurThunk(p)) return p;                 // kernel32's stub -> kernelbase: both detoured, wrapped once
    int kind = -1;
    if (!strcmp(name, "xrNegotiateLoaderRuntimeInterface")) kind = kKindRuntime;
    else if (!strcmp(name, "xrNegotiateLoaderApiLayerInterface")) kind = kKindLayer;
    else if (!strcmp(name, "xrGetInstanceProcAddr") && IsLoaderModule(mod)) kind = kKindLoader;
    if (kind < 0) return p;
    const int i = SlotFor(reinterpret_cast<void*>(p), kind, mod);
    if (i < 0) {
        static std::atomic<bool> s_full{false};
        if (!s_full.exchange(true))
            Log("OpenXR hook: WARNING more than %d OpenXR components -- %s not wrapped (the VR panel falls back to the arm "
                "point if the release is not seen)", kSlots, name);
        return p;
    }
    const ThunkSet& ts = Sets()[i];
    static std::atomic<int> s_logs{0};
    if (s_logs.fetch_add(1, std::memory_order_relaxed) < 16)
        Log("OpenXR hook: GetProcAddress(%s, %s) -> wrapped (%s, slot %d)", s_slot[i].module, name, kKindName[kind], i);
    return reinterpret_cast<FARPROC>(kind == kKindRuntime ? ts.negRuntime : kind == kKindLayer ? ts.negLayer : ts.gipa);
}
// kernel32!GetProcAddress (what the game exe imports) is a stub that may go to kernelbase!GetProcAddress or straight to
// kernelbase!GetProcAddressForCaller depending on the Windows build -- both exports are detoured (Filter skips a pointer
// that is already one of our thunks, so a stub -> kernelbase chain wraps once).
FARPROC WINAPI hkGetProcAddressK32(HMODULE mod, LPCSTR name) { return Filter(mod, name, oGetProcAddressK32(mod, name)); }
FARPROC WINAPI hkGetProcAddressKb(HMODULE mod, LPCSTR name) { return Filter(mod, name, oGetProcAddressKb(mod, name)); }

bool HookOne(const wchar_t* dll, void* hook, PFN_GetProcAddress* orig) {
    HMODULE m = GetModuleHandleW(dll);
    void* target = m ? reinterpret_cast<void*>(GetProcAddress(m, "GetProcAddress")) : nullptr;
    if (!target) {
        Log("OpenXR hook: %ls!GetProcAddress not found", dll);
        return false;
    }
    MH_STATUS st = MH_CreateHook(target, hook, reinterpret_cast<void**>(orig));
    if (st == MH_ERROR_ALREADY_CREATED) {        // kernel32's export forwards to the kernelbase function detoured already
        Log("OpenXR hook: %ls!GetProcAddress (%p) is the kernelbase function -- detoured once", dll, target);
        return true;
    }
    if (st == MH_OK) st = MH_EnableHook(target);
    if (st != MH_OK) {
        Log("OpenXR hook: detour of %ls!GetProcAddress (%p) failed: %s", dll, target, MH_StatusToString(st));
        return false;
    }
    Log("OpenXR hook: %ls!GetProcAddress detoured (%p)", dll, target);
    return true;
}

} // namespace

bool Install() {
    if (s_hooked.load()) return true;
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        Log("OpenXR hook: MH_Initialize failed: %s -- VR panel stays at the arm point", MH_StatusToString(st));
        return false;
    }
    Sets();                                      // the thunk table before the first lookup can need it
    // kernelbase first: once it is detoured, the kernel32 lookup below already runs through it (harmless: not an "xr" name)
    const bool kb = HookOne(L"kernelbase.dll", reinterpret_cast<void*>(&hkGetProcAddressKb), &oGetProcAddressKb);
    const bool k32 = HookOne(L"kernel32.dll", reinterpret_cast<void*>(&hkGetProcAddressK32), &oGetProcAddressK32);
    if (!kb && !k32) return false;
    s_hooked.store(true);
    Log("OpenXR hook: installed (kernelbase %s, kernel32 %s) -- the OpenXR runtime / API layer negotiation and "
        "xrReleaseSwapchainImage are wrapped when the game creates its OpenXR instance", kb ? "yes" : "NO", k32 ? "yes" : "NO");
    return true;
}
void SetReleaseCallback(ReleaseFn fn) { s_cb.store(fn, std::memory_order_release); }
bool     Hooked() { return s_hooked.load(std::memory_order_relaxed); }
int      Components() { return s_slotN.load(std::memory_order_relaxed); }
uint64_t Releases() { return s_releases.load(std::memory_order_relaxed); }
uint64_t ReleasesMapped() { return s_releasesMapped.load(std::memory_order_relaxed); }

// v0.10.3 phase 28 (see xr_hook.h). Phase 28b: the newest locate of the locate space with the newest locate, CORRECTED to the
// submitted pose (C (x) L, that space's C from the last xrEndFrame), in the projection LAYER's space, with the submitted fov.
// False until an xrEndFrame gave a correction (no layer = no picture to place the box in anyway).
bool LatestViews(Views* out) {
    if (!out) return false;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    AcquireSRWLockShared(&s_poseLock);
    const LocSpace* best = nullptr;
    for (const LocSpace& e : s_lsp) if (e.space && e.haveC && e.newest && (!best || e.newest > best->newest)) best = &e;
    const LocRec* rec = best ? &s_loc[best->newest % kRing] : nullptr;
    if (!best || !s_haveLayout || rec->seq != best->newest) {
        out->seq = 0;
        out->corrected = 0;
        ReleaseSRWLockShared(&s_poseLock);
        return false;
    }
    for (int k = 0; k < 2; ++k) {
        const Pose P = Compose(best->c[k], PoseOf(rec->v[k]));
        View& v = out->v[k];
        v.qx = (float)P.q[0]; v.qy = (float)P.q[1]; v.qz = (float)P.q[2]; v.qw = (float)P.q[3];
        v.px = (float)P.p[0]; v.py = (float)P.p[1]; v.pz = (float)P.p[2];
        v.angleLeft = s_subFov[k][0]; v.angleRight = s_subFov[k][1]; v.angleUp = s_subFov[k][2]; v.angleDown = s_subFov[k][3];
        out->raw[k] = rec->v[k];
    }
    out->space = s_layoutSpace;
    out->spaceType = SpaceTypeLocked(s_layoutSpace);
    out->locSpace = best->space;
    out->locSpaceType = best->type;
    out->flags = rec->flags;
    out->ageSec = freq.QuadPart > 0 ? (double)(now.QuadPart - rec->qpc) / (double)freq.QuadPart : 0.0;
    out->corrected = 1;
    out->seq = best->newest;
    ReleaseSRWLockShared(&s_poseLock);
    return true;
}
void PoseDiagArm(int seconds) {
    LARGE_INTEGER now, fq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&fq);
    AcquireSRWLockExclusive(&s_poseLock);
    s_diagUntil = seconds > 0 ? now.QuadPart + fq.QuadPart * seconds : 0;
    s_diagNext = now.QuadPart;
    ReleaseSRWLockExclusive(&s_poseLock);
}
int ViewForImage(void* tex, uint32_t slice, int32_t rect[4]) {
    if (!tex) return -1;
    XrSwapchain sc = nullptr;                    // the swapchain holding image `tex` (the release map)
    AcquireSRWLockShared(&s_scLock);
    for (const ScEntry& e : s_sc) {
        if (!e.sc) continue;
        for (uint32_t k = 0; k < e.n && !sc; ++k) if (e.img[k] == tex) sc = e.sc;
        if (sc) break;
    }
    ReleaseSRWLockShared(&s_scLock);
    if (!sc) return -1;
    int view = -1;
    AcquireSRWLockShared(&s_poseLock);
    if (s_haveLayout)
        for (int v = 0; v < 2 && view < 0; ++v)
            if (s_layout[v].sc == sc && s_layout[v].slice == slice) {
                view = v;
                if (rect) memcpy(rect, s_layout[v].rect, sizeof(s_layout[v].rect));
            }
    ReleaseSRWLockShared(&s_poseLock);
    return view;
}
PoseStats GetPoseStats() {
    PoseStats s{};
    s.locates = s_nLocates.load(std::memory_order_relaxed);
    s.endFrames = s_nEndFrames.load(std::memory_order_relaxed);
    s.matched = s_nMatched.load(std::memory_order_relaxed);
    s.unmatched = s_nUnmatched.load(std::memory_order_relaxed);
    AcquireSRWLockShared(&s_poseLock);
    s.resFrames = s_resN;
    s.resMeanDeg = s_resN ? s_resAngSum / (double)s_resN : 0.0;
    s.resMeanMm = s_resN ? s_resPosSum / (double)s_resN : 0.0;
    s.resMaxDeg = s_resAngMax;
    int n = 0;
    for (const LocSpace& e : s_lsp) {
        if (!e.space || n >= 4) continue;
        s.spaces[n].space = e.space; s.spaces[n].type = e.type; s.spaces[n].count = e.count;
        ++n;
    }
    s.nSpaces = n;
    s.layerSpaceType = SpaceTypeLocked(s_layoutSpace);
    ReleaseSRWLockShared(&s_poseLock);
    return s;
}

// ---- v0.10.3 phase 29: the overlay quads (see xr_hook.h) ----
void OverlaySet(const OverlayParams& p) {
    AcquireSRWLockExclusive(&s_ovParLock);
    s_ovPar = p;
    ReleaseSRWLockExclusive(&s_ovParLock);
    if (!p.enabled) { s_ovOk[0].store(false, std::memory_order_relaxed); s_ovOk[1].store(false, std::memory_order_relaxed); }
}
void SetOverlayDrawCallback(OverlayDrawFn fn) { s_ovCb.store(fn, std::memory_order_release); }
bool OverlayActive(int which) {
    if (which < 0 || which > 1 || !s_ovOk[which].load(std::memory_order_relaxed)) return false;
    static LONGLONG s_freq = 0;
    if (!s_freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_freq = f.QuadPart > 0 ? f.QuadPart : 1; }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart - s_ovOkQpc.load(std::memory_order_acquire) < s_freq / 2;   // the last xrEndFrame within 0.5 s
}
OverlayStats GetOverlayStats() {
    OverlayStats s;
    s.enabled = OvParams().enabled;
    s.ready = s_ovReady.load(std::memory_order_relaxed);
    s.mode = s_ovMode.load(std::memory_order_relaxed);
    s.format = s_ovFormat.load(std::memory_order_relaxed);
    s.frames = s_ovFrames.load(std::memory_order_relaxed);
    s.panelQuads = s_ovPanel.load(std::memory_order_relaxed);
    s.fpsQuads = s_ovFps.load(std::memory_order_relaxed);
    s.createFails = s_ovCreateFails.load(std::memory_order_relaxed);
    s.acquireFails = s_ovAcqFails.load(std::memory_order_relaxed);
    s.waitFails = s_ovWaitFails.load(std::memory_order_relaxed);
    s.releaseFails = s_ovRelFails.load(std::memory_order_relaxed);
    s.drawFails = s_ovDrawFails.load(std::memory_order_relaxed);
    s.rejects = s_ovRejects.load(std::memory_order_relaxed);
    s.fixedHow = s_ovFixedHow.load(std::memory_order_relaxed);
    s.fixedSpaceType = s_ovFixedType.load(std::memory_order_relaxed);
    return s;
}

} // namespace XrHook
