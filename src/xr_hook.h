// XrHook -- v0.10.3 phase 27: the OpenXR swapchain release, seen from the game's side.
//
// Why: the VR tuning panel / fps box must be the LAST thing drawn into each eye picture. In ATS / ETS2 VR the eye picture
// IS the OpenXR swapchain image (stereo scale 2.0: 6120x6496); the runtime takes it at xrReleaseSwapchainImage, a call the
// D3D11 hooks never see (the v0.10.0 phase 16 / 20 "late" mode waits for a D3D11 read of the picture that never comes:
// 0 accepted game reads in ~22000 Presents). This module wraps xrReleaseSwapchainImage (+ xrEnumerateSwapchainImages /
// xrAcquireSwapchainImage / xrDestroySwapchain for the swapchain -> ID3D11Texture2D* image map) and calls a callback right
// BEFORE the real release, with the D3D11 texture being released.
//
// How it gets in: the game exe links the OpenXR loader STATICALLY (amtrucks.exe has the loader's RuntimeInterface /
// ApiLayerInterface code and no openxr_loader.dll ships), so there is no loader export to detour. Instead kernelbase!
// GetProcAddress is detoured (MinHook) and three names are answered with our thunks:
//  * "xrNegotiateLoaderRuntimeInterface" (the runtime DLL) and "xrNegotiateLoaderApiLayerInterface" (each API layer DLL,
//    OFXR Bridge included): after the real negotiation the returned getInstanceProcAddr is replaced by a thunk. The loader
//    builds the game's dispatch table from the TOP of the chain (the first layer, else the runtime), and each layer gets
//    its "next" from the same negotiated pointers -- so every level is wrapped and a thread-local depth counter makes only
//    the OUTERMOST call (the game's own) act: the image map and the release callback are always the game's view, above
//    the layers (OFXR Bridge's own nested calls are passed straight through).
//  * "xrGetInstanceProcAddr" resolved from a module named openxr_loader.dll (a game build that ships the loader DLL):
//    the same thunk on the loader's own entry point.
// v0.10.3 phase 28: + xrLocateViews / xrEndFrame / xrCreateReferenceSpace (read only: the view poses, see LatestViews).
// v0.10.3 phase 29: + xrDestroySession, and xrEndFrame gets our quad layers appended (OverlaySet below).
// Only these function names are touched; everything else the GIPA thunks return is the real pointer. No OpenXR SDK
// dependency: the few types needed are declared in xr_hook.cpp. No D3D calls here (the texture is passed as an identity).
#pragma once
#include <cstdint>

namespace XrHook {

// Called on the thread that calls xrReleaseSwapchainImage, before the real release. `tex` = the ID3D11Texture2D* of the
// image being released (identity, no reference added; null: no D3D11 image map for that swapchain -- not enumerated
// through us, or not a D3D11 swapchain).
typedef void (*ReleaseFn)(void* swapchain, void* tex);

// Detours GetProcAddress (initialises MinHook if needed). Call once, early (before the game creates its XrInstance).
bool Install();
void SetReleaseCallback(ReleaseFn fn);

// Diagnostics (atomics; any thread).
bool     Hooked();                // GetProcAddress detoured
int      Components();            // runtime / layer / loader entry points wrapped so far
uint64_t Releases();              // outermost xrReleaseSwapchainImage calls seen
uint64_t ReleasesMapped();        // ... with a known D3D11 image

// ---- v0.10.3 phase 28: the game's view poses (the VR fps box "Fixed in cab", dlaa.ini fps_vr_lock = 1) ----------------
// xrLocateViews, xrEndFrame and xrCreateReferenceSpace are wrapped too (outermost call = the game's own, as above). Each
// successful PRIMARY_STEREO xrLocateViews is kept (a ring of 8, with the space it was located in); each xrEndFrame's first
// projection layer gives the LAYOUT the runtime will use for the pictures (per view: swapchain, sub-image rect, array slice,
// layer space, fov) and the SUBMITTED poses. Phase 28b (the "lag" matching of phase 28 replaced: the ATS / Virtual Desktop
// test showed submitted poses that equal no located one in 85 % of the frames): per locate space and view, the correction
// C = S (x) L^-1 between the submitted pose S and the locate L the box was drawn with; LatestViews hands out C (x) L_newest
// in the LAYER space -- the pose the compositor shows the picture with (its timewarp then keeps the box fixed in that space).
struct View {
    float qx, qy, qz, qw;                    // orientation in the reference space (unit quaternion)
    float px, py, pz;                        // position in the reference space (m); OpenXR: +x right, +y up, -z forward
    float angleLeft, angleRight, angleUp, angleDown;   // fov (radians; left / down negative)
};
struct Views {
    View     v[2];                           // phase 28b: the pose CORRECTED to the submitted one (C (x) L), the submitted fov
    View     raw[2];                         // ... the located pose / fov as the game got it (diagnostics)
    void*    space = nullptr;                // the projection layer's XrSpace (the space v[] is in)
    int      spaceType = 0;                  // its XrReferenceSpaceType if created through us (1 VIEW, 2 LOCAL, 3 STAGE,
                                             //   1000426000 LOCAL_FLOOR); 0 = unknown (an action space, or created before us)
    void*    locSpace = nullptr;             // the space the game located the views in (raw[])
    int      locSpaceType = 0;
    uint64_t flags = 0;                      // XrViewStateFlags (1 orientation valid, 2 position valid, 4 / 8 tracked)
    double   ageSec = 0.0;                   // seconds since that xrLocateViews returned
    int      corrected = 0;                  // 1 = v[] carries a correction learned at an xrEndFrame
    uint64_t seq = 0;                        // the locate's sequence number (1 = the first locate)
};
// The views to project with now (copied out under a lock; any thread). False = no stereo locate + xrEndFrame correction yet.
bool LatestViews(Views* out);
// Phase 28b: log the located / submitted / correction poses once a second (at xrEndFrame) for `seconds` (0 = stop).
void PoseDiagArm(int seconds);
// The view whose picture is D3D11 image `tex` (array slice `slice`) in the game's last xrEndFrame projection layer, and its
// sub-image rect (x, y, w, h px). -1 = not known (no xrEndFrame seen, `tex` is not a swapchain image -- an intermediate eye
// texture -- or not in the layer).
int ViewForImage(void* tex, uint32_t slice, int32_t rect[4]);
// Counters: stereo locates seen, xrEndFrame calls with a projection layer, submitted view-0 poses equal to a located one / not,
// the residual of the previous frame's correction on this frame's locate against the actual submitted pose (mean / max degrees,
// mean mm: what the box is off by in the compositor), and the spaces the game locates its views in.
struct PoseStats {
    uint64_t locates, endFrames, matched, unmatched;
    uint64_t resFrames; double resMeanDeg, resMeanMm, resMaxDeg;
    struct Sp { void* space; int type; uint64_t count; } spaces[4];
    int nSpaces, layerSpaceType;
};
PoseStats GetPoseStats();

// ---- v0.10.3 phase 29: the VR tuning panel / fps box as OpenXR QUAD composition layers (dlaa.ini menu_vr_layer = 1) -------
// Drawn into the eye picture (phases 16 / 27) a head-locked overlay is part of the projection layer the runtime timewarps with
// the game's submitted pose every frame: it swims / jitters by the head motion between that pose and the display. As a quad
// layer the COMPOSITOR places it: the panel and the head-following fps box in a VIEW reference space of our own (head-locked
// by the runtime itself), the fps box "fixed in the cab" in the game's projection-layer space at the anchor pose (no pose
// maths, no lag). xrEndFrame / xrDestroySession are wrapped (outermost = the game's call): per frame the wanted quads get an
// image of our own swapchains (acquire, wait, the inject.cpp draw callback renders into the D3D11 texture, release) and are
// APPENDED after the game's layers (on top) in a copy of its XrFrameEndInfo. The OpenXR functions we call are resolved through
// the real xrGetInstanceProcAddr of the component that handles the game's outermost xrEndFrame (the top of the chain as the
// game sees it), every call inside ++t_depth (our swapchains never reach the image map or the release callback). Thread: the
// draw needs the game's immediate context -> at xrEndFrame when it runs on `drawTid` (the render thread), else at the game's
// first xrReleaseSwapchainImage of the frame on `drawTid` (the layers are still appended at xrEndFrame). A runtime that
// rejects the frame with our layers gets it again without them, and the quad path stays off for that session.
struct OverlayParams {
    bool          enabled = false;           // dlaa.ini menu_vr_layer
    bool          menuOpen = false;          // the panel quad is wanted
    bool          fpsShow = false;           // the fps box quad is wanted
    int           fpsLock = 0;               // fps_vr_lock: 0 follows the head (VIEW space), 1 fixed in the cab (layer space)
    int           menuX = 0, menuY = 0;      // menu_vr_x / menu_vr_y (% of the eye fov from the view axis; y + = down)
    float         menuScale = 1.0f;          // menu_vr_scale (1 = 54 % of the eye fov width, at any distance)
    float         menuDist = 1.0f;           // v0.10.3 phase 29b: menu_vr_dist / 100 -- the panel quad's distance (m, VIEW space;
                                             //   its angular size stays: the width scales with it)
    bool          menuFace = true;           // v0.10.3 phase 29b: menu_vr_face -- the panel and the head-following fps box quads
                                             //   turned to FACE the head (false: parallel to the view plane)
    int           fpsX = 0, fpsY = 0;        // fps_vr_x / fps_vr_y (% as the panel; fixed in the cab: yaw / pitch degrees)
    float         fpsScale = 0.3f;           // fps_vr_scale (1 = 8 % of the eye fov width at 0.8 m)
    unsigned long drawTid = 0;               // the game render thread (owner of the immediate context); 0 = none yet
    uint32_t      panelW = 0, panelH = 0;    // the panel swapchain size (px; read at its creation)
    uint32_t      fpsW = 0, fpsH = 0;        // the fps box swapchain size (px)
};
void OverlaySet(const OverlayParams& p);     // inject.cpp, every Present (any thread; copied under a lock)
// The draw callback (inject.cpp, on drawTid): clear `tex` (the swapchain image, an ID3D11Texture2D* of `format` = the
// DXGI_FORMAT the swapchain was created with; the texture itself may be typeless) and draw quad `which` (0 = the panel, 1 = the
// fps box) into its top-left corner; usedW / usedH = the drawn size (px; the quad shows that sub-image). False = not drawn
// (device objects not ready, wrong thread ...): the quad is skipped this frame (and counted).
struct OverlayDraw {
    int      which = 0;
    void*    tex = nullptr;
    uint32_t w = 0, h = 0;
    int64_t  format = 0;
    uint32_t usedW = 0, usedH = 0;           // out
};
typedef bool (*OverlayDrawFn)(OverlayDraw* d);
void SetOverlayDrawCallback(OverlayDrawFn fn);
// True = quad `which` (0 panel, 1 fps box) went to the compositor at the last xrEndFrame (within 0.5 s), or the quad path is
// up and that quad was not wanted -- inject.cpp then does NOT draw it into the eye picture. False = the in-picture fallback.
bool OverlayActive(int which);
struct OverlayStats {
    bool     enabled = false, ready = false; // menu_vr_layer; swapchains + space made for the current session
    int      mode = 0;                       // where the quads are drawn: 0 not yet, 1 at xrEndFrame, 2 at the game's release
    int64_t  format = 0;                     // the swapchain DXGI_FORMAT (0 = none yet)
    uint64_t frames = 0, panelQuads = 0, fpsQuads = 0;   // xrEndFrame calls with our quads appended; quads per kind
    uint64_t createFails = 0, acquireFails = 0, waitFails = 0, releaseFails = 0, drawFails = 0, rejects = 0;
    int      fixedHow = 0;                   // fps box fixed in the cab: 0 not used yet, 1 the layer space (LOCAL: its origin =
                                             //   the seat), 2 the layer space around a pivot captured at lock-on, 3 head-locked
                                             //   (the layer space is a VIEW space)
    int      fixedSpaceType = 0;             // that layer space's XrReferenceSpaceType (0 = unknown)
};
OverlayStats GetOverlayStats();

} // namespace XrHook
