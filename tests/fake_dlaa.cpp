// v0.10.0 phase 7: a stand-in DlaaProcessor (src/dlaa.h) for the WARP harness -- there is no NGX on WARP. Lets the harness link
// the real SceneDlaa (src/scene_dlaa.cpp) and test its colour-set bookkeeping (two units kept alive, swaps instead of rebuilds,
// Warm, KindIdleRuns, size change, deferred / failed builds). "Features" are counted ids; Evaluate clears the output UAV to a
// value derived from the feature id (so the harness can read back WHICH feature / colour set produced a picture) and records
// the reset flag it was given. Built only into build\tests\drawid_harness.exe (tests\build_drawid_harness.bat).
#include <d3d11.h>
#include <cstdint>
#include "dlaa.h"

namespace FakeNgx {
int       creates = 0;        // Init calls that built a feature
int       destroys = 0;       // Shutdown calls that released one
int       evals = 0;          // Evaluate calls that ran
int       resets = 0;         // ... with reset = 1
bool      budgetBusy = false; // CreateBudgetFree() answers false (SceneDlaa defers a build)
bool      failHdrInit = false;// Init of an HDR feature fails
uintptr_t nextId = 1;
uintptr_t lastFeature = 0;    // id of the feature that evaluated last
bool      lastReset = false, lastHdr = false;
float     Marker(uintptr_t id) { return (float)(id % 200) / 256.0f + 0.1f; }   // the UAV clear value of a feature
}

bool DlaaProcessor::Init(ID3D11Device* dev, uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH,
                         bool depthInverted, bool hdr) {
    if (m_ngxInited || !dev) return false;
    if (hdr && FakeNgx::failHdrInit) return false;
    m_dev = dev; m_dev->AddRef();
    m_width = renderW; m_height = renderH; m_outW = outW; m_outH = outH;
    m_hdr = hdr; m_depthInverted = depthInverted; m_ngxInited = true;
    m_params = reinterpret_cast<NVSDK_NGX_Parameter*>(static_cast<uintptr_t>(16));   // never dereferenced
    m_feature = reinterpret_cast<NVSDK_NGX_Handle*>(FakeNgx::nextId++ * 16);
    ++FakeNgx::creates;
    return true;
}

bool DlaaProcessor::Evaluate(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* color, ID3D11ShaderResourceView* depth,
                             ID3D11ShaderResourceView* motionVectors, ID3D11UnorderedAccessView* output,
                             const DlaaFrameParams& p) {
    if (!m_feature || !ctx || !color || !depth || !motionVectors || !output) return false;
    const uintptr_t id = reinterpret_cast<uintptr_t>(m_feature) / 16;
    ++FakeNgx::evals;
    if (p.reset) ++FakeNgx::resets;
    FakeNgx::lastFeature = id; FakeNgx::lastReset = p.reset; FakeNgx::lastHdr = m_hdr;
    const float v = FakeNgx::Marker(id);
    const float c[4] = { v, v, v, 1.0f };
    ctx->ClearUnorderedAccessViewFloat(output, c);
    return true;
}

void DlaaProcessor::Shutdown() {
    if (m_feature) ++FakeNgx::destroys;
    m_feature = nullptr; m_params = nullptr; m_ngxInited = false;
    if (m_dev) { m_dev->Release(); m_dev = nullptr; }
    m_width = m_height = m_outW = m_outH = 0; m_perfQ = 0; m_depthInverted = false; m_hdr = false;
}

const char* DlaaProcessor::QualityName() const { return "DLAA"; }
bool DlaaProcessor::SetRenderPreset(char) { return true; }
const char* DlaaProcessor::RenderPresetName() { return "default"; }
uint32_t DlaaProcessor::PresetFallbacks() { return 0; }
bool DlaaProcessor::Prewarm(ID3D11Device*) { return false; }
void* DlaaProcessor::NgxDevice() { return nullptr; }
void DlaaProcessor::BeginFrame() {}
bool DlaaProcessor::CreateBudgetFree() { return !FakeNgx::budgetBusy; }
