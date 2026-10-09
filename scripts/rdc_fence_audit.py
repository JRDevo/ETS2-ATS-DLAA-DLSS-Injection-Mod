# RenderDoc audit for the ETS2 / ATS DLAA injector v0.10.0 phase 12: the "jittery fence with moire" (ATS flat, user capture
# captures/ats_flat_fence_frame1634.rdc, taken WITH the mod loaded; its log: captures/dlaa_inject_ats_v0100p11_flat_fence.log).
#   "C:\Program Files\RenderDoc\qrenderdoc.exe" --python scripts\rdc_fence_audit.py
# (an empty %APPDATA%\qrenderdoc\UI.config hangs qrenderdoc: rename it first)
# Environment: RDC_CAPTURE (default captures\ats_flat_fence_frame1634.rdc), RDC_OUT (default <capture>.fence_audit.txt),
#              RDC_DUMP (default <capture>.fence_dump\: the PS bytecode *.dxbc for `lod_scope_test.exe --scan`, the fence
#              texture's alpha mips m<N>.png for scripts\fence_alpha_sim.py, the tone-mapped scene PNG),
#              RDC_FENCE_PX ("x,y;x,y;..." scene pixels on the fence, default: 4 pixels of the wire-mesh fence of frame 1634)
# Answers:
#   0. is the mod's own work in the capture at all? (compute dispatches, jittered viewports, sampler twins with a bias)
#   1. the pass map (world G-buffer = 4 RTVs + scene depth, forward = 1 RGBA16F RTV + scene depth) with GPU durations
#   2. every G-buffer draw: blend state (blend / A2C), pixel shader discard (RenderDoc disassembly), counts and GPU ms
#   3. the fence: pixel history of the given pixels on the G-buffer RT2 (albedo) -> the draw that owns each pixel, its PS,
#      blend / depth state, its texture (format, size, mips) and the game's sampler (filter, aniso, LOD bias, LOD range)
#   4. the forward "over" set (the phase-10 forward-depth batch re-draws exactly these) and its GPU cost
import os, sys, re, json, traceback, collections

def _default_capture():
    bases = []
    if "__file__" in globals():
        bases.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    bases += [os.getcwd(), os.path.dirname(os.getcwd()), r"E:\ETS2-DLAA-Injector"]
    for b in bases:
        c = os.path.join(b, "captures", "ats_flat_fence_frame1634.rdc")
        if os.path.isfile(c):
            return c
    return os.path.join(bases[0], "captures", "ats_flat_fence_frame1634.rdc")
CAP = os.environ.get("RDC_CAPTURE") or _default_capture()
OUT = os.environ.get("RDC_OUT") or (os.path.splitext(CAP)[0] + ".fence_audit.txt")
DUMP = os.environ.get("RDC_DUMP") or (os.path.splitext(CAP)[0] + ".fence_dump")
PX = [tuple(int(v) for v in p.split(",")) for p in
      (os.environ.get("RDC_FENCE_PX") or "2507,770;2005,780;2013,780;2510,775").split(";") if p.strip()]
os.makedirs(DUMP, exist_ok=True)
f = open(OUT, "w", buffering=1)
def P(*a): f.write(" ".join(str(x) for x in a) + "\n")
DEPTH_FMTS_S = ("D32_FLOAT_S8X24_UINT", "R32G8X24_TYPELESS", "D24_UNORM_S8_UINT", "R24G8_TYPELESS")

try:
    import renderdoc as rd
    def topy(o):
        bt = o.type.basetype
        if bt == rd.SDBasic.Struct: return {o.GetChild(i).name: topy(o.GetChild(i)) for i in range(o.NumChildren())}
        if bt == rd.SDBasic.Array: return [topy(o.GetChild(i)) for i in range(o.NumChildren())]
        if bt == rd.SDBasic.Resource: return int(o.AsResourceId())
        if bt == rd.SDBasic.Boolean: return o.AsBool()
        if bt == rd.SDBasic.Float: return o.AsFloat()
        if bt == rd.SDBasic.Enum: return o.AsString()
        if bt in (rd.SDBasic.UnsignedInteger, rd.SDBasic.SignedInteger): return o.AsInt()
        if bt == rd.SDBasic.Null: return None
        if bt == rd.SDBasic.Buffer: return "<buf>"
        try: return o.AsString()
        except Exception: return "?"
    def args(ch): return {ch.GetChild(k).name: ch.GetChild(k) for k in range(ch.NumChildren())}
    def first(a, *names):
        for nm in names:
            if nm in a: return a[nm]
        return None
    def short(x): return (x or "").replace("DXGI_FORMAT_", "").replace("D3D11_", "")

    cap = rd.OpenCaptureFile(); r = cap.OpenFile(CAP, '', None)
    P("capture", CAP, "open", r)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None); P("replay", res)
    sf = ctl.GetStructuredFile()

    tex = {}; view = {}; smp = {}; bsd = {}; dssd = {}
    calls = []; in_frame = False
    for ci, ch in enumerate(sf.chunks):
        n = ch.name
        if n == "Internal::Beginning of Capture": in_frame = True; continue
        a = args(ch)
        if not in_frame:
            nm = n.split("::")[-1]
            if nm in ("CreateTexture2D", "CreateTexture2D1"): tex[topy(a["pTexture"])] = topy(first(a, "Descriptor", "pDesc"))
            elif "pView" in a and "pResource" in a:
                view[topy(a["pView"])] = ("dsv" if "DepthStencil" in n else ("rtv" if "RenderTarget" in n else "srv"), topy(a["pResource"]))
            elif nm == "CreateSamplerState": smp[topy(a["pState"])] = topy(a["Descriptor"])
            elif nm in ("CreateBlendState", "CreateBlendState1"): bsd[topy(first(a, "pState", "pBlendState"))] = topy(first(a, "Descriptor", "pBlendStateDesc"))
            elif nm == "CreateDepthStencilState": dssd[topy(a["pState"])] = topy(first(a, "Descriptor", "pDepthStencilDesc"))
            continue
        calls.append((ci, n.split("::")[-1], a))
    def res_of(v): return view.get(v, (None, 0))[1]
    def tdesc(t): return tex.get(t) or {}
    def tstr(t):
        d = tdesc(t)
        return "R%d %s %sx%s mips %s" % (t, short(d.get("Format")), d.get("Width"), d.get("Height"), d.get("MipLevels")) if d else "R%d ?" % t
    def sstr(s):
        d = smp.get(s)
        if not d: return "R%d ?" % s
        return "R%d %s aniso %d bias %.2f LOD %.3g..%.3g address %s" % (s, short(d["Filter"]).replace("FILTER_", ""), d["MaxAnisotropy"],
               d["MipLODBias"], d["MinLOD"], d["MaxLOD"], short(d["AddressU"]).replace("TEXTURE_ADDRESS_", ""))
    def bstr(b):
        d = bsd.get(b)
        if not d: return "R%d (default: opaque)" % b if b else "null (opaque)"
        r0 = d["RenderTarget"][0]
        return "R%d A2C %s RT0 blend %s %s/%s" % (b, d["AlphaToCoverageEnable"], r0["BlendEnable"], short(r0["SrcBlend"]).replace("BLEND_", ""),
                                                 short(r0["DestBlend"]).replace("BLEND_", ""))
    def dstr(s):
        d = dssd.get(s)
        if not d: return "R%d ?" % s
        return "R%d depth %s write %s %s stencil %s" % (s, d["DepthEnable"], short(d["DepthWriteMask"]).replace("DEPTH_WRITE_MASK_", ""),
                                                        short(d["DepthFunc"]).replace("COMPARISON_", ""), d["StencilEnable"])

    # ---- (0) the mod's own work in the capture? ------------------------------------------------------------------------
    names = collections.Counter(n for _, n, _ in calls)
    vps = collections.Counter()
    for ci, n, a in calls:
        if n == "RSSetViewports":
            for v in topy(a["pViewports"]) or []:
                vps[(v["TopLeftX"] % 1.0 != 0.0) or (v["TopLeftY"] % 1.0 != 0.0)] += 1
    biased = [s for s, d in smp.items() if d["MipLODBias"] != 0.0]
    r16 = [t for t, d in tex.items() if str(d.get("Format", "")).endswith("R16_UINT")]
    P("\n== (0) the mod's own work in this capture")
    P("   Dispatch calls %d, CSSetShader %d (all null?) | viewports with a fractional (jittered) origin %d of %d | samplers with "
      "MipLODBias != 0: %d of %d | R16_UINT textures (draw-id targets): %d"
      % (names.get("Dispatch", 0), names.get("CSSetShader", 0), vps.get(True, 0), sum(vps.values()), len(biased), len(smp), len(r16)))
    if not names.get("Dispatch", 0) and not vps.get(True, 0) and not biased:
        P("   -> NONE of the mod's work is recorded: its hooks sit on the real d3d11.dll functions, BELOW RenderDoc's wrapper. RenderDoc "
          "records the game's own calls (unjittered viewports, the game's own samplers); the mod's argument rewrites and its own draws / "
          "dispatches go straight to the real context. The replay is the plain game frame.")

    # ---- state walk ----------------------------------------------------------------------------------------------------
    st = dict(bs=0, dss=0, rtvs=[], dsv=0, ps=0, smp={}, srv={}, vps=[])
    draws = []; oms = []
    for ci, n, a in calls:
        if n == "OMSetBlendState": st["bs"] = topy(a["pBlendState"]) or 0
        elif n == "OMSetDepthStencilState": st["dss"] = topy(a["pDepthStencilState"]) or 0
        elif n == "OMSetRenderTargets":
            st["rtvs"] = [x for x in (topy(a["ppRenderTargetViews"]) or []) if x]; st["dsv"] = topy(a["pDepthStencilView"]) or 0
            oms.append((ci, list(st["rtvs"]), st["dsv"]))
        elif n == "PSSetShader": st["ps"] = topy(first(a, "pPixelShader", "pShader")) or 0
        elif n == "PSSetSamplers":
            for i, x in enumerate(topy(a["ppSamplers"]) or []): st["smp"][topy(a["StartSlot"]) + i] = x
        elif n == "PSSetShaderResources":
            for i, x in enumerate(topy(a["ppShaderResourceViews"]) or []): st["srv"][topy(a["StartSlot"]) + i] = x
        elif n == "RSSetViewports": st["vps"] = topy(a["pViewports"]) or []
        elif n in ("DrawIndexed", "DrawIndexedInstanced", "Draw", "DrawInstanced"):
            draws.append(dict(ci=ci, kind=n, bs=st["bs"], dss=st["dss"], rtvs=list(st["rtvs"]), dsv=st["dsv"], ps=st["ps"],
                              smp=dict(st["smp"]), srv=dict(st["srv"]), vps=list(st["vps"])))
    acts = {}
    def walk(al):
        for c in al:
            if len(c.events): acts[c.events[-1].chunkIndex] = c.eventId
            if c.children: walk(c.children)
    walk(ctl.GetRootActions())
    dur = {}
    try:
        for r_ in ctl.FetchCounters([rd.GPUCounter.EventGPUDuration]): dur[r_.eventId] = r_.value.d
    except Exception:
        P("   counters: %s" % traceback.format_exc().splitlines()[-1])
    def ms(L): return sum(dur.get(acts.get(d["ci"]), 0.0) for d in L) * 1000.0
    eid2d = {acts.get(d["ci"]): d for d in draws}

    # PS disassembly (discard?) + bytecode dump
    tg = ctl.GetDisassemblyTargets(True)
    psinfo = {}
    for rdesc in ctl.GetResources():
        if rdesc.type != rd.ResourceType.Shader: continue
        try:
            eps = ctl.GetShaderEntryPoints(rdesc.resourceId)
            if not eps or eps[0].stage != rd.ShaderStage.Pixel: continue
            refl = ctl.GetShader(rd.ResourceId.Null(), rdesc.resourceId, eps[0])
            txt = ctl.DisassembleShader(rd.ResourceId.Null(), refl, tg[0]) if tg else ""
            open(os.path.join(DUMP, "%d.dxbc" % int(rdesc.resourceId)), "wb").write(bytes(refl.rawBytes))
            psinfo[int(rdesc.resourceId)] = len(re.findall(r"^\s*\d+:\s*discard", txt, re.M))
        except Exception:
            pass
    def disc(ps): return psinfo.get(ps, 0) > 0

    # ---- (1) passes ----------------------------------------------------------------------------------------------------
    cands = {}
    for v, (k, t) in view.items():
        d = tdesc(t)
        if k == "dsv" and any(str(d.get("Format", "")).endswith(x) for x in DEPTH_FMTS_S): cands[t] = d["Width"] * d["Height"]
    scene = max(cands, key=lambda t: cands[t]) if cands else 0
    SW, SH = tdesc(scene).get("Width", 0), tdesc(scene).get("Height", 0)
    gbi = [i for i, (ci, rt, dsv) in enumerate(oms) if len(rt) == 4 and res_of(dsv) == scene]
    fwi = [i for i, (ci, rt, dsv) in enumerate(oms) if len(rt) == 1 and res_of(dsv) == scene and
           str(tdesc(res_of(rt[0])).get("Format", "")).endswith("R16G16B16A16_FLOAT")]
    def span(i): return (oms[i][0], oms[i + 1][0] if i + 1 < len(oms) else 10 ** 9)
    G0, G1 = span(gbi[-1]); F0, F1 = span(fwi[-1])
    gb = [d for d in draws if G0 < d["ci"] < G1]; fw = [d for d in draws if F0 < d["ci"] < F1]
    P("\n== (1) passes: scene depth R%d %dx%d; replay GPU total %.3f ms over %d events (this PC, RenderDoc replay)"
      % (scene, SW, SH, sum(dur.values()) * 1000.0, len(dur)))
    P("   world G-buffer: chunks %d..%d, %d draws, %.3f ms | forward: chunks %d..%d, %d draws, %.3f ms | before the G-buffer "
      "(shadows, mirrors, probes) %.3f ms" % (G0, G1, len(gb), ms(gb), F0, F1, len(fw), ms(fw), ms([d for d in draws if d["ci"] < G0])))

    # ---- (2) G-buffer classification -----------------------------------------------------------------------------------
    P("\n== (2) world G-buffer draws by class (phase 11 'solid' = blend off + A2C off; phase 12 'opaque' = solid + PS without discard)")
    def bsolid(b):
        d = bsd.get(b)
        return (not d) or (not d["AlphaToCoverageEnable"] and not d["RenderTarget"][0]["BlendEnable"])
    cls = collections.defaultdict(list)
    for d in gb:
        cls["blended / A2C" if not bsolid(d["bs"]) else ("solid + discard (cut-out)" if disc(d["ps"]) else "solid, no discard")].append(d)
    for k, L in cls.items():
        P("   %-28s %4d draws %.3f ms, blend states %s, pixel shaders %d distinct" % (k, len(L), ms(L), dict(collections.Counter(d["bs"] for d in L)),
          len(set(d["ps"] for d in L))))
    cut = cls.get("solid + discard (cut-out)", [])
    P("   cut-out PS (draws): %s" % collections.Counter(d["ps"] for d in cut).most_common())
    P("   pixel shaders in the frame: %d, with a discard: %d (bytecode dumped to %s for lod_scope_test.exe --scan)"
      % (len(psinfo), sum(1 for v in psinfo.values() if v), DUMP))

    # ---- (3) the fence --------------------------------------------------------------------------------------------------
    P("\n== (3) the fence pixels (pixel history on the G-buffer RT2 at the last G-buffer draw)")
    lastg = acts.get(gb[-1]["ci"])
    ctl.SetFrameEvent(lastg, True)
    om = ctl.GetD3D11PipelineState().outputMerger
    v = om.renderTargets[2]
    rt2 = v.resource if hasattr(v, "resource") else v.resourceId
    owners = collections.Counter()
    for (x, y) in PX:
        try:
            mods = ctl.PixelHistory(rt2, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            win = [m for m in mods if m.Passed() and m.eventId in eid2d]
            if not win:
                P("   (%d,%d): no draw" % (x, y)); continue
            d = eid2d[win[-1].eventId]
            owners[(d["ps"], res_of(d["srv"].get(6)), d["smp"].get(0), d["bs"], d["dss"])] += 1
            P("   (%d,%d): EID %d %s PS R%d discard=%s | %s | %s | t6 %s | s0 %s | depth %.6f"
              % (x, y, win[-1].eventId, d["kind"], d["ps"], disc(d["ps"]), bstr(d["bs"]), dstr(d["dss"]), tstr(res_of(d["srv"].get(6))),
                 sstr(d["smp"].get(0)), win[-1].postMod.depth))
        except Exception:
            P("   (%d,%d): %s" % (x, y, traceback.format_exc().splitlines()[-1]))
    if owners:
        (fps, ftex, fsmp, fbs, fdss), _ = owners.most_common(1)[0]
        same = [d for d in gb if res_of(d["srv"].get(6)) == ftex]
        P("   -> the fence = G-buffer draws with t6 %s: %d draws, %.3f ms, PS %s (discard: %s), blend states %s, samplers s0 %s"
          % (tstr(ftex), len(same), ms(same), dict(collections.Counter(d["ps"] for d in same)),
             {p: disc(p) for p in set(d["ps"] for d in same)}, dict(collections.Counter(d["bs"] for d in same)),
             dict(collections.Counter(d["smp"].get(0) for d in same))))
        P("      the game's sampler: %s" % sstr(fsmp))
        P("      phase 11 (tex_lod_bias_scope = solid): blend %s -> 'solid' -> the FULL twin = the same desc with MipLODBias + "
          "tex_lod_bias (the log's first twin line: filter 0x55 -> 0x55, bias 0.00 -> -1.00, MaxAnisotropy 16 -> 16) = the risk-3 case"
          % ("opaque" if bsolid(fbs) else "blended"))
        # alpha mips of the fence texture + the tone-mapped scene for the simulation / a look
        for m in range(int(tdesc(ftex).get("MipLevels", 1))):
            ts = rd.TextureSave(); ts.resourceId = [x.resourceId for x in ctl.GetResources() if int(x.resourceId) == ftex][0]
            ts.destType = rd.FileType.PNG; ts.mip = m; ts.channelExtract = 3; ts.alpha = rd.AlphaMapping.Discard
            ctl.SaveTexture(ts, os.path.join(DUMP, "m%d.png" % m))
        P("      alpha mips saved: %s\\m<N>.png (scripts\\fence_alpha_sim.py)" % DUMP)

    # ---- (4) the forward over set ------------------------------------------------------------------------------------
    def over(d):
        ds = dssd.get(d["dss"]) or {}; b = bsd.get(d["bs"]) or {}
        b0 = (b.get("RenderTarget") or [{}])[0]; vp = d["vps"][0] if d["vps"] else None
        return (ds.get("DepthEnable") and "ZERO" in str(ds.get("DepthWriteMask")) and b0.get("BlendEnable") and
                "INV_SRC_ALPHA" in str(b0.get("DestBlend")) and vp and vp["MinDepth"] >= 0.005 and vp["MaxDepth"] <= 0.95)
    ov = [d for d in fw if over(d)]
    P("\n== (4) forward 'over' set (what the phase-10 forward-depth batch re-draws with the game's PS): %d draws, %.3f ms at %dx%d "
      "(the game's own pass, with the real scene depth for culling); PS %s" % (len(ov), ms(ov), SW, SH,
      collections.Counter(d["ps"] for d in ov).most_common(6)))
    P("   (the G-buffer pass above, %.3f ms, bounds the draw-id replay: the same %d draws' vertex work with a one-instruction PS)"
      % (ms(gb), len(gb)))
    ctl.Shutdown(); cap.Shutdown()
    P("\nDONE")
except SystemExit as e:
    P("STOP:", e)
except Exception:
    P(traceback.format_exc())
f.close()
os._exit(0)
