// XrHook -- see xr_hook.h. v0.10.3 phase 27: OpenXR swapchain release hook (GetProcAddress -> negotiation -> GIPA thunks).
#include "xr_hook.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>
#include "MinHook.h"
#include "log.h"
#include "fps_pose.h"                                 // phase 44: the fps box turn (ComposeTurn)
#include "frame_pace.h"                               // phase 52: the VR FPS cap's target maths + the precise sleeper

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
// phase 30 (Turbo mode): the frame loop types (openxr.h 1.0 layouts, same SDK header as above)
struct XrFrameWaitInfo  { int32_t type; const void* next; };
struct XrFrameState     { int32_t type; void* next; XrTime predictedDisplayTime; XrDuration predictedDisplayPeriod; uint32_t shouldRender; };
struct XrFrameBeginInfo { int32_t type; const void* next; };
static_assert(sizeof(XrFrameState) == 40 && offsetof(XrFrameState, predictedDisplayTime) == 16 &&
              offsetof(XrFrameState, shouldRender) == 32 && sizeof(XrFrameWaitInfo) == 16 && sizeof(XrFrameBeginInfo) == 16,
              "OpenXR struct layout (x64)");
constexpr int32_t kTypeFrameWaitInfo  = 33;                      // XR_TYPE_FRAME_WAIT_INFO
constexpr int32_t kTypeFrameState     = 44;                      // XR_TYPE_FRAME_STATE
constexpr int32_t kTypeFrameBeginInfo = 46;                      // XR_TYPE_FRAME_BEGIN_INFO
typedef XrResult (XRH_CALL* PFN_xrWaitFrame)(XrSession, const XrFrameWaitInfo*, XrFrameState*);
typedef XrResult (XRH_CALL* PFN_xrBeginFrame)(XrSession, const XrFrameBeginInfo*);
typedef XrResult (XRH_CALL* PFN_xrEndSession)(XrSession);
// phase 33: the session lifecycle + events (openxr.h 1.0 layouts, x64; values from the OpenXR-SDK 1.1.62 header named above)
struct XrSessionBeginInfo { int32_t type; const void* next; int32_t primaryViewConfigurationType; };
struct XrEvSessionStateChanged { int32_t type; const void* next; XrSession session; int32_t state; XrTime time; };
struct XrEvRefSpaceChangePending {
    int32_t type; const void* next; XrSession session; int32_t referenceSpaceType; XrTime changeTime; uint32_t poseValid;
    XrPosef poseInPreviousSpace;
};
struct XrEvEventsLost { int32_t type; const void* next; uint32_t lostEventCount; };
struct XrEvInstanceLossPending { int32_t type; const void* next; XrTime lossTime; };
struct XrEvRefreshRateChangedFB { int32_t type; const void* next; float from, to; };
struct XrEvSessionInt { int32_t type; const void* next; XrSession session; int32_t a; uint32_t b; };   // user presence (a = present),
                                                                                                        //   visibility mask (a, b)
struct XrEvPerfSettings { int32_t type; const void* next; int32_t domain, subDomain, fromLevel, toLevel; };
static_assert(offsetof(XrEvSessionStateChanged, session) == 16 && offsetof(XrEvSessionStateChanged, state) == 24 &&
              offsetof(XrEvSessionStateChanged, time) == 32 && sizeof(XrEvSessionStateChanged) == 40 &&
              offsetof(XrEvRefSpaceChangePending, referenceSpaceType) == 24 && offsetof(XrEvRefSpaceChangePending, changeTime) == 32 &&
              offsetof(XrEvRefSpaceChangePending, poseValid) == 40 && offsetof(XrEvRefSpaceChangePending, poseInPreviousSpace) == 44 &&
              sizeof(XrEvRefSpaceChangePending) == 72 && offsetof(XrEvEventsLost, lostEventCount) == 16 &&
              offsetof(XrEvInstanceLossPending, lossTime) == 16 && offsetof(XrEvRefreshRateChangedFB, from) == 16 &&
              offsetof(XrEvSessionInt, a) == 24 && offsetof(XrEvSessionInt, b) == 28 && sizeof(XrEvSessionInt) == 32 &&
              offsetof(XrEvPerfSettings, domain) == 16 && offsetof(XrSessionBeginInfo, primaryViewConfigurationType) == 16,
              "OpenXR event struct layout (x64)");
constexpr int32_t kTypeEvInstanceLossPending       = 17;          // XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING
constexpr int32_t kTypeEvSessionStateChanged       = 18;          // XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED
constexpr int32_t kTypeEvRefSpaceChangePending     = 40;          // XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING
constexpr int32_t kTypeEvEventsLost                = 49;          // XR_TYPE_EVENT_DATA_EVENTS_LOST
constexpr int32_t kTypeEvInteractionProfileChanged = 52;          // XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED
constexpr int32_t kTypeEvPerfSettingsExt           = 1000015000;  // XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT
constexpr int32_t kTypeEvVisibilityMaskChangedKhr  = 1000031001;  // XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR
constexpr int32_t kTypeEvMainSessionVisibilityExtx = 1000033003;  // XR_TYPE_EVENT_DATA_MAIN_SESSION_VISIBILITY_CHANGED_EXTX
constexpr int32_t kTypeEvRefreshRateChangedFb      = 1000101000;  // XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB
constexpr int32_t kTypeEvUserPresenceChangedExt    = 1000470000;  // XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT
typedef XrResult (XRH_CALL* PFN_xrPollEvent)(XrInstance, void* eventData);
typedef XrResult (XRH_CALL* PFN_xrCreateSession)(XrInstance, const void* createInfo, XrSession*);
typedef XrResult (XRH_CALL* PFN_xrBeginSession)(XrSession, const XrSessionBeginInfo*);
typedef XrResult (XRH_CALL* PFN_xrRequestExitSession)(XrSession);
// phase 40: the hidden area mask -- instance creation + XR_KHR_visibility_mask (openxr.h 1.0 layouts, x64; values from the same
// OpenXR-SDK 1.1.62 header as above)
struct XrApplicationInfo {
    char applicationName[128]; uint32_t applicationVersion; char engineName[128]; uint32_t engineVersion; XrVersion apiVersion;
};
struct XrInstanceCreateInfo {
    int32_t type; const void* next; uint64_t createFlags; XrApplicationInfo applicationInfo;
    uint32_t enabledApiLayerCount; const char* const* enabledApiLayerNames;
    uint32_t enabledExtensionCount; const char* const* enabledExtensionNames;
};
struct XrExtensionProperties { int32_t type; void* next; char extensionName[128]; uint32_t extensionVersion; };
struct XrVector2f { float x, y; };
struct XrVisibilityMaskKHR {
    int32_t type; void* next; uint32_t vertexCapacityInput, vertexCountOutput; XrVector2f* vertices;
    uint32_t indexCapacityInput, indexCountOutput; uint32_t* indices;
};
static_assert(sizeof(XrApplicationInfo) == 272 && offsetof(XrApplicationInfo, apiVersion) == 264 &&
              offsetof(XrInstanceCreateInfo, applicationInfo) == 24 && offsetof(XrInstanceCreateInfo, enabledApiLayerCount) == 296 &&
              offsetof(XrInstanceCreateInfo, enabledExtensionCount) == 312 &&
              offsetof(XrInstanceCreateInfo, enabledExtensionNames) == 320 && sizeof(XrInstanceCreateInfo) == 328 &&
              offsetof(XrExtensionProperties, extensionName) == 16 && sizeof(XrExtensionProperties) == 152 &&
              offsetof(XrVisibilityMaskKHR, vertices) == 24 && offsetof(XrVisibilityMaskKHR, indexCountOutput) == 36 &&
              offsetof(XrVisibilityMaskKHR, indices) == 40 && sizeof(XrVisibilityMaskKHR) == 48, "OpenXR struct layout (x64)");
constexpr int32_t kTypeInstanceCreateInfo  = 3;            // XR_TYPE_INSTANCE_CREATE_INFO
constexpr int32_t kTypeExtensionProperties = 2;            // XR_TYPE_EXTENSION_PROPERTIES
constexpr int32_t kTypeEventDataBuffer     = 16;           // XR_TYPE_EVENT_DATA_BUFFER
constexpr int32_t kTypeVisibilityMaskKhr   = 1000031000;   // XR_TYPE_VISIBILITY_MASK_KHR
constexpr int32_t kMaskHiddenTriangleMesh  = 1;            // XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR
constexpr XrResult kEventUnavailable       = 4;            // XR_EVENT_UNAVAILABLE
const char* const kVisMaskExt = "XR_KHR_visibility_mask";
typedef XrResult (XRH_CALL* PFN_xrCreateInstance)(const XrInstanceCreateInfo*, XrInstance*);
typedef XrResult (XRH_CALL* PFN_xrEnumerateInstanceExtensionProperties)(const char*, uint32_t, uint32_t*, XrExtensionProperties*);
typedef XrResult (XRH_CALL* PFN_xrGetVisibilityMaskKHR)(XrSession, int32_t, uint32_t, int32_t, XrVisibilityMaskKHR*);

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
    std::atomic<void*> waitFrame{nullptr}, beginFrame{nullptr}, endSession{nullptr};  // phase 30 (Turbo mode)
    std::atomic<void*> pollEvent{nullptr}, createSession{nullptr}, beginSession{nullptr}, requestExit{nullptr};   // phase 33
    std::atomic<void*> createInstance{nullptr};                                       // phase 40 (runtime slots only)
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
            LogWarn("OpenXR overlay: cannot resolve the swapchain functions yet at the %s %s (GIPA %p, instance %p) -- the VR panel "
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
            LogWarn("OpenXR overlay: %s not resolved at the %s %s (instance %p) -- the VR panel stays in the eye picture",
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
        LogErr("OpenXR overlay: %s failed (result %d) -- the VR panel / fps box stay in the eye picture for this session", what, r);
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
        LogWarn("OpenXR overlay: the runtime offers none of the 8-bit RGBA swapchain formats (offered: %s)", list);
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
        if (n < 4) LogErr("OpenXR overlay: xrAcquireSwapchainImage of our %s swapchain failed (result %d)", which ? "fps box" : "panel", r);
        if (r == kErrSessionLost) s_ov.dead = true;
        return false;
    }
    const XrSwapchainImageWaitInfo wi{ kTypeSwapchainImageWaitInfo, nullptr, kInfiniteDuration };
    r = o.wait(c.sc, &wi);
    if (r < 0 || r == kTimeoutExpired) {                 // an acquired image we may not release: the session is given up
        s_ovWaitFails.fetch_add(1, std::memory_order_relaxed);
        LogErr("OpenXR overlay: xrWaitSwapchainImage of our %s swapchain failed (result %d) -- the quad path is off for this session",
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
        LogErr("OpenXR overlay: xrReleaseSwapchainImage of our %s swapchain failed (result %d) -- the quad path is off for this session",
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
        LogWarn("OpenXR overlay: 60 frames in a row without an image of our swapchains -- the quad path is off for this session");
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
        LogDbg("OpenXR overlay: %s quad orientation check: yaw %+.1f pitch %+.1f deg, R(q)(0,0,1) = (%+.3f %+.3f %+.3f) vs back-to-%s "
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
            LogWarn("OpenXR overlay: the submitted view 0 fov (L%+.1f R%+.1f U%+.1f D%+.1f deg) is not usable -- no quads",
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
        // the fps box: fps_vr_dist (phase 44; was 0.8 m = the phase 28 anchor distance, still the default). wM scales with d:
        // the same angular size at every distance, in both lock modes.
        const double d = par.fpsDist >= 0.3f && par.fpsDist <= 3.0f ? (double)par.fpsDist : 0.8;
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
        // phase 44: the box's own turn (fps_vr_yaw / _pitch / _roll) in its local frame, on top of the facing above
        if (!FpsPose::TurnIsZero(par.fpsYaw, par.fpsPitch, par.fpsRoll)) {
            double t[4];
            FpsPose::ComposeTurn(rq, par.fpsYaw, par.fpsPitch, par.fpsRoll, t);
            memcpy(rq, t, sizeof(t));
        }
    }
    q.pose.orientation.x = (float)rq[0]; q.pose.orientation.y = (float)rq[1];
    q.pose.orientation.z = (float)rq[2]; q.pose.orientation.w = (float)rq[3];
    q.pose.position.x = (float)pos[0]; q.pose.position.y = (float)pos[1]; q.pose.position.z = (float)pos[2];
    q.size.width = (float)wM;
    q.size.height = (float)(wM * aspect);
    if (!s_ov.firstLog[logIdx]) {
        s_ov.firstLog[logIdx] = true;
        static const char* const kWhat[3] = { "panel (VIEW space, ours)", "fps box following the head (VIEW space, ours)",
                                              "fps box FIXED IN THE PLAY SPACE (the game's projection-layer space)" };
        Log("OpenXR overlay: first %s quad -- space %p, pose q=(%+.3f %+.3f %+.3f %+.3f) p=(%+.3f %+.3f %+.3f) m, size %.3f x %.3f m, "
            "sub-image %dx%d of %ux%u px; view 0 fov tangents L%+.3f R%+.3f U%+.3f D%+.3f%s%s", kWhat[logIdx], q.space,
            rq[0], rq[1], rq[2], rq[3], pos[0], pos[1], pos[2], wM, wM * aspect, q.subImage.w, q.subImage.h, s_ov.sc[which].w,
            s_ov.sc[which].h, tl, tr, tu, tdn,
            how == 3 ? " (fixed in the cab asked, but the layer space is a VIEW space: head-locked)" : "",
            logIdx == 2 ? "" : (par.menuFace ? "; menu_vr_face=1: turned to face the head (phase 29b)"
                                             : "; menu_vr_face=0: parallel to the view plane"));
    }
    // phase 44: the fps box quad again whenever its lock / distance / turn changes (level 4; 32 lines at most)
    if (which == 1) {
        static int s_key[5] = { -1, -1, -999, -999, -999 }, s_lines = 0;
        const int key[5] = { logIdx, (int)std::lround(par.fpsDist * 100.0f), par.fpsYaw, par.fpsPitch, par.fpsRoll };
        if (memcmp(key, s_key, sizeof(key)) && s_lines < 32) {
            ++s_lines;
            memcpy(s_key, key, sizeof(key));
            LogDbg("OpenXR overlay: fps box quad (%s) -- fps_vr_dist %.2f m, turn yaw %+d pitch %+d roll %+d deg -> pose q=(%+.3f %+.3f "
                "%+.3f %+.3f) p=(%+.3f %+.3f %+.3f) m, size %.3f x %.3f m (~%.2f deg wide at that distance)", logIdx == 2 ? "fixed in the play space" :
                "following the head", (double)par.fpsDist, par.fpsYaw, par.fpsPitch, par.fpsRoll, rq[0], rq[1], rq[2], rq[3], pos[0],
                pos[1], pos[2], wM, wM * aspect,
                2.0 * std::atan(0.5 * wM / (par.fpsDist >= 0.3f ? (double)par.fpsDist : 0.8)) * 57.29578);
        }
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
            LogWarn("OpenXR overlay: the game submits %d layers (we append ours up to %d in all) -- no quads in such frames", nGame,
                kOvMaxLayers);
        }
        OvSetOk(ok[0], ok[1]);
        return false;
    }
    // phase 29b: the later layer is drawn on top -- the panel last when it sits nearer than the fps box (menu_vr_dist vs fps_vr_dist)
    const bool panelTop = par.menuDist < par.fpsDist;     // phase 44: the fps box's own distance (was its fixed 0.8 m)
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
    LogErr("OpenXR overlay: the runtime REJECTED xrEndFrame with our quad layers (result %d) -- the frame was submitted again without "
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

// ---- phase 33: names for the log -- OpenXR results, session states, event types (values checked against openxr.h of
// OpenXR-SDK 1.1.62, E:/OpenXR-EyeCapture/build/_deps/openxr_sdk-src/include/openxr/openxr.h) --------------------------------
const char* XrResultName(XrResult r) {
    switch (r) {
    case 0: return "XR_SUCCESS";                       case 1: return "XR_TIMEOUT_EXPIRED";
    case 3: return "XR_SESSION_LOSS_PENDING";          case 4: return "XR_EVENT_UNAVAILABLE";
    case 7: return "XR_SPACE_BOUNDS_UNAVAILABLE";      case 8: return "XR_SESSION_NOT_FOCUSED";
    case 9: return "XR_FRAME_DISCARDED";               case -1: return "XR_ERROR_VALIDATION_FAILURE";
    case -2: return "XR_ERROR_RUNTIME_FAILURE";        case -3: return "XR_ERROR_OUT_OF_MEMORY";
    case -4: return "XR_ERROR_API_VERSION_UNSUPPORTED"; case -6: return "XR_ERROR_INITIALIZATION_FAILED";
    case -7: return "XR_ERROR_FUNCTION_UNSUPPORTED";   case -9: return "XR_ERROR_EXTENSION_NOT_PRESENT";
    case -10: return "XR_ERROR_LIMIT_REACHED";         case -11: return "XR_ERROR_SIZE_INSUFFICIENT";
    case -12: return "XR_ERROR_HANDLE_INVALID";        case -13: return "XR_ERROR_INSTANCE_LOST";
    case -14: return "XR_ERROR_SESSION_RUNNING";       case -16: return "XR_ERROR_SESSION_NOT_RUNNING";
    case -17: return "XR_ERROR_SESSION_LOST";          case -18: return "XR_ERROR_SYSTEM_INVALID";
    case -23: return "XR_ERROR_LAYER_INVALID";         case -24: return "XR_ERROR_LAYER_LIMIT_EXCEEDED";
    case -25: return "XR_ERROR_SWAPCHAIN_RECT_INVALID"; case -28: return "XR_ERROR_SESSION_NOT_READY";
    case -29: return "XR_ERROR_SESSION_NOT_STOPPING";  case -30: return "XR_ERROR_TIME_INVALID";
    case -31: return "XR_ERROR_REFERENCE_SPACE_UNSUPPORTED"; case -37: return "XR_ERROR_CALL_ORDER_INVALID";
    case -38: return "XR_ERROR_GRAPHICS_DEVICE_INVALID"; case -39: return "XR_ERROR_POSE_INVALID";
    case -40: return "XR_ERROR_INDEX_OUT_OF_RANGE";    case -41: return "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED";
    case -42: return "XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED"; case -50: return "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING";
    default: return r < 0 ? "XR_ERROR_(other)" : "XR_(other success code)";
    }
}
const char* SessionStateName(int s) {               // XrSessionState
    static const char* const k[9] = { "UNKNOWN", "IDLE", "READY", "SYNCHRONIZED", "VISIBLE", "FOCUSED", "STOPPING", "LOSS_PENDING",
                                       "EXITING" };
    return s >= 0 && s < 9 ? k[s] : "?";
}
const char* EventTypeName(int32_t t) {              // XrStructureType of an XrEventDataBuffer
    switch (t) {
    case kTypeEvInstanceLossPending:        return "INSTANCE_LOSS_PENDING";
    case kTypeEvSessionStateChanged:        return "SESSION_STATE_CHANGED";
    case kTypeEvRefSpaceChangePending:      return "REFERENCE_SPACE_CHANGE_PENDING";
    case kTypeEvEventsLost:                 return "EVENTS_LOST";
    case kTypeEvInteractionProfileChanged:  return "INTERACTION_PROFILE_CHANGED";
    case kTypeEvPerfSettingsExt:            return "PERF_SETTINGS_EXT";
    case kTypeEvVisibilityMaskChangedKhr:   return "VISIBILITY_MASK_CHANGED_KHR";
    case kTypeEvMainSessionVisibilityExtx:  return "MAIN_SESSION_VISIBILITY_CHANGED_EXTX";
    case kTypeEvRefreshRateChangedFb:       return "DISPLAY_REFRESH_RATE_CHANGED_FB";
    case kTypeEvUserPresenceChangedExt:     return "USER_PRESENCE_CHANGED_EXT";
    default:                                return "other";
    }
}
int64_t SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- phase 33: the session lifecycle as the game sees it (who starts a session restart: the runtime or the game) -----------
std::atomic<int>      s_sesState{0};                 // the newest XrSessionState the game was handed (xrPollEvent); 0 = none
std::atomic<int64_t>  s_sesStateNs{0};               // ... when (steady clock)
std::atomic<int>      s_rtStop{0};                   // STOPPING / LOSS_PENDING / EXITING handed to the game since the last
std::atomic<int64_t>  s_rtStopNs{0};                 //   xrBeginSession (the highest one; 0 = none) -- the RUNTIME asks
std::atomic<bool>     s_instLoss{false};             // INSTANCE_LOSS_PENDING seen since the last xrBeginSession
std::atomic<bool>     s_exitAsked{false};            // the game called xrRequestExitSession since the last xrBeginSession
std::atomic<bool>     s_endCalled{false};            // the game called xrEndSession since the last xrBeginSession
std::atomic<int>      s_sesLogs{0};                  // lifecycle + state lines so far (cap kSesLogCap)
constexpr int         kSesLogCap = 2000;
// "who started it" for the xrEndSession / xrDestroySession lines
void WhoStarted(char* b, size_t n) {
    const int stop = s_rtStop.load(std::memory_order_relaxed);
    if (s_exitAsked.load(std::memory_order_relaxed))
        snprintf(b, n, "=> started by the GAME (it called xrRequestExitSession first)");
    else if (stop || s_instLoss.load(std::memory_order_relaxed))
        snprintf(b, n, "=> started by the RUNTIME (it sent %s %.1f s before; the game did not call xrRequestExitSession)",
                 stop ? SessionStateName(stop) : "INSTANCE_LOSS_PENDING",
                 stop ? (double)(SteadyNs() - s_rtStopNs.load(std::memory_order_relaxed)) / 1e9 : 0.0);
    else
        snprintf(b, n, "=> started by the GAME on its own (no STOPPING / LOSS_PENDING / EXITING / INSTANCE_LOSS_PENDING from the "
                 "runtime, no xrRequestExitSession; last session state %s, %.1f s ago)",
                 SessionStateName(s_sesState.load(std::memory_order_relaxed)),
                 s_sesStateNs.load(std::memory_order_relaxed)
                     ? (double)(SteadyNs() - s_sesStateNs.load(std::memory_order_relaxed)) / 1e9 : -1.0);
}

// ---- phase 33: non-XR_SUCCESS results of the frame loop (the first 20 of each kind logged, then only counted) ----------------
enum NsKind { kNsWait = 0, kNsBegin, kNsEnd, kNsLocate, kNsAsyncWait, kNsTbBegin, kNsTbSyncWait, kNsCount };
const char* const kNsName[kNsCount] = { "xrWaitFrame", "xrBeginFrame", "xrEndFrame", "xrLocateViews", "async xrWaitFrame (turbo)",
                                        "xrBeginFrame by turbo (at xrEndFrame)", "xrWaitFrame by turbo at xrEndFrame" };
std::atomic<uint64_t> s_ns[kNsCount];
void TbDescribe(char* b, size_t n);                   // (below: the turbo state, lock-free)
void NoteResult(int kind, XrResult r, XrSession ses) {
    if (r == 0) return;
    const uint64_t k = s_ns[kind].fetch_add(1, std::memory_order_relaxed);
    if (k >= 20) return;
    char tb[400];
    TbDescribe(tb, sizeof(tb));
    LogWarn("OpenXR result: %s(session %p) returned %d = %s (#%llu of this kind%s; tid=%lu) | %s", kNsName[kind], ses, r, XrResultName(r),
        (unsigned long long)k + 1, k == 19 ? "; the rest are only counted in the turbo stats line" : "", GetCurrentThreadId(), tb);
}

// ---- phase 30: TURBO MODE (dlaa.ini xr_turbo; the tuning menu's Turbo mode row; see xr_hook.h) -----------------------------
// The logic below is ported from OpenXR Toolkit (XR_APILAYER_MBUCCHIA_toolkit/layer.cpp: xrWaitFrame, xrBeginFrame, the end of
// xrEndFrame, xrDestroySession; setting "turbo"), used under its MIT licence:
//
//   MIT License
//
//   Copyright (c) 2021-2022 Matthieu Bucchianeri
//   Copyright (c) 2021-2022 Jean-Luc Dupiot - Reality XP
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
//   files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
//   modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//   Software is furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
//   Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
//   WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
//   COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
//   ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// How it works (the game's OUTERMOST calls only; t_depth > 0 = an inner layer's call or ours: passed straight through):
//  * a worker thread calls the REAL xrWaitFrame (the slot's next function, never our thunk; ++t_depth on the worker so the
//    inner layers' thunks pass it through) and keeps its predictedDisplayTime / predictedDisplayPeriod;
//  * the game's xrWaitFrame while that wait is pending does not wait: it gets a made-up frame state (the async time when the
//    wait is done, else the last real time + the time since the game's previous xrWaitFrame, at most 2 frame periods ahead
//    (phase 33); the period of the last wait; shouldRender TRUE; strictly increasing). A SECOND such poll before a frame was
//    ended first blocks on the pending wait (exactly one frame of pipelining, as the Toolkit; bounded at 1 s here);
//  * the game's xrBeginFrame after a made-up frame state returns XR_SUCCESS without the real call (the frame is a TURBO frame);
//  * the game's xrEndFrame of a turbo frame: up to 1 s for the pending wait to finish, the REAL xrBeginFrame, the real
//    xrEndFrame (after the phase 29 quads are appended, as before), then the next async wait if turbo is still on.
// phase 33 (ETS2 VR log 2026-10-11 16:02-16:14: the game calls xrWaitFrame + xrBeginFrame on its MAIN thread and xrEndFrame on
// its RENDER thread, and the main thread runs ahead: its real xrWaitFrame of frame N+1 is usually already running -- or done,
// not yet begun -- while the render thread is still in xrEndFrame of frame N. Phase 30 armed the first async wait at that
// xrEndFrame: then the game's real wait of N+1 and our async wait were outstanding TOGETHER (two concurrent xrWaitFrame = not
// allowed: the calls must be externally synchronised), the game's xrBeginFrame of N+1 was then skipped (a wait was pending),
// and our async wait -- which the runtime holds until the previous waited frame is BEGUN -- could only finish after the next
// xrEndFrame's real xrBeginFrame, which first waited 1 s for it: the 1-s freeze at every turbo start, at the switch-on and in
// every new session):
//  * the FIRST async wait is armed right after a real xrBeginFrame of the game (on the thread that called it), and only when no
//    real game xrWaitFrame is in flight: at that point every frame the runtime waited has been begun, so the async wait is the
//    only xrWaitFrame outstanding and it needs nothing but time. Steady state as before: each turbo frame's xrEndFrame posts the
//    next wait (no real game wait can be running then: every game wait is answered with a made-up state while one is pending);
//  * every frame the game begins carries a TAG (a small FIFO from the game's xrBeginFrame to its xrEndFrame): REAL = the game's
//    own xrBeginFrame reached the runtime (its xrEndFrame is passed through: nothing consumed, nothing begun -- the arming frame),
//    TURBO = its xrWaitFrame got a made-up state and its xrBeginFrame was skipped (its xrEndFrame consumes the pending wait and
//    calls the real xrBeginFrame). Phase 30 decided by "a wait is pending" at each call, which mixed up the frames of the two
//    threads at a switch;
//  * a TURBO frame whose xrEndFrame finds NO wait pending (turbo was switched off -- or given up -- while the main thread had
//    already been handed its made-up state): its frame is waited for for real right there, then begun (phase 30 let the game's
//    next xrBeginFrame through without any real xrWaitFrame = XR_ERROR_CALL_ORDER_INVALID, the frame lost);
//  * no new async wait while the runtime has asked the session to stop (STOPPING / LOSS_PENDING / EXITING / IDLE handed to the
//    game by xrPollEvent); xrBeginSession / xrEndSession / xrDestroySession reset the frame tags and the "given up" mark (the
//    runtime reuses session handle values: VD's sessions are all 0x1);
//  * a real predicted display time is only nudged to stay increasing when it is within 2 frame periods of the last one handed
//    out (phase 30 nudged any amount: after a 1-s stall every frame got the same time ~1 s in the future for ~1 s).
// Locks: s_tbMx = the frame state below; held by the game's xrWaitFrame / xrBeginFrame checks (and over the game's real
// xrBeginFrame) and over the whole turbo part of xrEndFrame (as the Toolkit's frame lock: another thread's xrWaitFrame sees
// either the old or the new pending wait, never the gap between them). TbWorker::mx = the worker's job / result data (never
// held while waiting for anything but its own condition). The worker never takes s_tbMx: blocking on it with s_tbMx held
// cannot deadlock. The log's view of the state (TbDescribe) reads atomic mirrors only (never s_tbMx).
struct TbWorker {                                // heap, never destroyed (no destructor at process exit / with the loader lock)
    std::mutex              mx;
    std::condition_variable cv;
    bool                    started = false;     // the thread exists (under s_tbMx)
    uint64_t                jobSeq = 0, doneSeq = 0;   // the newest posted / finished async wait
    std::atomic<uint64_t>   doneSeqA{0};         // phase 33: doneSeq for the log (lock-free)
    XrSession               ses = nullptr;
    PFN_xrWaitFrame         fn = nullptr;        // the REAL xrWaitFrame of the slot that posted it
    XrResult                result = 0;          // of doneSeq
    XrTime                  lastTime = 0;        // the last REAL predicted display time / period (the game's own real waits
    XrDuration              lastPeriod = 0;      //   too: needed for the switch into turbo)
};
TbWorker& Wk() { static TbWorker* w = new TbWorker; return *w; }
enum : uint8_t { kTagReal = 1, kTagTurbo = 2 };  // phase 33: how a game frame was begun (see above)
constexpr int kTags = 8;
struct TbState {                                 // under s_tbMx
    XrSession ses = nullptr;                     // the session of the pending wait
    bool      pending = false;                   // an async wait was started and no turbo frame's xrEndFrame consumed it yet
    uint64_t  seq = 0;                           // ... its job seq
    bool      polled = false;                    // the pending wait is CLAIMED: a made-up game frame will consume it (phase 30:
                                                 //   "answered once"; phase 33: also set at a post for a frame already made up)
    bool      lastWaitFake = false;              // phase 33: the game's newest xrWaitFrame got a made-up state (its xrBeginFrame
                                                 //   is skipped: a TURBO frame)
    uint8_t   tags[kTags] = {};                  // phase 33: the begun, not yet ended game frames (FIFO, oldest at tagH)
    int       tagH = 0, tagN = 0;
    int       gameWaits = 0;                     // phase 33: real game xrWaitFrame calls running right now
    bool      gameWaited = false;                // phase 33: a real game xrWaitFrame returned and its xrBeginFrame has not come
    int64_t   lastGameWaitNs = 0;                // steady clock at the game's previous xrWaitFrame
    XrTime    lastReturned = 0;                  // the last predicted display time handed to the game (kept increasing)
    int64_t   aheadMaxNs = 0;                    // phase 33: the most a made-up time was ahead of the newest real one (window)
    bool      loggedOn = false;                  // the turbo state the log last announced
    bool      broken = false;                    // turbo given up for `brokenSes` (a failed async wait / real xrBeginFrame)
    XrSession brokenSes = nullptr;
    int64_t   statsNextNs = 0;
    uint64_t  statsPrev[12] = {};
    uint64_t  nsPrev = 0;                        // phase 33: the non-success results total at the last stats line
};
TbState               s_tb;
std::mutex            s_tbMx;
std::condition_variable s_tbCv;                  // phase 33: s_tb.pending went false (a switch-off waits for it, s_tbMx)
std::atomic<bool>     s_tbWant{false};
std::atomic<bool>     s_tbFailed{false};
std::atomic<int64_t>  s_tbLastStartNs{0};
std::atomic<uint64_t> s_tbStarts{0}, s_tbFaked{0}, s_tbBeginSkips{0}, s_tbBlocks{0}, s_tbToEnd{0}, s_tbToWait{0}, s_tbFails{0};
std::atomic<uint64_t> s_tbArms{0}, s_tbSyncWaits{0}, s_tbBumps{0}, s_tbBackJumps{0}, s_tbTagDrops{0};   // phase 33
// phase 33: lock-free mirrors of s_tb for TbDescribe (written under s_tbMx by TbMirrorLocked)
std::atomic<bool>     s_tbPendA{false}, s_tbPolledA{false}, s_tbFakeA{false};
std::atomic<uint64_t> s_tbSeqA{0};
std::atomic<int>      s_tbTagsA{0}, s_tbTurboTagsA{0}, s_tbGameWaitsA{0};

// ---- phase 52: VR FPS CAP (dlaa.ini vr_fps_cap; see xr_hook.h, frame_pace.h) ----------------------------------------------
// Why sleeping BEFORE the real xrWaitFrame (and not after it, with a made-up later display time): the runtime then still decides
// the display slot and predicts its time itself, so the pose the game renders with, the time it hands back at xrEndFrame and
// the frame the runtime shows all agree -- no fiction to keep consistent across wait / begin / end, turbo's tags or OFXR
// Bridge. The runtime only sees a game that is slower than the display; its reprojection (ASW / Motion Smoothing / SSW) fills
// the slots in between, at a cadence that no longer changes (the point of the cap). Same idea as OpenXR Toolkit's frame
// throttling (a sleep in front of the real xrWaitFrame); written here, not ported.
// The game's outermost xrWaitFrame calls are externally synchronised (OpenXR rule) -> the plain statics need no lock.
std::atomic<int>      s_capDiv{0};
std::atomic<int64_t>  s_capBaseA{0}, s_capSleepNs{0}, s_gameWaitNsA{0};
std::atomic<uint64_t> s_capWaits{0}, s_capSleeps{0};
int64_t               s_capLastReturn = 0;              // FramePace::NowNs when the game's last real xrWaitFrame returned
int64_t               s_capBase = 0;                    // FramePace::VrBasePeriod of this run of waits
FramePace::Sleeper    s_capSleeper;
int                   s_capAnnouncedDiv = 0;           // the div of the last "VR FPS cap:" line (one line per change)

void TbMirrorLocked() {
    int turbo = 0;
    for (int k = 0; k < s_tb.tagN; ++k) if (s_tb.tags[(s_tb.tagH + k) % kTags] == kTagTurbo) ++turbo;
    s_tbPendA.store(s_tb.pending, std::memory_order_relaxed);
    s_tbPolledA.store(s_tb.polled, std::memory_order_relaxed);
    s_tbFakeA.store(s_tb.lastWaitFake, std::memory_order_relaxed);
    s_tbSeqA.store(s_tb.seq, std::memory_order_relaxed);
    s_tbTagsA.store(s_tb.tagN, std::memory_order_relaxed);
    s_tbTurboTagsA.store(turbo, std::memory_order_relaxed);
    s_tbGameWaitsA.store(s_tb.gameWaits, std::memory_order_relaxed);
}
void TbDescribe(char* b, size_t n) {
    const bool pend = s_tbPendA.load(std::memory_order_relaxed);
    const uint64_t seq = s_tbSeqA.load(std::memory_order_relaxed), done = Wk().doneSeqA.load(std::memory_order_relaxed);
    char aw[96];
    if (pend)
        snprintf(aw, sizeof(aw), "#%llu pending, %s, started %.1f ms ago", (unsigned long long)seq,
                 done >= seq ? "finished (not consumed yet)" : "RUNNING",
                 (double)(SteadyNs() - s_tbLastStartNs.load(std::memory_order_relaxed)) / 1e6);
    else
        snprintf(aw, sizeof(aw), "none pending (%llu posted in all)", (unsigned long long)s_tbStarts.load(std::memory_order_relaxed));
    snprintf(b, n, "turbo %s%s: async wait %s; game frames begun + not ended %d (%d turbo); last game xrWaitFrame %s; game waits in "
             "flight %d; session state %s", s_tbWant.load(std::memory_order_relaxed) ? "ON" : "off",
             s_tbFailed.load(std::memory_order_relaxed) ? " (given up for this session)" : "", aw,
             s_tbTagsA.load(std::memory_order_relaxed), s_tbTurboTagsA.load(std::memory_order_relaxed),
             s_tbFakeA.load(std::memory_order_relaxed) ? "made up" : "real", s_tbGameWaitsA.load(std::memory_order_relaxed),
             SessionStateName(s_sesState.load(std::memory_order_relaxed)));
}
void TbPushTagLocked(uint8_t t) {
    if (s_tb.tagN == kTags) {                    // (never: a game ends what it begins) drop the oldest
        s_tb.tagH = (s_tb.tagH + 1) % kTags;
        --s_tb.tagN;
        s_tbTagDrops.fetch_add(1, std::memory_order_relaxed);
    }
    s_tb.tags[(s_tb.tagH + s_tb.tagN) % kTags] = t;
    ++s_tb.tagN;
}
uint8_t TbPopTagLocked() {                       // 0 = no begun frame known (treated as REAL: passed through)
    if (!s_tb.tagN) return 0;
    const uint8_t t = s_tb.tags[s_tb.tagH];
    s_tb.tagH = (s_tb.tagH + 1) % kTags;
    --s_tb.tagN;
    return t;
}
// A new async wait may start: the runtime has not asked the session to stop (0 = no state seen yet: allowed).
bool TbSessionStateOk() {
    const int st = s_sesState.load(std::memory_order_relaxed);
    return st == 0 || (st >= 2 && st <= 5);      // READY, SYNCHRONIZED, VISIBLE, FOCUSED
}

DWORD WINAPI TbWorkerMain(LPVOID) {
    TbWorker& w = Wk();
    uint64_t done = 0;
    for (;;) {
        uint64_t seq;
        XrSession ses;
        PFN_xrWaitFrame fn;
        {
            std::unique_lock<std::mutex> lk(w.mx);
            w.cv.wait(lk, [&] { return w.jobSeq != done; });
            seq = w.jobSeq; ses = w.ses; fn = w.fn;   // (posted twice while busy: only the newest runs)
        }
        CpuProf::SetRole(CpuProf::kRoleTurbo);   // phase 33: the CPU profiler's thread label
        const XrFrameWaitInfo wi{ kTypeFrameWaitInfo, nullptr };
        XrFrameState fs{ kTypeFrameState, nullptr, 0, 0, 0 };
        ++t_depth;                               // the inner layers' xrWaitFrame thunks pass it through (never the turbo path)
        const XrResult r = fn ? fn(ses, &wi, &fs) : -1;
        --t_depth;
        {
            std::lock_guard<std::mutex> lk(w.mx);
            w.result = r;
            if (r >= 0 && seq == w.jobSeq) { w.lastTime = fs.predictedDisplayTime; w.lastPeriod = fs.predictedDisplayPeriod; }
            w.doneSeq = seq;
            w.doneSeqA.store(seq, std::memory_order_relaxed);
            done = seq;
        }
        w.cv.notify_all();
        if (r != 0) NoteResult(kNsAsyncWait, r, ses);   // phase 33 (never under s_tbMx: lock-free)
    }
}
// Waits up to `ms` for async wait `seq` to finish; true = finished (*r = its result).
bool TbWaitDone(uint64_t seq, int ms, XrResult* r) {
    TbWorker& w = Wk();
    std::unique_lock<std::mutex> lk(w.mx);
    const bool ok = w.cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return w.doneSeq >= seq; });
    if (ok && r) *r = w.result;
    return ok;
}
// Under s_tbMx: the next async wait. claimed = a made-up game frame is already waiting for it (phase 33). False = the worker
// thread could not be made (logged once).
bool TbStartLocked(XrSession ses, PFN_xrWaitFrame fn, bool claimed) {
    TbWorker& w = Wk();
    if (!w.started) {
        HANDLE h = CreateThread(nullptr, 0, &TbWorkerMain, nullptr, 0, nullptr);
        if (!h) {
            static bool s_logged = false;
            if (!s_logged) { s_logged = true; LogErr("OpenXR turbo: the wait thread could not be created (error %lu) -- turbo stays off", GetLastError()); }
            return false;
        }
        CloseHandle(h);
        w.started = true;
    }
    {
        std::lock_guard<std::mutex> lk(w.mx);
        w.ses = ses;
        w.fn = fn;
        s_tb.seq = ++w.jobSeq;
    }
    w.cv.notify_all();
    s_tb.ses = ses;
    s_tb.pending = true;
    s_tb.polled = claimed;
    s_tbStarts.fetch_add(1, std::memory_order_relaxed);
    s_tbLastStartNs.store(SteadyNs(), std::memory_order_relaxed);
    return true;
}
// phase 33: xrBeginSession / xrEndSession / xrDestroySession by the game (s_tbMx taken here): a pending wait of `ses` is drained
// first when `drain` (up to 5 s, as the Toolkit's xrDestroySession) and forgotten; the frame tags, the made-up mark and the
// "given up" mark of `ses` are reset. `out` = what was done (for the caller's lifecycle line).
void TbSessionReset(XrSession ses, bool drain, char* out, size_t n) {
    std::lock_guard<std::mutex> lk(s_tbMx);
    const bool had = s_tb.pending && ses == s_tb.ses;
    const int tags = s_tb.tagN;
    const bool fake = s_tb.lastWaitFake;
    bool ok = true;
    int64_t waitedMs = 0;
    if (had && drain) {
        const int64_t t0 = SteadyNs();
        ok = TbWaitDone(s_tb.seq, 5000, nullptr);
        waitedMs = (SteadyNs() - t0) / 1000000;
    }
    if (had && drain) { s_tb.pending = false; s_tb.polled = false; }
    s_tb.tagN = 0; s_tb.tagH = 0;
    s_tb.lastWaitFake = false;
    s_tb.gameWaited = false;
    s_tb.lastGameWaitNs = 0;
    s_tbCv.notify_all();
    if (s_tb.broken && ses == s_tb.brokenSes) {
        s_tb.broken = false;
        s_tb.brokenSes = nullptr;
        s_tbFailed.store(false, std::memory_order_relaxed);
    }
    TbMirrorLocked();
    snprintf(out, n, "turbo %s: %s; %d begun + not ended frame tag(s)%s cleared", s_tbWant.load(std::memory_order_relaxed) ? "ON" : "off",
             !had ? "no async wait pending" : !drain ? "an async wait of this session is pending (kept)"
             : ok ? (waitedMs ? "the pending async xrWaitFrame was drained (finished) and dropped" : "the pending async xrWaitFrame had finished; dropped")
                  : "the pending async xrWaitFrame did NOT finish within 5 s (dropped anyway; the runtime ends it)",
             tags, fake ? " + the made-up mark" : "");
}
void TbLogStatsLocked(const char* why) {         // under s_tbMx
    const uint64_t v[12] = { s_tbStarts.load(), s_tbFaked.load(), s_tbBeginSkips.load(), s_tbBlocks.load(), s_tbToEnd.load(),
                             s_tbToWait.load(), s_tbFails.load(), s_tbArms.load(), s_tbSyncWaits.load(), s_tbBumps.load(),
                             s_tbBackJumps.load(), s_tbTagDrops.load() };
    uint64_t d[12];
    for (int k = 0; k < 12; ++k) { d[k] = v[k] - s_tb.statsPrev[k]; s_tb.statsPrev[k] = v[k]; }
    char ns[400] = "";
    int at = 0;
    uint64_t nsAll = 0;
    for (int k = 0; k < kNsCount && at < (int)sizeof(ns) - 64; ++k) {
        const uint64_t c = s_ns[k].load(std::memory_order_relaxed);
        nsAll += c;
        if (c) at += snprintf(ns + at, sizeof(ns) - (size_t)at, "%s%s %llu", at ? ", " : "", kNsName[k], (unsigned long long)c);
    }
    s_tb.nsPrev = nsAll;
    LogDbg("OpenXR turbo stats (%s): %llu async xrWaitFrame (%llu in all), %llu made-up frame states, %llu xrBeginFrame skipped, "
        "%llu second-poll blocks, 1-s timeouts %llu at xrEndFrame / %llu at xrWaitFrame, %llu failures | phase 33: %llu first waits "
        "armed at the game's xrBeginFrame, %llu turbo frames waited for at xrEndFrame (no wait pending), made-up time up to %.2f ms "
        "ahead of the newest real one, %llu nudged to stay increasing, %llu real times kept although earlier, %llu frame tags "
        "dropped | non-XR_SUCCESS results since the start: %s", why, (unsigned long long)d[0], (unsigned long long)v[0],
        (unsigned long long)d[1], (unsigned long long)d[2], (unsigned long long)d[3], (unsigned long long)d[4],
        (unsigned long long)d[5], (unsigned long long)d[6], (unsigned long long)d[7], (unsigned long long)d[8],
        (double)s_tb.aheadMaxNs / 1e6, (unsigned long long)d[9], (unsigned long long)d[10], (unsigned long long)d[11],
        at ? ns : "none");
    s_tb.aheadMaxNs = 0;
}
// Under s_tbMx: the on / off announcement (`where` = the call that noticed the switch).
void TbAnnounceLocked(int i, XrSession ses, bool want, const char* where) {
    if (want == s_tb.loggedOn) return;
    s_tb.loggedOn = want;
    if (want) {
        s_tb.broken = false;                     // a fresh switch-on re-arms a session turbo gave up on
        s_tb.brokenSes = nullptr;
        s_tbFailed.store(false, std::memory_order_relaxed);
        s_tb.statsNextNs = SteadyNs() + 30000000000LL;
        Log("OpenXR turbo: ON at %s (session %p, the %s %s, tid=%lu) -- the first async wait starts right after the game's next "
            "real xrBeginFrame (phase 33: never while a real game xrWaitFrame is outstanding); then the game no longer waits for "
            "the headset in xrWaitFrame; frame smoothing (ASW / Motion Smoothing / SSW / reprojection) stops working", where, ses,
            kKindName[s_slot[i].kind], s_slot[i].module, GetCurrentThreadId());
    } else {
        Log("OpenXR turbo: OFF at %s (session %p) -- %s; the game waits in its own xrWaitFrame again", where, ses,
            s_tb.pending ? "the pending async wait is consumed by the next turbo frame's xrEndFrame" : "no wait pending");
        TbLogStatsLocked("at switch-off");
    }
}
// Under s_tbMx: the first async wait, right after a real xrBeginFrame of the game succeeded (see the phase 33 notes above).
void TbArmLocked(int i, XrSession ses) {
    const bool want = s_tbWant.load(std::memory_order_relaxed);
    TbAnnounceLocked(i, ses, want, "xrBeginFrame");
    if (!want || s_tb.pending || (s_tb.broken && ses == s_tb.brokenSes) || s_tb.gameWaits != 0 || !TbSessionStateOk()) return;
    auto realWait = reinterpret_cast<PFN_xrWaitFrame>(s_slot[i].waitFrame.load(std::memory_order_acquire));
    auto realEnd = s_slot[i].endFrame.load(std::memory_order_acquire);
    if (!realWait || !realEnd) {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            LogErr("OpenXR turbo: xrWaitFrame / xrEndFrame not resolved at the %s %s (the component that begins the game's frame) "
                "-- turbo cannot run", kKindName[s_slot[i].kind], s_slot[i].module);
        }
        return;
    }
    if (!TbStartLocked(ses, realWait, false)) return;
    const uint64_t n = s_tbArms.fetch_add(1, std::memory_order_relaxed);
    if (n < 16)
        Log("OpenXR turbo: first async xrWaitFrame #%llu armed right after the game's real xrBeginFrame (session %p, tid=%lu; no "
            "real game xrWaitFrame outstanding) -- the frame begun now is passed through at its xrEndFrame, the game's next "
            "frame is a turbo frame", (unsigned long long)s_tb.seq, ses, GetCurrentThreadId());
}
// The game's xrEndFrame of a TURBO frame, BEFORE the real call (s_tbMx held by the caller): the pending wait is consumed and the
// real xrBeginFrame of this frame is called (the game's own was skipped); no wait pending: a real xrWaitFrame first.
void TbBeginForGameLocked(int i, XrSession ses) {
    XrResult wr = 0;
    if (s_tb.pending && ses == s_tb.ses) {
        const bool ready = TbWaitDone(s_tb.seq, 1000, &wr);
        if (ready) {
            s_tb.pending = false;
        } else {
            const uint64_t n = s_tbToEnd.fetch_add(1, std::memory_order_relaxed);
            if (n < 8) {
                char tb[400];
                TbMirrorLocked();
                TbDescribe(tb, sizeof(tb));
                LogWarn("OpenXR turbo: the async xrWaitFrame did not finish within 1 s at xrEndFrame -- the frame is begun and ended "
                    "anyway; the wait stays pending for the next turbo frame (phase 33: not expected any more -- the first wait "
                    "is armed only when no game wait is outstanding) | %s", tb);
            }
        }
        if (ready && wr < 0) {                   // no frame was waited for: no xrBeginFrame (the real xrEndFrame reports it)
            s_tbFails.fetch_add(1, std::memory_order_relaxed);
            s_tb.broken = true;
            s_tb.brokenSes = ses;
            s_tbFailed.store(true, std::memory_order_relaxed);
            LogErr("OpenXR turbo: the async xrWaitFrame FAILED (result %d = %s) -- turbo is off for this session (session %p)", wr,
                XrResultName(wr), ses);
            return;
        }
    } else {
        // phase 33: the game got a made-up frame state but no async wait is pending (turbo switched off / given up / a session
        // reset in between): the frame is waited for for real now (at most one frame period on this thread), then begun
        // (never while a real game xrWaitFrame is running or has waited a frame not yet begun: two waits outstanding -- and the
        // runtime would hold ours until the game begins its frame, which the game does only after this xrEndFrame = a hang;
        // then the frame is not begun: its xrEndFrame fails, logged as a non-success result)
        auto realWait = reinterpret_cast<PFN_xrWaitFrame>(s_slot[i].waitFrame.load(std::memory_order_acquire));
        if (!realWait) return;
        if (s_tb.gameWaits || s_tb.gameWaited) {
            const uint64_t n = s_tbSyncWaits.fetch_add(1, std::memory_order_relaxed);
            if (n < 8)
                LogWarn("OpenXR turbo: a turbo frame reached xrEndFrame with no async wait pending while the game has a real xrWaitFrame "
                    "%s -- the frame is NOT begun (its xrEndFrame will fail; a wait here could hang the game) (tid=%lu)",
                    s_tb.gameWaits ? "running" : "done and not begun", GetCurrentThreadId());
            return;
        }
        const XrFrameWaitInfo wi{ kTypeFrameWaitInfo, nullptr };
        XrFrameState fs{ kTypeFrameState, nullptr, 0, 0, 0 };
        ++t_depth;
        wr = realWait(ses, &wi, &fs);
        --t_depth;
        const uint64_t n = s_tbSyncWaits.fetch_add(1, std::memory_order_relaxed);
        if (n < 8)
            LogWarn("OpenXR turbo: a turbo frame reached xrEndFrame with no async wait pending (turbo %s) -- waited for it for real "
                "here (result %d = %s), then begun (tid=%lu)", s_tbWant.load(std::memory_order_relaxed) ? "on" : "switched off",
                wr, XrResultName(wr), GetCurrentThreadId());
        if (wr != 0) NoteResult(kNsTbSyncWait, wr, ses);
        if (wr < 0) return;                      // no frame waited: no xrBeginFrame (the real xrEndFrame reports it)
        TbWorker& w = Wk();
        std::lock_guard<std::mutex> wl(w.mx);
        w.lastTime = fs.predictedDisplayTime;
        w.lastPeriod = fs.predictedDisplayPeriod;
    }
    auto realBegin = reinterpret_cast<PFN_xrBeginFrame>(s_slot[i].beginFrame.load(std::memory_order_acquire));
    const XrFrameBeginInfo bi{ kTypeFrameBeginInfo, nullptr };
    ++t_depth;
    const XrResult rb = realBegin ? realBegin(ses, &bi) : -1;
    --t_depth;
    if (rb != 0) NoteResult(kNsTbBegin, rb, ses);
    if (rb < 0) {
        s_tbFails.fetch_add(1, std::memory_order_relaxed);
        s_tb.broken = true;
        s_tb.brokenSes = ses;
        s_tbFailed.store(true, std::memory_order_relaxed);
        LogErr("OpenXR turbo: the real xrBeginFrame FAILED (result %d = %s%s) -- turbo is off for this session (session %p)", rb,
            XrResultName(rb), realBegin ? "" : ", not resolved", ses);
    }
}
// ... AFTER the real xrEndFrame (result r; s_tbMx still held; tag = how the frame was begun): the on / off log, the stats
// line, the next async wait (turbo frames only: the first one is armed at the game's xrBeginFrame).
void TbAfterEndLocked(int i, XrSession ses, XrResult r, bool want, uint8_t tag) {
    TbAnnounceLocked(i, ses, want, "xrEndFrame");
    const int64_t now = SteadyNs();
    if (now >= s_tb.statsNextNs) {               // every 30 s while turbo is on or something new went wrong
        uint64_t nsAll = 0;
        for (int k = 0; k < kNsCount; ++k) nsAll += s_ns[k].load(std::memory_order_relaxed);
        if (want || s_tb.pending || nsAll != s_tb.nsPrev) TbLogStatsLocked("last 30 s");
        s_tb.statsNextNs = now + 30000000000LL;
    }
    // the next wait (turbo frames only): NEEDED when the game already has a made-up frame that no wait covers yet (its
    // xrWaitFrame came while the one just consumed was pending: the newest wait is made up, or a turbo frame is still begun) --
    // posted even with turbo switched off / given up / the session stopping (switched off: CLAIMED by that frame, so the game's
    // next xrWaitFrame waits for it to be consumed instead of being made up); else only while turbo is on (then the game's
    // next xrWaitFrame claims it). A switch-off thus ends cleanly: no made-up frame is ever left without a wait, and the game's
    // next real xrWaitFrame is the only wait outstanding.
    if (tag == kTagTurbo && !s_tb.pending && r >= 0) {
        bool needed = s_tb.lastWaitFake;
        for (int k = 0; k < s_tb.tagN && !needed; ++k) if (s_tb.tags[(s_tb.tagH + k) % kTags] == kTagTurbo) needed = true;
        const bool more = want && !(s_tb.broken && ses == s_tb.brokenSes) && s_tb.gameWaits == 0 && !s_tb.gameWaited &&
                          TbSessionStateOk();
        if (needed || more)
            if (auto realWait = reinterpret_cast<PFN_xrWaitFrame>(s_slot[i].waitFrame.load(std::memory_order_acquire)))
                TbStartLocked(ses, realWait, needed && !want);   // (turbo on: unclaimed, the phase 30 pipelining)
    }
    if (tag == kTagTurbo) s_tbCv.notify_all();   // a switch-off's xrWaitFrame waits for the consume (WaitFrameImpl)
    TbMirrorLocked();
}

// ---- phase 40: the hidden area mask (see xr_hook.h) -----------------------------------------------------------------------
std::atomic<bool>     s_hmWant{false};            // HiddenMaskRequest: ask for the extension at xrCreateInstance
std::atomic<bool>     s_hmActive{false};          // HiddenMaskActive: fetch the meshes
std::atomic<int>      s_hmExt{0};                 // HiddenStats::ext
std::atomic<bool>     s_hmOurs{false};            // the extension is enabled by us only: its events are swallowed
std::atomic<bool>     s_hmDirty[2] = {};          // re-query view v at the next xrEndFrame (event / new session)
std::atomic<uint64_t> s_hmQueries{0}, s_hmEvents{0}, s_hmSwallowed{0};
std::atomic<bool>     s_hmFnOk{false}, s_hmFailedA{false};
struct HmView {
    uint64_t gen = 0;
    uint32_t nv = 0, ni = 0;
    float    xy[2 * kHiddenMaxVerts] = {};
    uint32_t idx[kHiddenMaxIdx] = {};
    float    fov[4] = {};
    bool     have = false;
};
HmView   s_hmView[2];
uint64_t s_hmGenNext = 0;
SRWLOCK  s_hmLock = SRWLOCK_INIT;                 // s_hmView + s_hmGenNext (copies only)
// the query side: the game's outermost xrEndFrame (s_hmMx; never held over a game call)
std::mutex                 s_hmMx;
XrSession                  s_hmSes = nullptr;     // the session the meshes belong to
PFN_xrGetVisibilityMaskKHR s_hmFn = nullptr;
int                        s_hmFnSlot = -1;       // the component it was resolved at
uint32_t                   s_hmFnTried = 0;       // bit per slot: resolution tried
bool                       s_hmFailed = false;    // a query failed for s_hmSes: given up until the next session
int                        s_hmTries[2] = {};     // size-mismatch retries of the current dirty query
XrVector2f                 s_hmVb[kHiddenMaxVerts];
uint32_t                   s_hmIb[kHiddenMaxIdx];

// under s_hmMx: xrGetVisibilityMaskKHR through component k's real GIPA (+ its last instance). False = not resolvable there.
bool HmResolveAt(int k) {
    if (k < 0 || k >= s_slotN.load(std::memory_order_acquire) || (s_hmFnTried & (1u << k))) return false;
    s_hmFnTried |= 1u << k;
    auto gipa = reinterpret_cast<PFN_xrGetInstanceProcAddr>(s_slot[k].gipa.load(std::memory_order_acquire));
    XrInstance inst = s_slot[k].instance.load(std::memory_order_acquire);
    if (!gipa || !inst) return false;
    PFN_xrVoidFunction fp = nullptr;
    ++t_depth;
    const XrResult r = gipa(inst, "xrGetVisibilityMaskKHR", &fp);
    --t_depth;
    if (r < 0 || !fp) {
        LogDbg("hidden area mask: xrGetVisibilityMaskKHR not resolved at the %s %s (result %d = %s)", kKindName[s_slot[k].kind],
            s_slot[k].module, r, XrResultName(r));
        return false;
    }
    s_hmFn = reinterpret_cast<PFN_xrGetVisibilityMaskKHR>(fp);
    s_hmFnSlot = k;
    s_hmFnOk.store(true, std::memory_order_relaxed);
    Log("hidden area mask: xrGetVisibilityMaskKHR resolved at the %s %s (slot %d, instance %p)", kKindName[s_slot[k].kind],
        s_slot[k].module, k, inst);
    return true;
}
// under s_hmMx: the next component to try after slot `from` failed: the runtime (the extension is enabled there for sure)
bool HmResolveRuntime() {
    const int n = s_slotN.load(std::memory_order_acquire);
    for (int k = 0; k < n; ++k)
        if (s_slot[k].kind == kKindRuntime && HmResolveAt(k)) return true;
    return false;
}
// under s_hmMx, t_depth 0: one view's hidden mesh into s_hmView[v]. 1 = done (mesh or empty), 0 = retry next frame, -1 = failed.
int HmQuery(XrSession ses, int v, const XrFovf& fov) {
    XrVisibilityMaskKHR m{};
    m.type = kTypeVisibilityMaskKhr;
    ++t_depth;
    XrResult r = s_hmFn(ses, kViewConfigPrimaryStereo, (uint32_t)v, kMaskHiddenTriangleMesh, &m);
    --t_depth;
    if (r < 0) {
        LogErr("hidden area mask: xrGetVisibilityMaskKHR(view %d, count) FAILED -> %d = %s (at the %s %s) -- no mask", v, r,
            XrResultName(r), kKindName[s_slot[s_hmFnSlot].kind], s_slot[s_hmFnSlot].module);
        return -1;
    }
    const uint32_t nv = m.vertexCountOutput, ni = m.indexCountOutput;
    if (nv > kHiddenMaxVerts || ni > kHiddenMaxIdx || ni % 3 != 0 || (nv == 0) != (ni == 0)) {
        LogErr("hidden area mask: view %d mesh not usable (%u vertices, %u indices; at most %u / %u, indices a multiple of 3) -- no "
            "mask", v, nv, ni, kHiddenMaxVerts, kHiddenMaxIdx);
        return -1;
    }
    if (nv) {
        m.vertexCapacityInput = nv; m.vertices = s_hmVb;
        m.indexCapacityInput = ni; m.indices = s_hmIb;
        m.vertexCountOutput = m.indexCountOutput = 0;
        ++t_depth;
        r = s_hmFn(ses, kViewConfigPrimaryStereo, (uint32_t)v, kMaskHiddenTriangleMesh, &m);
        --t_depth;
        if (r < 0 || m.vertexCountOutput != nv || m.indexCountOutput != ni) {
            if (++s_hmTries[v] <= 3) return 0;          // (the mesh changed between the two calls: again next frame)
            LogErr("hidden area mask: xrGetVisibilityMaskKHR(view %d, fill) FAILED -> %d = %s (%u / %u of %u / %u) -- no mask", v, r,
                XrResultName(r), m.vertexCountOutput, m.indexCountOutput, nv, ni);
            return -1;
        }
        float mn[2] = { 1e30f, 1e30f }, mx[2] = { -1e30f, -1e30f };
        for (uint32_t k = 0; k < nv; ++k) {
            const XrVector2f& p = s_hmVb[k];
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
                LogErr("hidden area mask: view %d vertex %u is not finite -- no mask", v, k);
                return -1;
            }
            mn[0] = (std::min)(mn[0], p.x); mx[0] = (std::max)(mx[0], p.x);
            mn[1] = (std::min)(mn[1], p.y); mx[1] = (std::max)(mx[1], p.y);
        }
        for (uint32_t k = 0; k < ni; ++k)
            if (s_hmIb[k] >= nv) {
                LogErr("hidden area mask: view %d index %u = %u is out of range (%u vertices) -- no mask", v, k, s_hmIb[k], nv);
                return -1;
            }
        LogDbg("hidden area mask: view %d hidden triangle mesh %u vertices, %u triangles; raw bounds x %+.3f..%+.3f y %+.3f..%+.3f "
            "(view tangents; the submitted fov tangents are x %+.3f..%+.3f y %+.3f..%+.3f) -- session %p", v, nv, ni / 3,
            mn[0], mx[0], mn[1], mx[1], std::tan(fov.angleLeft), std::tan(fov.angleRight), std::tan(fov.angleDown),
            std::tan(fov.angleUp), ses);
    } else {
        LogDbg("hidden area mask: view %d -- the runtime reports NO hidden area (empty mesh), session %p", v, ses);
    }
    AcquireSRWLockExclusive(&s_hmLock);
    HmView& h = s_hmView[v];
    h.nv = nv; h.ni = ni;
    if (nv) { memcpy(h.xy, s_hmVb, sizeof(float) * 2 * nv); memcpy(h.idx, s_hmIb, sizeof(uint32_t) * ni); }
    h.fov[0] = fov.angleLeft; h.fov[1] = fov.angleRight; h.fov[2] = fov.angleUp; h.fov[3] = fov.angleDown;
    h.have = true;
    h.gen = ++s_hmGenNext;
    ReleaseSRWLockExclusive(&s_hmLock);
    s_hmQueries.fetch_add(1, std::memory_order_relaxed);
    s_hmTries[v] = 0;
    return 1;
}
// under s_hmMx: the meshes of the old session go (an empty mesh with a new gen: the drawer drops its copy)
void HmClearLocked() {
    AcquireSRWLockExclusive(&s_hmLock);
    for (HmView& h : s_hmView) { if (h.have || h.nv) { h.have = false; h.nv = h.ni = 0; h.gen = ++s_hmGenNext; } }
    ReleaseSRWLockExclusive(&s_hmLock);
}
// The game's outermost xrEndFrame with its first stereo projection layer (slot i = the component it went to; t_depth 0):
// the meshes for this session (once, and again after a mask-changed event) + the submitted fov of each view.
void HmEndFrame(int i, XrSession ses, const XrCompositionLayerProjection* proj) {
    if (!s_hmActive.load(std::memory_order_relaxed)) return;
    const int ext = s_hmExt.load(std::memory_order_relaxed);
    if (ext != 2 && ext != 3) return;
    std::lock_guard<std::mutex> lk(s_hmMx);
    if (ses != s_hmSes) {
        if (s_hmSes) HmClearLocked();
        s_hmSes = ses;
        s_hmFailed = false;
        s_hmFailedA.store(false, std::memory_order_relaxed);
        s_hmDirty[0].store(true, std::memory_order_relaxed);
        s_hmDirty[1].store(true, std::memory_order_relaxed);
        s_hmTries[0] = s_hmTries[1] = 0;
    }
    if (!s_hmFailed && (s_hmDirty[0].load(std::memory_order_relaxed) || s_hmDirty[1].load(std::memory_order_relaxed))) {
        if (!s_hmFn && !HmResolveAt(i) && !HmResolveRuntime()) {
            s_hmFailed = true;
            s_hmFailedA.store(true, std::memory_order_relaxed);
            LogErr("hidden area mask: xrGetVisibilityMaskKHR could not be resolved (extension state %d) -- no mask", ext);
        }
        for (int v = 0; v < 2 && !s_hmFailed; ++v) {
            if (!s_hmDirty[v].load(std::memory_order_relaxed)) continue;
            int q = HmQuery(ses, v, proj->views[v].fov);
            if (q < 0 && s_hmFnSlot >= 0 && s_slot[s_hmFnSlot].kind != kKindRuntime) {
                // a layer above the runtime refused it (it may not know the extension we enabled below it): straight to the runtime
                const int was = s_hmFnSlot;
                s_hmFn = nullptr; s_hmFnSlot = -1;
                if (HmResolveRuntime()) {
                    LogWarn("hidden area mask: the query failed through the %s %s -- trying the runtime directly", kKindName[s_slot[was].kind],
                        s_slot[was].module);
                    q = HmQuery(ses, v, proj->views[v].fov);
                }
            }
            if (q < 0) { s_hmFailed = true; s_hmFailedA.store(true, std::memory_order_relaxed); HmClearLocked(); break; }
            if (q > 0) s_hmDirty[v].store(false, std::memory_order_relaxed);
        }
    }
    // the submitted fov may change without a new mesh (another FOV setting): a new gen, the drawer maps it again
    AcquireSRWLockExclusive(&s_hmLock);
    for (int v = 0; v < 2; ++v) {
        HmView& h = s_hmView[v];
        if (!h.have) continue;
        const XrFovf& f = proj->views[v].fov;
        if (std::fabs(h.fov[0] - f.angleLeft) > 1e-5f || std::fabs(h.fov[1] - f.angleRight) > 1e-5f ||
            std::fabs(h.fov[2] - f.angleUp) > 1e-5f || std::fabs(h.fov[3] - f.angleDown) > 1e-5f) {
            h.fov[0] = f.angleLeft; h.fov[1] = f.angleRight; h.fov[2] = f.angleUp; h.fov[3] = f.angleDown;
            h.gen = ++s_hmGenNext;
        }
    }
    ReleaseSRWLockExclusive(&s_hmLock);
}
// The game's xrDestroySession (outermost): that session's meshes go.
void HmDestroySession(XrSession ses) {
    std::lock_guard<std::mutex> lk(s_hmMx);
    if (!ses || ses != s_hmSes) return;
    HmClearLocked();
    s_hmSes = nullptr;
    s_hmFailed = false;
}
// The runtime's xrCreateInstance (wrapped at runtime slots only; the loader's own call, layers or not): + XR_KHR_visibility_mask.
XrResult CreateInstanceImpl(int i, const XrInstanceCreateInfo* ci, XrInstance* out) {
    auto real = reinterpret_cast<PFN_xrCreateInstance>(s_slot[i].createInstance.load(std::memory_order_acquire));
    if (!real) return -1;
    auto pass = [&]() { ++t_depth; const XrResult r = real(ci, out); --t_depth; return r; };
    if (!s_hmWant.load(std::memory_order_relaxed) || !ci || ci->type != kTypeInstanceCreateInfo) return pass();
    for (uint32_t k = 0; k < ci->enabledExtensionCount; ++k)
        if (ci->enabledExtensionNames && ci->enabledExtensionNames[k] && !strcmp(ci->enabledExtensionNames[k], kVisMaskExt)) {
            s_hmExt.store(2, std::memory_order_relaxed);
            s_hmOurs.store(false, std::memory_order_relaxed);
            Log("hidden area mask: the game enables %s itself (xrCreateInstance at the runtime %s)", kVisMaskExt, s_slot[i].module);
            return pass();
        }
    // does the runtime offer it? (its own xrEnumerateInstanceExtensionProperties, no layer name)
    bool offered = false;
    auto gipa = reinterpret_cast<PFN_xrGetInstanceProcAddr>(s_slot[i].gipa.load(std::memory_order_acquire));
    PFN_xrVoidFunction fp = nullptr;
    ++t_depth;
    XrResult r = gipa ? gipa(nullptr, "xrEnumerateInstanceExtensionProperties", &fp) : -1;
    if (r >= 0 && fp) {
        auto en = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(fp);
        uint32_t n = 0;
        r = en(nullptr, 0, &n, nullptr);
        if (r >= 0 && n > 0 && n < 4096) {
            XrExtensionProperties* props = new (std::nothrow) XrExtensionProperties[n];
            if (props) {
                for (uint32_t k = 0; k < n; ++k) { props[k] = XrExtensionProperties{}; props[k].type = kTypeExtensionProperties; }
                uint32_t got = 0;
                r = en(nullptr, n, &got, props);
                if (r >= 0)
                    for (uint32_t k = 0; k < got && k < n; ++k) {
                        props[k].extensionName[127] = 0;
                        if (!strcmp(props[k].extensionName, kVisMaskExt)) offered = true;
                    }
                delete[] props;
            }
        }
    }
    --t_depth;
    if (!offered) {
        s_hmExt.store(1, std::memory_order_relaxed);
        LogWarn("hidden area mask: the OpenXR runtime %s does not offer %s (enumerate result %d) -- the hidden area mask is not "
            "available", s_slot[i].module, kVisMaskExt, r);
        return pass();
    }
    // a COPY of the create info with the extension appended (the game's array stays as it is)
    const uint32_t n0 = ci->enabledExtensionCount;
    const char** names = new (std::nothrow) const char*[n0 + 1];
    if (!names) return pass();
    for (uint32_t k = 0; k < n0; ++k) names[k] = ci->enabledExtensionNames[k];
    names[n0] = kVisMaskExt;
    XrInstanceCreateInfo c = *ci;
    c.enabledExtensionCount = n0 + 1;
    c.enabledExtensionNames = names;
    ++t_depth;
    r = real(&c, out);
    --t_depth;
    delete[] names;
    if (r >= 0) {
        s_hmExt.store(3, std::memory_order_relaxed);
        s_hmOurs.store(true, std::memory_order_relaxed);
        Log("hidden area mask: %s enabled at the runtime %s's xrCreateInstance (the game asked for %u extension(s); instance %p) -- "
            "the meshes are fetched at the game's xrEndFrame while xr_hidden_mask is on", kVisMaskExt, s_slot[i].module, n0,
            out ? *out : nullptr);
        return r;
    }
    s_hmExt.store(4, std::memory_order_relaxed);
    LogErr("hidden area mask: xrCreateInstance with %s FAILED (%d = %s) -- created again with the game's own extensions, no mask",
        kVisMaskExt, r, XrResultName(r));
    return pass();
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
    CPU_HOOK(kHkXrAcquire);                                    // phase 31 CPU profiler (cpu_prof.h)
    const bool outer = t_depth == 0;
    ++t_depth;
    XrResult r;
    CPU_ORIG(r = real(sc, info, index));
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
    CPU_HOOK(kHkXrRelease);                                    // phase 31 CPU profiler (cpu_prof.h)
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
    XrResult r;
    CPU_ORIG(r = real(sc, info));
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
    CPU_HOOK(kHkXrLocate);                                    // phase 31 CPU profiler (cpu_prof.h)
    const bool outer = t_depth == 0;
    ++t_depth;
    XrResult r;
    CPU_ORIG(r = real(ses, li, vs, cap, cnt, views));
    --t_depth;
    if (outer && r != 0) NoteResult(kNsLocate, r, ses);   // phase 33
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
// phase 30 (Turbo mode, ported from OpenXR Toolkit -- see the MIT notice above TbWorker): the game's xrWaitFrame. While an async
// wait is pending: a made-up frame state (a second poll first blocks on that wait, up to 1 s). Else the real call, its
// predicted time / period kept for the switch into turbo. Every time handed out is kept increasing (phase 33: a real time is
// nudged only when it is within 2 frame periods of the last one handed out; a made-up one is at most 2 periods ahead).
XrResult WaitFrameImpl(int i, XrSession ses, const XrFrameWaitInfo* wi, XrFrameState* fs) {
    auto real = reinterpret_cast<PFN_xrWaitFrame>(s_slot[i].waitFrame.load(std::memory_order_acquire));
    if (!real) return -1;
    CPU_HOOK(kHkXrWait);                                    // phase 31 CPU profiler (cpu_prof.h)
    if (t_depth > 0) {                           // an inner layer's / our call: straight through
        ++t_depth;
        XrResult r;
        CPU_ORIG(r = real(ses, wi, fs));
        --t_depth;
        return r;
    }
    CpuProf::SetRole(CpuProf::kRoleXrWait);      // phase 33: the CPU profiler's "main" thread label
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true))
        Log("OpenXR hook: first xrWaitFrame by the game (session %p; via the %s %s, tid=%lu)", ses, kKindName[s_slot[i].kind],
            s_slot[i].module, GetCurrentThreadId());
    // phase 52: the VR FPS cap -- sleep (no lock held) until the target, then the call goes on exactly as without the cap.
    // Turbo wanted: no cap (turbo's made-up frames must not wait; inject.cpp keeps turbo off while the cap is on).
    const int capDiv = s_capDiv.load(std::memory_order_relaxed);
    const bool turboWant = s_tbWant.load(std::memory_order_relaxed);
    if (capDiv != s_capAnnouncedDiv && (!capDiv || s_capBase > 0)) {   // (on: once the display period is known)
        s_capAnnouncedDiv = capDiv;
        if (capDiv)
            Log("VR FPS cap (phase 52): 1/%d of the headset rate (vr_fps_cap=%d; display period %.2f ms -> %.1f fps)%s -- the game's "
                "xrWaitFrame sleeps until a quarter period before every %s display slot; the runtime's reprojection fills the slots "
                "in between (session %p)", capDiv, capDiv, (double)s_capBase / 1e6,
                s_capBase > 0 ? 1e9 / (double)s_capBase / capDiv : 0.0,
                turboWant ? " -- NOT pacing while Turbo mode is on" : "", capDiv == 2 ? "2nd" : "3rd", ses);
        else
            Log("VR FPS cap (phase 52): off (vr_fps_cap=0) -- the game's xrWaitFrame is not delayed (session %p)", ses);
    }
    if (capDiv >= 2 && !turboWant && fs) {
        const int64_t target = FramePace::VrWaitTarget(s_capLastReturn, s_capBase, capDiv);
        const int64_t t0 = FramePace::NowNs();
        // (a target more than div periods ahead cannot happen with a sane clock: never sleep longer than that)
        if (target > t0 && target - t0 <= (int64_t)capDiv * s_capBase) {
            int64_t slept;
            CPU_ORIG(slept = s_capSleeper.Until(target, 0));   // phase 31: a wait for the headset slot, not mod CPU
            s_capSleepNs.fetch_add(slept, std::memory_order_relaxed);
            s_capSleeps.fetch_add(1, std::memory_order_relaxed);
        }
    }
    s_gameWaitNsA.store(SteadyNs(), std::memory_order_relaxed);   // phase 52: FrameLoopActive
    const int64_t now = SteadyNs();
    std::unique_lock<std::mutex> lk(s_tbMx);
    const int64_t prev = s_tb.lastGameWaitNs;
    s_tb.lastGameWaitNs = now;
    if (s_tb.pending && fs && ses != s_tb.ses) {
        static std::atomic<int> s_other{0};
        if (s_other.fetch_add(1, std::memory_order_relaxed) < 4)
            LogWarn("OpenXR turbo: xrWaitFrame of session %p while an async wait of session %p is pending -- passed to the runtime",
                ses, s_tb.ses);
    }
    // phase 33: turbo switched off while the pending wait is already claimed by a made-up frame: this frame must not be made
    // up (nothing would wait for it) -- wait (<= 1 s) until that frame's xrEndFrame consumes the wait: then either no new one
    // is posted (turbo still off: wait for real below, the only xrWaitFrame outstanding) or turbo came back on and a new,
    // unclaimed one was posted (made up from it below)
    if (s_tb.pending && ses == s_tb.ses && fs && s_tb.polled && !s_tbWant.load(std::memory_order_relaxed)) {
        const uint64_t seq0 = s_tb.seq;
        bool freed;
        CPU_ORIG(freed = s_tbCv.wait_for(lk, std::chrono::milliseconds(1000), [seq0] { return !s_tb.pending || s_tb.seq != seq0; }));
        if (!freed) {
            const uint64_t n = s_tbToWait.fetch_add(1, std::memory_order_relaxed);
            if (n < 8) {
                char tb[400];
                TbMirrorLocked();
                TbDescribe(tb, sizeof(tb));
                LogWarn("OpenXR turbo: switched off, but the pending async wait was not consumed within 1 s at the game's xrWaitFrame "
                    "-- a made-up frame state is returned once more | %s", tb);
            }
        }
    }
    if (s_tb.pending && ses == s_tb.ses && fs) {
        if (s_tb.polled) {                       // pipelining of exactly one frame: the second poll waits
            s_tbBlocks.fetch_add(1, std::memory_order_relaxed);
            bool waited;
            CPU_ORIG(waited = TbWaitDone(s_tb.seq, 1000, nullptr));   // phase 31: a wait for the headset, not mod CPU
            if (!waited) {
                const uint64_t n = s_tbToWait.fetch_add(1, std::memory_order_relaxed);
                if (n < 8) {
                    char tb[400];
                    TbMirrorLocked();
                    TbDescribe(tb, sizeof(tb));
                    LogWarn("OpenXR turbo: the async xrWaitFrame did not finish within 1 s at the game's second xrWaitFrame -- a "
                        "made-up frame state is returned anyway | %s", tb);
                }
            }
        }
        s_tb.polled = true;
        TbWorker& w = Wk();
        bool done;
        XrTime t;
        XrDuration per;
        {
            std::lock_guard<std::mutex> wl(w.mx);
            done = w.doneSeq >= s_tb.seq;
            t = w.lastTime;
            per = w.lastPeriod;
        }
        const XrTime newest = t;
        if (!done && prev) {                     // not waited yet: the last time + the time since the previous game wait
            int64_t dt = now - prev;             // (phase 33: at most 2 frame periods -- a stall must not push the time ahead)
            const int64_t cap = per > 0 ? 2 * per : 50000000;
            if (dt > cap) dt = cap;
            if (dt > 0) t += dt;
        }
        if (t <= s_tb.lastReturned) { t = s_tb.lastReturned + 1; s_tbBumps.fetch_add(1, std::memory_order_relaxed); }
        if (t - newest > s_tb.aheadMaxNs) s_tb.aheadMaxNs = t - newest;
        s_tb.lastReturned = t;
        fs->predictedDisplayTime = t;
        fs->predictedDisplayPeriod = per;
        fs->shouldRender = 1;
        s_tb.lastWaitFake = true;                // phase 33: its xrBeginFrame is skipped (a TURBO frame)
        TbMirrorLocked();
        s_tbFaked.fetch_add(1, std::memory_order_relaxed);
        return 0;                                // XR_SUCCESS
    }
    ++s_tb.gameWaits;                            // phase 33: no async wait may start while this one runs
    TbMirrorLocked();
    lk.unlock();
    ++t_depth;
    XrResult r;
    CPU_ORIG(r = real(ses, wi, fs));
    --t_depth;
    if (r >= 0 && fs) {                          // phase 52: the VR FPS cap's anchor (the runtime woke the game for a slot now)
        const int64_t ret = FramePace::NowNs();
        if (ret - s_capLastReturn > 2000000000LL) s_capBase = 0;   // a gap (new session, maybe another rate): re-seed the period
        s_capBase = FramePace::VrBasePeriod(s_capBase, (int64_t)fs->predictedDisplayPeriod);
        s_capLastReturn = ret;
        s_capBaseA.store(s_capBase, std::memory_order_relaxed);
        s_capWaits.fetch_add(1, std::memory_order_relaxed);
    }
    lk.lock();
    --s_tb.gameWaits;
    s_tb.gameWaited = r >= 0;                    // (until the game's xrBeginFrame)
    s_tb.lastWaitFake = false;
    if (r >= 0 && fs) {
        TbWorker& w = Wk();
        {
            std::lock_guard<std::mutex> wl(w.mx);  // always kept: the switch into turbo starts from it
            w.lastTime = fs->predictedDisplayTime;
            w.lastPeriod = fs->predictedDisplayPeriod;
        }
        if (fs->predictedDisplayTime <= s_tb.lastReturned) {
            const int64_t back = s_tb.lastReturned - fs->predictedDisplayTime;
            const int64_t tol = fs->predictedDisplayPeriod > 0 ? 2 * fs->predictedDisplayPeriod : 50000000;
            if (back < tol) {
                fs->predictedDisplayTime = s_tb.lastReturned + 1;
                s_tbBumps.fetch_add(1, std::memory_order_relaxed);
            } else {
                const uint64_t n = s_tbBackJumps.fetch_add(1, std::memory_order_relaxed);
                if (n < 8)
                    LogWarn("OpenXR turbo: the runtime's predicted display time is %.2f ms EARLIER than the last one handed to the game "
                        "-- kept as it is (phase 33: only nudged within 2 frame periods)", (double)back / 1e6);
            }
        }
        s_tb.lastReturned = fs->predictedDisplayTime;
    }
    TbMirrorLocked();
    lk.unlock();
    if (r != 0) NoteResult(kNsWait, r, ses);     // phase 33
    return r;
}
// phase 30: the game's xrBeginFrame -- skipped (XR_SUCCESS) after a made-up frame state (xrEndFrame begins the frame). phase 33:
// every game frame is tagged (REAL / TURBO) here for its xrEndFrame, and the first async wait is armed right after a real one.
XrResult BeginFrameImpl(int i, XrSession ses, const XrFrameBeginInfo* bi) {
    auto real = reinterpret_cast<PFN_xrBeginFrame>(s_slot[i].beginFrame.load(std::memory_order_acquire));
    if (!real) return -1;
    CPU_HOOK(kHkXrBegin);                                    // phase 31 CPU profiler (cpu_prof.h)
    if (t_depth > 0) {
        ++t_depth;
        XrResult r;
        CPU_ORIG(r = real(ses, bi));
        --t_depth;
        return r;
    }
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true))
        Log("OpenXR hook: first xrBeginFrame by the game (session %p; via the %s %s, tid=%lu)", ses, kKindName[s_slot[i].kind],
            s_slot[i].module, GetCurrentThreadId());
    std::unique_lock<std::mutex> lk(s_tbMx);     // (held over the real call: the frame tags stay in call order)
    if (s_tb.lastWaitFake) {
        s_tb.lastWaitFake = false;
        TbPushTagLocked(kTagTurbo);
        TbMirrorLocked();
        s_tbBeginSkips.fetch_add(1, std::memory_order_relaxed);
        return 0;                                // XR_SUCCESS
    }
    ++t_depth;
    XrResult r;
    CPU_ORIG(r = real(ses, bi));
    --t_depth;
    s_tb.gameWaited = false;
    if (r == 9) s_tb.tagN = 0;                   // XR_FRAME_DISCARDED: the runtime dropped the frame still open (its tag too)
    TbPushTagLocked(kTagReal);
    if (r >= 0) TbArmLocked(i, ses);
    TbMirrorLocked();
    lk.unlock();
    if (r != 0) NoteResult(kNsBegin, r, ses);    // phase 33
    return r;
}
// phase 30: the game's xrEndSession -- a pending async wait is drained first (the next session run starts with a real wait).
// phase 33: + the lifecycle line (who started it, the turbo state).
XrResult EndSessionImpl(int i, XrSession ses) {
    auto real = reinterpret_cast<PFN_xrEndSession>(s_slot[i].endSession.load(std::memory_order_acquire));
    if (!real) return -1;
    CPU_HOOK(kHkXrOther);
    const bool outer = t_depth == 0;
    char tb[400] = "", who[300] = "";
    if (outer) {
        WhoStarted(who, sizeof(who));
        TbSessionReset(ses, true, tb, sizeof(tb));
    }
    ++t_depth;
    XrResult r;
    CPU_ORIG(r = real(ses));
    --t_depth;
    if (outer) {
        s_endCalled.store(true, std::memory_order_relaxed);
        if (s_sesLogs.fetch_add(1, std::memory_order_relaxed) < kSesLogCap)
            Log("OpenXR session: xrEndSession(%p) by the game -> %d = %s (tid=%lu) %s | %s", ses, r, XrResultName(r),
                GetCurrentThreadId(), who, tb);
    }
    return r;
}
// phase 33: xrCreateSession / xrBeginSession / xrRequestExitSession -- logged (with the turbo state); xrBeginSession also starts
// a fresh lifecycle record (who-started marks) and clears the turbo frame tags.
XrResult CreateSessionImpl(int i, XrInstance inst, const void* ci, XrSession* out) {
    auto real = reinterpret_cast<PFN_xrCreateSession>(s_slot[i].createSession.load(std::memory_order_acquire));
    if (!real) return -1;
    CPU_HOOK(kHkXrOther);
    const bool outer = t_depth == 0;
    ++t_depth;
    XrResult r;
    CPU_ORIG(r = real(inst, ci, out));
    --t_depth;
    if (outer) {
        s_sesState.store(0, std::memory_order_relaxed);
        s_rtStop.store(0, std::memory_order_relaxed);
        s_instLoss.store(false, std::memory_order_relaxed);
        s_exitAsked.store(false, std::memory_order_relaxed);
        s_endCalled.store(false, std::memory_order_relaxed);
        char tb[400];
        TbDescribe(tb, sizeof(tb));
        if (s_sesLogs.fetch_add(1, std::memory_order_relaxed) < kSesLogCap)
            Log("OpenXR session: xrCreateSession(instance %p) by the game -> %d = %s, session %p (via the %s %s, tid=%lu) | %s", inst,
                r, XrResultName(r), (r >= 0 && out) ? *out : nullptr, kKindName[s_slot[i].kind], s_slot[i].module,
                GetCurrentThreadId(), tb);
    }
    return r;
}
XrResult BeginSessionImpl(int i, XrSession ses, const XrSessionBeginInfo* bi) {
    auto real = reinterpret_cast<PFN_xrBeginSession>(s_slot[i].beginSession.load(std::memory_order_acquire));
    if (!real) return -1;
    CPU_HOOK(kHkXrOther);
    const bool outer = t_depth == 0;
    char tb[400] = "";
    if (outer) TbSessionReset(ses, true, tb, sizeof(tb));
    ++t_depth;
    XrResult r;
    CPU_ORIG(r = real(ses, bi));
    --t_depth;
    if (outer) {
        const int prevState = s_sesState.load(std::memory_order_relaxed);
        if (r >= 0) {                            // a new run of the session: fresh who-started marks
            s_rtStop.store(0, std::memory_order_relaxed);
            s_instLoss.store(false, std::memory_order_relaxed);
            s_exitAsked.store(false, std::memory_order_relaxed);
            s_endCalled.store(false, std::memory_order_relaxed);
        }
        if (s_sesLogs.fetch_add(1, std::memory_order_relaxed) < kSesLogCap)
            Log("OpenXR session: xrBeginSession(%p, view configuration %d) by the game -> %d = %s (tid=%lu; session state %s) | %s",
                ses, bi ? bi->primaryViewConfigurationType : -1, r, XrResultName(r), GetCurrentThreadId(),
                SessionStateName(prevState), tb);
    }
    return r;
}
XrResult RequestExitSessionImpl(int i, XrSession ses) {
    auto real = reinterpret_cast<PFN_xrRequestExitSession>(s_slot[i].requestExit.load(std::memory_order_acquire));
    if (!real) return -1;
    CPU_HOOK(kHkXrOther);
    const bool outer = t_depth == 0;
    ++t_depth;
    XrResult r;
    CPU_ORIG(r = real(ses));
    --t_depth;
    if (outer) {
        s_exitAsked.store(true, std::memory_order_relaxed);
        char tb[400];
        TbDescribe(tb, sizeof(tb));
        if (s_sesLogs.fetch_add(1, std::memory_order_relaxed) < kSesLogCap)
            Log("OpenXR session: xrRequestExitSession(%p) by the GAME -> %d = %s (tid=%lu; session state %s) -- the game itself asks "
                "the runtime to end the session | %s", ses, r, XrResultName(r), GetCurrentThreadId(),
                SessionStateName(s_sesState.load(std::memory_order_relaxed)), tb);
    }
    return r;
}
// phase 33: xrPollEvent -- every event handed to the game is logged by name (session state changes always, up to kSesLogCap
// lines; the others at most 20 lines a second, the rest counted and reported in the next line).
void LogEvent(const void* ev) {
    const int32_t type = *static_cast<const int32_t*>(ev);
    static std::atomic<uint64_t> s_n{0};
    static std::atomic<int64_t>  s_sec{0};
    static std::atomic<int>      s_inSec{0};
    static std::atomic<uint64_t> s_dropped{0};
    const uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    const int64_t now = SteadyNs();
    char tb[400];
    if (type == kTypeEvSessionStateChanged) {
        XrEvSessionStateChanged e;
        memcpy(&e, ev, sizeof(e));
        const int prev = s_sesState.exchange(e.state, std::memory_order_relaxed);
        s_sesStateNs.store(now, std::memory_order_relaxed);
        const bool stop = e.state >= 6 && e.state <= 8;     // STOPPING / LOSS_PENDING / EXITING
        if (stop && e.state > s_rtStop.load(std::memory_order_relaxed)) {
            s_rtStop.store(e.state, std::memory_order_relaxed);
            s_rtStopNs.store(now, std::memory_order_relaxed);
        }
        TbDescribe(tb, sizeof(tb));
        if (s_sesLogs.fetch_add(1, std::memory_order_relaxed) < kSesLogCap)
            Log("OpenXR event #%llu: SESSION_STATE_CHANGED session %p %s -> %s (%d) at XrTime %lld (tid=%lu)%s | %s",
                (unsigned long long)n, e.session, SessionStateName(prev), SessionStateName(e.state), e.state, (long long)e.time,
                GetCurrentThreadId(),
                e.state == 6 ? " -- the RUNTIME asks the game to end the session (xrEndSession expected)"
                : e.state == 7 ? " -- the RUNTIME is losing the session (the game must destroy it)"
                : e.state == 8 ? " -- the RUNTIME wants the application to exit / leave VR"
                : e.state == 1 && prev >= 6 ? " -- the session has ended" : "", tb);
        return;
    }
    if (type == kTypeEvInstanceLossPending) s_instLoss.store(true, std::memory_order_relaxed);
    const int64_t sec = now / 1000000000LL;
    if (s_sec.exchange(sec, std::memory_order_relaxed) != sec) s_inSec.store(0, std::memory_order_relaxed);
    if (s_inSec.fetch_add(1, std::memory_order_relaxed) >= 20) { s_dropped.fetch_add(1, std::memory_order_relaxed); return; }
    const uint64_t dropped = s_dropped.exchange(0, std::memory_order_relaxed);
    char what[256] = "";
    switch (type) {
    case kTypeEvInstanceLossPending: {
        XrEvInstanceLossPending e; memcpy(&e, ev, sizeof(e));
        snprintf(what, sizeof(what), " at XrTime %lld -- the RUNTIME is losing the whole OpenXR instance", (long long)e.lossTime);
        break; }
    case kTypeEvRefSpaceChangePending: {
        XrEvRefSpaceChangePending e; memcpy(&e, ev, sizeof(e));
        snprintf(what, sizeof(what), " session %p, space type %d (%s), change at XrTime %lld, pose %s (pos %+.3f %+.3f %+.3f m in the "
                 "old space) -- a recenter / seat reset", e.session, e.referenceSpaceType, SpaceTypeName(e.referenceSpaceType),
                 (long long)e.changeTime, e.poseValid ? "valid" : "not valid", e.poseInPreviousSpace.position.x,
                 e.poseInPreviousSpace.position.y, e.poseInPreviousSpace.position.z);
        break; }
    case kTypeEvEventsLost: {
        XrEvEventsLost e; memcpy(&e, ev, sizeof(e));
        snprintf(what, sizeof(what), " -- %u event(s) LOST by the runtime's queue", e.lostEventCount);
        break; }
    case kTypeEvInteractionProfileChanged: {
        XrEvSessionInt e; memcpy(&e, ev, 24);
        snprintf(what, sizeof(what), " session %p", e.session);
        break; }
    case kTypeEvRefreshRateChangedFb: {
        XrEvRefreshRateChangedFB e; memcpy(&e, ev, sizeof(e));
        snprintf(what, sizeof(what), " %.1f -> %.1f Hz", e.from, e.to);
        break; }
    case kTypeEvUserPresenceChangedExt: {
        XrEvSessionInt e; memcpy(&e, ev, sizeof(e));
        snprintf(what, sizeof(what), " session %p, user %s", e.session, e.a ? "PRESENT (headset on)" : "NOT present (headset off)");
        break; }
    case kTypeEvVisibilityMaskChangedKhr: {
        XrEvSessionInt e; memcpy(&e, ev, sizeof(e));
        snprintf(what, sizeof(what), " session %p, view configuration %d, view %u", e.session, e.a, e.b);
        break; }
    case kTypeEvPerfSettingsExt: {
        XrEvPerfSettings e; memcpy(&e, ev, sizeof(e));
        snprintf(what, sizeof(what), " domain %d sub-domain %d, level %d -> %d", e.domain, e.subDomain, e.fromLevel, e.toLevel);
        break; }
    default:
        break;
    }
    TbDescribe(tb, sizeof(tb));
    char drop[64] = "";
    if (dropped) snprintf(drop, sizeof(drop), " [%llu earlier event line(s) dropped: > 20 a second]", (unsigned long long)dropped);
    Log("OpenXR event #%llu: %s (type %d)%s (tid=%lu)%s | %s", (unsigned long long)n, EventTypeName(type), type, what,
        GetCurrentThreadId(), drop, tb);
}
XrResult PollEventImpl(int i, XrInstance inst, void* ev) {
    auto real = reinterpret_cast<PFN_xrPollEvent>(s_slot[i].pollEvent.load(std::memory_order_acquire));
    if (!real) return -1;
    CPU_HOOK(kHkXrOther);
    const bool outer = t_depth == 0;
    int32_t evType0 = 0;                                 // phase 40: the game's buffer header (a swallowed event polls again)
    const void* evNext0 = nullptr;
    if (outer && ev) { memcpy(&evType0, ev, sizeof(evType0)); memcpy(&evNext0, static_cast<char*>(ev) + 8, sizeof(evNext0)); }
    ++t_depth;
    XrResult r;
    CPU_ORIG(r = real(inst, ev));
    --t_depth;
    // phase 40: XrEventDataVisibilityMaskChangedKHR -> that view's mesh is fetched again at the next xrEndFrame. When only WE
    // enabled the extension the game never asked for these events: it gets the next event instead (at most 8 in a row)
    for (int k = 0; outer && r == 0 && ev && k < 8; ++k) {
        int32_t t = 0;
        memcpy(&t, ev, sizeof(t));
        if (t != kTypeEvVisibilityMaskChangedKhr) break;
        XrEvSessionInt e;
        memcpy(&e, ev, sizeof(e));
        s_hmEvents.fetch_add(1, std::memory_order_relaxed);
        if (e.b < 2) s_hmDirty[e.b].store(true, std::memory_order_relaxed);
        const bool ours = s_hmOurs.load(std::memory_order_relaxed);
        Log("hidden area mask: VISIBILITY_MASK_CHANGED for view %u (session %p, view configuration %d) -- fetched again at the next "
            "xrEndFrame%s", e.b, e.session, e.a, ours ? "; not passed to the game (it did not enable the extension)" : "");
        if (!ours) break;
        s_hmSwallowed.fetch_add(1, std::memory_order_relaxed);
        memcpy(ev, &evType0, sizeof(evType0));
        memcpy(static_cast<char*>(ev) + 8, &evNext0, sizeof(evNext0));
        ++t_depth;
        CPU_ORIG(r = real(inst, ev));
        --t_depth;
    }
    if (outer) {
        if (r == 0 && ev) LogEvent(ev);
        else if (r < 0) {
            static std::atomic<int> s_errs{0};
            if (s_errs.fetch_add(1, std::memory_order_relaxed) < 20)
                LogErr("OpenXR event: xrPollEvent(instance %p) FAILED -> %d = %s (tid=%lu)", inst, r, XrResultName(r), GetCurrentThreadId());
        }
    }
    return r;
}
// v0.10.3 phase 28: xrEndFrame -- the first projection layer: its per-view swapchain / rect / slice (the picture layout) and
// the submitted poses + fovs (the per-space correction C = S (x) L^-1, phase 28b). Read only.
XrResult EndFrameImpl(int i, XrSession ses, const XrFrameEndInfo* fe) {
    auto real = reinterpret_cast<PFN_xrEndFrame>(s_slot[i].endFrame.load(std::memory_order_acquire));
    if (!real) return -1;
    if (t_depth == 0) CpuProf::FrameMark(CpuProf::kMarkXrEnd);   // phase 33: the VR render thread's frame boundary
    CPU_HOOK(kHkXrEnd);                                    // phase 31 CPU profiler (cpu_prof.h)
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
                LogDbg("OpenXR hook pose diag: view 0 LOCATED (space %p %s) yaw %+.1f pitch %+.1f pos (%+.3f %+.3f %+.3f) | SUBMITTED "
                    "(layer space %p %s) yaw %+.1f pitch %+.1f pos (%+.3f %+.3f %+.3f) | correction C = S (x) L^-1: rotation %.1f deg "
                    "(yaw %+.1f pitch %+.1f) offset (%+.3f %+.3f %+.3f) m | mean residual of the previous C %.2f deg over %llu frames",
                    bestSpace, SpaceTypeName(bestType), ly, lp, L0.p[0], L0.p[1], L0.p[2], proj->space, SpaceTypeName(layerType), sy,
                    spt, S0.p[0], S0.p[1], S0.p[2], cAng, cy, cp, C0.p[0], C0.p[1], C0.p[2], resA, (unsigned long long)s_resN);
            }
        }
    }
    if (outer && proj) HmEndFrame(i, ses, proj);           // phase 40: the hidden area meshes (+ the submitted fov per view)
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
    // phase 30 (Turbo mode): s_tbMx over the whole turbo part of a TURBO frame -- the pending wait consumed + the real
    // xrBeginFrame, the real xrEndFrame, the next async wait. phase 33: every outermost xrEndFrame takes the frame's tag (how
    // it was begun); a REAL frame (the game's own xrBeginFrame reached the runtime) releases the lock over the real call.
    std::unique_lock<std::mutex> tlk(s_tbMx, std::defer_lock);
    const bool want = s_tbWant.load(std::memory_order_relaxed);
    uint8_t tag = 0;
    if (outer && fe) {
        tlk.lock();
        tag = TbPopTagLocked();
        if (tag == kTagTurbo) TbBeginForGameLocked(i, ses);
        else { TbMirrorLocked(); tlk.unlock(); }
    }
    ++t_depth;
    XrResult r;
    CPU_ORIG(r = real(ses, ov ? &ovFe : fe));
    if (ov && r < 0 && r != kErrSessionLost) {           // refused with our layers: the game's frame again, as it was
        OvRejected(r);
        CPU_ORIG(r = real(ses, fe));
    }
    --t_depth;
    if (outer && fe) {
        if (!tlk.owns_lock()) tlk.lock();
        TbAfterEndLocked(i, ses, r, want, tag);
        tlk.unlock();
        if (r != 0) NoteResult(kNsEnd, r, ses);          // phase 33
    }
    return r;
}
// v0.10.3 phase 29: xrDestroySession -- our swapchains / space of that session go first (through the runtime).
XrResult DestroySessionImpl(int i, XrSession ses) {
    auto real = reinterpret_cast<PFN_xrDestroySession>(s_slot[i].destroySession.load(std::memory_order_acquire));
    if (!real) return -1;
    CPU_HOOK(kHkXrOther);
    const bool outer = t_depth == 0;
    char tb[400] = "", who[300] = "";
    if (outer) {                                         // phase 33: who started it (before anything is reset)
        WhoStarted(who, sizeof(who));
        TbSessionReset(ses, true, tb, sizeof(tb));       // phase 30: the Toolkit drains its async wait first, too
    }
    if (outer) {
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
        HmDestroySession(ses);                           // phase 40
    }
    ++t_depth;
    XrResult r;
    CPU_ORIG(r = real(ses));
    --t_depth;
    if (outer && s_sesLogs.fetch_add(1, std::memory_order_relaxed) < kSesLogCap)
        Log("OpenXR session: xrDestroySession(%p) by the game -> %d = %s (tid=%lu; xrEndSession %s) %s | %s", ses, r, XrResultName(r),
            GetCurrentThreadId(), s_endCalled.load(std::memory_order_relaxed) ? "called before" : "NOT called before", who, tb);
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
    static XrResult XRH_CALL WaitFrame(XrSession a, const XrFrameWaitInfo* b, XrFrameState* c) { return WaitFrameImpl(I, a, b, c); }
    static XrResult XRH_CALL BeginFrame(XrSession a, const XrFrameBeginInfo* b) { return BeginFrameImpl(I, a, b); }   // phase 30
    static XrResult XRH_CALL EndSession(XrSession a) { return EndSessionImpl(I, a); }
    static XrResult XRH_CALL PollEvent(XrInstance a, void* b) { return PollEventImpl(I, a, b); }              // phase 33
    static XrResult XRH_CALL CreateSession(XrInstance a, const void* b, XrSession* c) { return CreateSessionImpl(I, a, b, c); }
    static XrResult XRH_CALL BeginSession(XrSession a, const XrSessionBeginInfo* b) { return BeginSessionImpl(I, a, b); }
    static XrResult XRH_CALL RequestExit(XrSession a) { return RequestExitSessionImpl(I, a); }
    static XrResult XRH_CALL CreateInstance(const XrInstanceCreateInfo* a, XrInstance* b) { return CreateInstanceImpl(I, a, b); }
};
struct ThunkSet { void *gipa, *negRuntime, *negLayer, *release, *acquire, *enumerate, *destroy, *locate, *endFrame, *createRefSpace,
                  *locateSpace, *destroySession, *waitFrame, *beginFrame, *endSession,
                  *pollEvent, *createSession, *beginSession, *requestExit,       // (phase 33: + 4)
                  *createInstance; };                                            // (phase 40: + 1)
template <int I> ThunkSet MakeSet() {
    return ThunkSet{ reinterpret_cast<void*>(&Thunk<I>::Gipa), reinterpret_cast<void*>(&Thunk<I>::NegRuntime),
                     reinterpret_cast<void*>(&Thunk<I>::NegLayer), reinterpret_cast<void*>(&Thunk<I>::Release),
                     reinterpret_cast<void*>(&Thunk<I>::Acquire), reinterpret_cast<void*>(&Thunk<I>::Enumerate),
                     reinterpret_cast<void*>(&Thunk<I>::Destroy), reinterpret_cast<void*>(&Thunk<I>::Locate),
                     reinterpret_cast<void*>(&Thunk<I>::EndFrame), reinterpret_cast<void*>(&Thunk<I>::CreateRefSpace),
                     reinterpret_cast<void*>(&Thunk<I>::LocateSpace), reinterpret_cast<void*>(&Thunk<I>::DestroySession),
                     reinterpret_cast<void*>(&Thunk<I>::WaitFrame), reinterpret_cast<void*>(&Thunk<I>::BeginFrame),
                     reinterpret_cast<void*>(&Thunk<I>::EndSession), reinterpret_cast<void*>(&Thunk<I>::PollEvent),
                     reinterpret_cast<void*>(&Thunk<I>::CreateSession), reinterpret_cast<void*>(&Thunk<I>::BeginSession),
                     reinterpret_cast<void*>(&Thunk<I>::RequestExit), reinterpret_cast<void*>(&Thunk<I>::CreateInstance) };
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
    else if (!strcmp(name, "xrWaitFrame"))                { store = &s_slot[i].waitFrame;  thunk = ts.waitFrame; }    // phase 30
    else if (!strcmp(name, "xrBeginFrame"))               { store = &s_slot[i].beginFrame; thunk = ts.beginFrame; }
    else if (!strcmp(name, "xrEndSession"))               { store = &s_slot[i].endSession; thunk = ts.endSession; }
    else if (!strcmp(name, "xrPollEvent"))                { store = &s_slot[i].pollEvent; thunk = ts.pollEvent; }   // phase 33
    else if (!strcmp(name, "xrCreateSession"))            { store = &s_slot[i].createSession; thunk = ts.createSession; }
    else if (!strcmp(name, "xrBeginSession"))             { store = &s_slot[i].beginSession; thunk = ts.beginSession; }
    else if (!strcmp(name, "xrRequestExitSession"))       { store = &s_slot[i].requestExit; thunk = ts.requestExit; }
    else if (!strcmp(name, "xrCreateInstance") && !inst && s_slot[i].kind == kKindRuntime) {   // phase 40 (hidden area mask)
        store = &s_slot[i].createInstance; thunk = ts.createInstance;
    }
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
    if (s_logs.fetch_add(1, std::memory_order_relaxed) < 64)   // (phase 30: was 16, + 3 names; phase 33: 32 -> 64, + 4 names)
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
            LogWarn("OpenXR hook: WARNING more than %d OpenXR components -- %s not wrapped (the VR panel falls back to the arm "
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
        LogErr("OpenXR hook: %ls!GetProcAddress not found", dll);
        return false;
    }
    MH_STATUS st = MH_CreateHook(target, hook, reinterpret_cast<void**>(orig));
    if (st == MH_ERROR_ALREADY_CREATED) {        // kernel32's export forwards to the kernelbase function detoured already
        Log("OpenXR hook: %ls!GetProcAddress (%p) is the kernelbase function -- detoured once", dll, target);
        return true;
    }
    if (st == MH_OK) st = MH_EnableHook(target);
    if (st != MH_OK) {
        LogErr("OpenXR hook: detour of %ls!GetProcAddress (%p) failed: %s", dll, target, MH_StatusToString(st));
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
        LogErr("OpenXR hook: MH_Initialize failed: %s -- VR panel stays at the arm point", MH_StatusToString(st));
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

// ---- phase 30: Turbo mode (see xr_hook.h; the logic is in the anonymous namespace above, from OpenXR Toolkit, MIT) ----
void SetTurbo(bool on) { s_tbWant.store(on, std::memory_order_relaxed); }
// ---- phase 52: VR FPS cap (see xr_hook.h; the pacing is in WaitFrameImpl) ----
void SetFpsCap(int div) { s_capDiv.store(FramePace::ClampVrDiv(div), std::memory_order_relaxed); }
FpsCapStats GetFpsCapStats() {
    FpsCapStats s;
    s.div = s_capDiv.load(std::memory_order_relaxed);
    s.basePeriodNs = s_capBaseA.load(std::memory_order_relaxed);
    s.waits = s_capWaits.load(std::memory_order_relaxed);
    s.sleeps = s_capSleeps.load(std::memory_order_relaxed);
    s.sleepNs = s_capSleepNs.load(std::memory_order_relaxed);
    return s;
}
bool FrameLoopActive() {
    const int64_t t = s_gameWaitNsA.load(std::memory_order_relaxed);
    return t && SteadyNs() - t < 1000000000LL;
}
int64_t DisplayPeriodNs() {                       // phase 45 (the tuning menu's Limit line)
    TbWorker& w = Wk();
    std::lock_guard<std::mutex> wl(w.mx);
    return (int64_t)w.lastPeriod;
}
TurboStats GetTurboStats() {
    TurboStats s;
    s.want = s_tbWant.load(std::memory_order_relaxed);
    const int64_t last = s_tbLastStartNs.load(std::memory_order_relaxed);
    s.running = s.want && last && SteadyNs() - last < 500000000LL;   // an async wait started within 0.5 s
    s.failed = s_tbFailed.load(std::memory_order_relaxed);
    s.asyncWaits = s_tbStarts.load(std::memory_order_relaxed);
    s.madeUp = s_tbFaked.load(std::memory_order_relaxed);
    s.blocks = s_tbBlocks.load(std::memory_order_relaxed);
    s.timeouts = s_tbToEnd.load(std::memory_order_relaxed) + s_tbToWait.load(std::memory_order_relaxed);
    s.failures = s_tbFails.load(std::memory_order_relaxed);
    return s;
}

// ---- phase 40: the hidden area mask (see xr_hook.h; the logic is in the anonymous namespace above) ----
void HiddenMaskRequest(bool want) { s_hmWant.store(want, std::memory_order_relaxed); }
void HiddenMaskActive(bool on) { s_hmActive.store(on, std::memory_order_relaxed); }
bool HiddenMaskRaw(int view, uint64_t haveGen, HiddenRaw* out) {
    if (view < 0 || view > 1 || !out) return false;
    AcquireSRWLockShared(&s_hmLock);
    const HmView& h = s_hmView[view];
    const bool fresh = h.gen != haveGen && h.gen != 0;
    if (fresh) {
        out->gen = h.gen;
        out->nv = h.have ? h.nv : 0;
        out->ni = h.have ? h.ni : 0;
        if (out->nv) {
            memcpy(out->xy, h.xy, sizeof(float) * 2 * out->nv);
            memcpy(out->idx, h.idx, sizeof(uint32_t) * out->ni);
        }
        memcpy(out->fov, h.fov, sizeof(out->fov));
    }
    ReleaseSRWLockShared(&s_hmLock);
    return fresh;
}
HiddenStats GetHiddenStats() {
    HiddenStats s;
    s.ext = s_hmExt.load(std::memory_order_relaxed);
    s.fn = s_hmFnOk.load(std::memory_order_relaxed);
    s.failed = s_hmFailedA.load(std::memory_order_relaxed);
    AcquireSRWLockShared(&s_hmLock);
    for (int v = 0; v < 2; ++v) {
        s.nv[v] = s_hmView[v].have ? s_hmView[v].nv : 0;
        s.ni[v] = s_hmView[v].have ? s_hmView[v].ni : 0;
    }
    ReleaseSRWLockShared(&s_hmLock);
    s.queries = s_hmQueries.load(std::memory_order_relaxed);
    s.events = s_hmEvents.load(std::memory_order_relaxed);
    return s;
}

} // namespace XrHook
