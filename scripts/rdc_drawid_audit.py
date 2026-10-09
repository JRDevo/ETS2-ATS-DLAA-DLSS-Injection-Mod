# RenderDoc audit for the ETS2 / ATS DLAA injector v0.10.0: can the world G-buffer draws be REPLAYED at the pass end
# (same VS / IA / cbuffers / RS / viewport, our pixel shader, depth func EQUAL against the game's scene depth) to get an
# exact per-pixel draw id? Answers the brief's engine assumptions a-g (captures/v010_brief.md).
#
# Run with RenderDoc's own Python:
#   "C:\Program Files\RenderDoc\qrenderdoc.exe" --python scripts\rdc_drawid_audit.py
# Environment: RDC_CAPTURE (default captures\ets2_flat_frame1969.rdc), RDC_OUT (default <capture>.drawid_audit.txt),
#              RDC_REPLAY=0 skips the shader reflection pass.
#
# Pure structured-chunk scan (fast) for the state of every G-buffer draw:
#   a. VS cbuffer slots (buffer, first constant, count) and VS SRVs / samplers per draw; Map / UpdateSubresource of the
#      buffers those draws read DURING the pass (a per-draw rewritten buffer would break an end-of-pass replay)
#   b. GS / HS / DS / stream-out bound during the pass
#   c. rasterizer states (cull, scissor, depth clip, depth bias)
#   d. depth-stencil states (depth func / write) of the draws
#   e. viewports / scissor rects per draw
#   f. DrawIndexedInstanced draws: count, instance counts, VB slot-1 use, index counts
#   g. scene depth texture format / bind flags and the game's DSV desc
# plus the replay (shader reflection per distinct VS / PS): SV_Depth / SV_Coverage outputs, VS output signature.
import os, sys, struct, traceback, collections

def _default_capture():
    bases = []
    if "__file__" in globals():
        bases.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    bases += [os.getcwd(), os.path.dirname(os.getcwd())]
    for b in bases:
        c = os.path.join(b, "captures", "ets2_flat_frame1969.rdc")
        if os.path.isfile(c):
            return c
    return os.path.join(bases[0], "captures", "ets2_flat_frame1969.rdc")
CAP = os.environ.get("RDC_CAPTURE") or _default_capture()
OUT = os.environ.get("RDC_OUT") or (os.path.splitext(CAP)[0] + ".drawid_audit.txt")
REPLAY = os.environ.get("RDC_REPLAY", "1") != "0"

f = open(OUT, "w", buffering=1)
def P(*a):
    f.write(" ".join(str(x) for x in a) + "\n")

DEPTH_FMTS_S = ("D32_FLOAT_S8X24_UINT", "R32G8X24_TYPELESS", "D24_UNORM_S8_UINT", "R24G8_TYPELESS")

try:
    import renderdoc as rd

    def topy(o):
        bt = o.type.basetype
        if bt == rd.SDBasic.Struct:
            return {o.GetChild(i).name: topy(o.GetChild(i)) for i in range(o.NumChildren())}
        if bt == rd.SDBasic.Array:
            return [topy(o.GetChild(i)) for i in range(o.NumChildren())]
        if bt == rd.SDBasic.Resource: return int(o.AsResourceId())
        if bt == rd.SDBasic.Boolean: return o.AsBool()
        if bt == rd.SDBasic.Float: return o.AsFloat()
        if bt == rd.SDBasic.Enum: return o.AsString()
        if bt in (rd.SDBasic.UnsignedInteger, rd.SDBasic.SignedInteger): return o.AsInt()
        if bt == rd.SDBasic.Null: return None
        try: return o.AsString()
        except Exception: return "?"

    def args(ch):
        return {ch.GetChild(k).name: ch.GetChild(k) for k in range(ch.NumChildren())}
    def first(a, *names):
        for nm in names:
            if nm in a: return a[nm]
        return None
    def firstres(a):
        for k, v in a.items():
            if v.type.basetype == rd.SDBasic.Resource: return int(v.AsResourceId())
        return 0
    def short(x): return (x or "").replace("DXGI_FORMAT_", "").replace("D3D11_", "")

    cap = rd.OpenCaptureFile()
    r = cap.OpenFile(CAP, '', None)
    P("capture", CAP, "open", r)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    P("replay", res)
    sf = ctl.GetStructuredFile()

    tex = {}; dsv = {}; rtv = {}; srv = {}; dss = {}; rss = {}; buf = {}; il = {}
    logged = set()
    st = dict(dss=0, ref=0, rs=0, dsv=0, rtvs=[], ib=(0, None, 0), vbs={}, layout=0, topo=None,
              vs=0, ps=0, gs=0, hs=0, ds=0, so=[], vscb={}, vssrv={}, vssmp={}, vps=[], sc=[])
    events = []
    maps = []           # (ci, kind, buffer, maptype)
    in_frame = False
    for ci, ch in enumerate(sf.chunks):
        n = ch.name
        a = args(ch)
        if n == "Internal::Beginning of Capture":
            in_frame = True
            continue
        if n.endswith("::CreateTexture2D") or n.endswith("::CreateTexture2D1"):
            tex[topy(a["pTexture"])] = topy(first(a, "Descriptor", "pDesc"))
        elif n.endswith("::CreateDepthStencilView"):
            dsv[topy(a["pView"])] = (topy(a["pResource"]), topy(a["pDesc"]) if "pDesc" in a else None)
        elif n.endswith("::CreateRenderTargetView") or n.endswith("::CreateRenderTargetView1"):
            rtv[topy(a["pView"])] = (topy(a["pResource"]), topy(a["pDesc"]) if "pDesc" in a else None)
        elif n.endswith("::CreateShaderResourceView") or n.endswith("::CreateShaderResourceView1"):
            srv[topy(a["pView"])] = (topy(a["pResource"]), topy(a["pDesc"]) if "pDesc" in a else None)
        elif n.endswith("::CreateDepthStencilState"):
            dss[topy(a["pState"])] = topy(first(a, "Descriptor", "pDepthStencilDesc"))
        elif n.endswith("::CreateRasterizerState") or n.endswith("::CreateRasterizerState1") or n.endswith("::CreateRasterizerState2"):
            if n not in logged: logged.add(n); P("chunk", n, "args", sorted(a.keys()))
            k_, d_ = first(a, "pState", "pRasterizerState", "ppRasterizerState"), first(a, "Descriptor", "pRasterizerDesc", "pDesc")
            if k_ is not None and d_ is not None: rss[topy(k_)] = topy(d_)
        elif n.endswith("::CreateBuffer"):
            buf[topy(a["pBuffer"])] = topy(a["pDesc"])
        elif n.endswith("::CreateInputLayout"):
            il[topy(a["pInputLayout"])] = topy(a["pInputElementDescs"])
        if not in_frame:
            continue
        if n.endswith("::OMSetDepthStencilState"):
            st["dss"] = topy(a["pDepthStencilState"]); st["ref"] = topy(a["StencilRef"])
        elif n.endswith("::RSSetState"):
            st["rs"] = topy(first(a, "pRasterizerState", "pState"))
        elif n.endswith("::OMSetRenderTargets"):
            st["rtvs"] = [x for x in topy(a["ppRenderTargetViews"]) if x]
            st["dsv"] = topy(a["pDepthStencilView"])
            events.append(dict(ci=ci, kind="OM", rtvs=list(st["rtvs"]), dsv=st["dsv"]))
        elif n.endswith("::OMSetRenderTargetsAndUnorderedAccessViews"):
            if topy(a["NumRTVs"]) != 0xFFFFFFFF:
                st["rtvs"] = [x for x in topy(a["ppRenderTargetViews"]) if x]
                st["dsv"] = topy(a["pDepthStencilView"])
                events.append(dict(ci=ci, kind="OM", rtvs=list(st["rtvs"]), dsv=st["dsv"]))
        elif n.endswith("::IASetPrimitiveTopology"):
            st["topo"] = short(topy(a["Topology"])).replace("PRIMITIVE_TOPOLOGY_", "").replace("D3D_", "")
        elif n.endswith("::IASetInputLayout"):
            st["layout"] = topy(a["pInputLayout"])
        elif n.endswith("::IASetIndexBuffer"):
            st["ib"] = (topy(a["pIndexBuffer"]), topy(a["Format"]), topy(a["Offset"]))
        elif n.endswith("::IASetVertexBuffers"):
            s0 = topy(a["StartSlot"]); vbl = topy(a["ppVertexBuffers"]); s_ = topy(a["pStrides"]); o_ = topy(a["pOffsets"])
            for i, v in enumerate(vbl):
                st["vbs"][s0 + i] = (v, s_[i] if i < len(s_) else 0, o_[i] if i < len(o_) else 0)
        elif n.endswith("::VSSetShader"):
            st["vs"] = topy(first(a, "pVertexShader", "pShader")) or 0
        elif n.endswith("::PSSetShader"):
            st["ps"] = topy(first(a, "pPixelShader", "pShader")) or 0
        elif n.endswith("::GSSetShader"):
            st["gs"] = topy(first(a, "pShader", "pGeometryShader")) or 0
        elif n.endswith("::HSSetShader"):
            st["hs"] = topy(first(a, "pShader", "pHullShader")) or 0
        elif n.endswith("::DSSetShader"):
            st["ds"] = topy(first(a, "pShader", "pDomainShader")) or 0
        elif n.endswith("::SOSetTargets"):
            st["so"] = [x for x in topy(first(a, "ppSOTargets")) or [] if x]
        elif n.endswith("::VSSetConstantBuffers1") or n.endswith("::VSSetConstantBuffers"):
            s0 = topy(a["StartSlot"]); b = topy(a["ppConstantBuffers"])
            fc = topy(a["pFirstConstant"]) if "pFirstConstant" in a else None
            nc = topy(a["pNumConstants"]) if "pNumConstants" in a else None
            for i, x in enumerate(b):
                st["vscb"][s0 + i] = (x, fc[i] if fc and i < len(fc) else 0, nc[i] if nc and i < len(nc) else 4096,
                                      n.endswith("1"))
        elif n.endswith("::VSSetShaderResources"):
            s0 = topy(a["StartSlot"])
            for i, x in enumerate(topy(a["ppShaderResourceViews"])): st["vssrv"][s0 + i] = x
        elif n.endswith("::VSSetSamplers"):
            s0 = topy(a["StartSlot"])
            for i, x in enumerate(topy(a["ppSamplers"])): st["vssmp"][s0 + i] = x
        elif n.endswith("::RSSetViewports"):
            st["vps"] = topy(a["pViewports"]) or []
        elif n.endswith("::RSSetScissorRects"):
            st["sc"] = topy(first(a, "pRects")) or []
        elif n.endswith("::Map"):
            if n not in logged: logged.add(n); P("chunk", n, "args", sorted(a.keys()))
            maps.append((ci, "Map", topy(first(a, "pResource")), topy(first(a, "MapType")) or "?"))
        elif n.endswith("::Unmap"):
            pass
        elif n.endswith("::UpdateSubresource") or n.endswith("::UpdateSubresource1"):
            maps.append((ci, "Update", topy(first(a, "pDstResource")), "UPDATE"))
        elif n.endswith("::CopySubresourceRegion") or n.endswith("::CopyResource") or n.endswith("::CopySubresourceRegion1"):
            maps.append((ci, "Copy", topy(first(a, "pDstResource")), "COPY"))
        kind = None
        if n.endswith("::DrawIndexed"): kind = "DI"
        elif n.endswith("::DrawIndexedInstanced"): kind = "DII"
        elif n.endswith("::DrawInstanced"): kind = "DInst"
        elif n.endswith("::Draw"): kind = "D"
        elif n.endswith("Indirect"): kind = "Indirect"
        elif n.endswith("::DrawAuto"): kind = "Auto"
        elif n.endswith("::ClearDepthStencilView"): kind = "CLRDS"
        elif n.endswith("::DiscardView") or n.endswith("::DiscardView1") or n.endswith("::DiscardResource"): kind = "DISCARD"
        if kind is None:
            continue
        e = dict(ci=ci, kind=kind, dss=st["dss"], ref=st["ref"], rs=st["rs"], dsv=st["dsv"], rtvs=list(st["rtvs"]),
                 ib=st["ib"], vbs=dict(st["vbs"]), layout=st["layout"], topo=st["topo"], vs=st["vs"], ps=st["ps"],
                 gs=st["gs"], hs=st["hs"], ds=st["ds"], so=list(st["so"]), vscb=dict(st["vscb"]), vssrv=dict(st["vssrv"]),
                 vssmp=dict(st["vssmp"]), vps=list(st["vps"]), sc=list(st["sc"]))
        if kind == "DI":
            e.update(ic=topy(a["IndexCount"]), si=topy(a["StartIndexLocation"]), bv=topy(a["BaseVertexLocation"]), inst=1, sinst=0)
        elif kind == "DII":
            e.update(ic=topy(a["IndexCountPerInstance"]), si=topy(a["StartIndexLocation"]), bv=topy(a["BaseVertexLocation"]),
                     inst=topy(a["InstanceCount"]), sinst=topy(a["StartInstanceLocation"]))
        elif kind == "CLRDS":
            e.update(cdsv=topy(a["pDepthStencilView"]))
        elif kind == "DISCARD":
            e.update(view=topy(first(a, "pResourceView", "pResource")))
        events.append(e)

    # ---- scene depth (g) ------------------------------------------------------------------------------------
    cands = set()
    for v, (t, d) in dsv.items():
        td = tex.get(t)
        if td and any(str(td.get("Format", "")).endswith(x) for x in DEPTH_FMTS_S):
            cands.add(t)
    scene = max(cands, key=lambda t: tex[t]["Width"] * tex[t]["Height"]) if cands else 0
    sd = tex.get(scene, {})
    P("\n== (g) scene depth texture R%d %dx%d Format=%s BindFlags=%s MipLevels=%s ArraySize=%s SampleDesc=%s Usage=%s MiscFlags=%s"
      % (scene, sd.get("Width", 0), sd.get("Height", 0), short(sd.get("Format")), sd.get("BindFlags"), sd.get("MipLevels"),
         sd.get("ArraySize"), sd.get("SampleDesc"), sd.get("Usage"), sd.get("MiscFlags")))
    for v, (t, d) in dsv.items():
        if t == scene: P("   game DSV R%d desc %s" % (v, d))
    def tex_of_dsv(v): return dsv.get(v, (0, None))[0]
    def tex_of_rtv(v): return rtv.get(v, (0, None))[0]

    # ---- G-buffer segments: draws with 4 RTVs + scene DSV; count binds and interruptions -------------------
    gb = []             # G-buffer draws (all kinds)
    seg = 0; inG = False; segs = []
    first_g = last_g = None
    for e in events:
        if e["kind"] == "OM":
            g = len(e["rtvs"]) == 4 and tex_of_dsv(e["dsv"]) == scene
            if g and not inG: seg += 1; segs.append([e["ci"], None, 0])
            if not g and inG and segs: segs[-1][1] = e["ci"]
            inG = g
            continue
        if e["kind"] in ("DI", "DII", "D", "DInst", "Indirect", "Auto") and len(e["rtvs"]) == 4 and tex_of_dsv(e["dsv"]) == scene:
            # main G-buffer only: RT0 at scene size
            t0 = tex.get(tex_of_rtv(e["rtvs"][0]), {})
            if t0.get("Width") != sd.get("Width"): continue
            gb.append(e); segs[-1][2] += 1 if segs else 0
            if first_g is None: first_g = e["ci"]
            last_g = e["ci"]
    P("\n== main G-buffer: %d draws, kinds %s; G-buffer bind segments %s (start chunk, end chunk, draws)"
      % (len(gb), dict(collections.Counter(e["kind"] for e in gb)), [s for s in segs if s[2]]))
    def layer(e):
        vp = e["vps"][0] if e["vps"] else None
        return "cabin" if (vp and vp["MinDepth"] >= 0.85) else ("sky" if (vp and vp["MaxDepth"] < 0.01) else "world")
    P("   layers %s" % dict(collections.Counter((layer(e), e["kind"]) for e in gb)))

    # ---- (a) VS constant buffers / SRVs / samplers ----------------------------------------------------------
    P("\n== (a) VS constant-buffer slots used by G-buffer draws (slot: draws, distinct buffers, set via *1, buffer sizes / usage)")
    slotc = collections.defaultdict(list)
    for e in gb:
        for s, (b, fc, nc, one) in e["vscb"].items():
            if b: slotc[s].append((b, fc, nc, one))
    for s in sorted(slotc):
        L = slotc[s]
        bs = collections.Counter(x[0] for x in L)
        desc = ["R%d %s %dB %s x%d" % (b, short(buf.get(b, {}).get("Usage")), buf.get(b, {}).get("ByteWidth", 0),
                                         short(str(buf.get(b, {}).get("CPUAccessFlags"))), c) for b, c in bs.most_common(6)]
        P("   slot %d: %d draws, %d buffers, via VSSetConstantBuffers1 %d, window sizes %s | %s"
          % (s, len(L), len(bs), sum(1 for x in L if x[3]), dict(collections.Counter(x[2] for x in L).most_common(6)), desc))
    P("   NOTE: the state above is what is BOUND; whether the VS reads a slot is in the reflection section below.")
    srvc = collections.Counter(); smpc = collections.Counter()
    for e in gb:
        for s, v in e["vssrv"].items():
            if v: srvc[(s, short(str((srv.get(v, (0, None))[1] or {}).get("Format"))), short(str((srv.get(v, (0, None))[1] or {}).get("ViewDimension"))))] += 1
        for s, v in e["vssmp"].items():
            if v: smpc[s] += 1
    P("   VS SRVs bound (slot, format, dim): draws %s" % dict(srvc.most_common(12)))
    P("   VS samplers bound (slot: draws) %s" % dict(smpc))

    # buffers read by the G-buffer draws (VS cbs, VBs, IB) and what writes them inside the G-buffer pass span
    used = collections.defaultdict(set)
    for e in gb:
        for s, (b, fc, nc, one) in e["vscb"].items():
            if b: used[b].add("vscb%d" % s)
        for s, (b, stride, off) in e["vbs"].items():
            if b: used[b].add("vb%d" % s)
        if e["ib"][0]: used[e["ib"][0]].add("ib")
    if first_g is not None:
        inpass = [m for m in maps if first_g <= m[0] <= last_g]
        P("\n== (a) writes inside the G-buffer span (chunks %d..%d): %d Map/Update/Copy; by type %s"
          % (first_g, last_g, len(inpass), dict(collections.Counter((m[1], m[3]) for m in inpass))))
        wr = collections.Counter((m[2], m[3]) for m in inpass if m[2] in used)
        for (b, t), c in wr.most_common(20):
            P("   buffer R%d (%s, %dB, %s) written %d x %s inside the pass" % (b, ",".join(sorted(used[b])), buf.get(b, {}).get("ByteWidth", 0),
              short(buf.get(b, {}).get("Usage")), c, t))
        if not wr: P("   none of the buffers the G-buffer draws read is written inside the pass")
        # discards of used buffers BEFORE the pass in this frame (per-frame rings)
        pre = collections.Counter((m[2], m[3]) for m in maps if m[0] < first_g and m[2] in used)
        P("   writes of those buffers earlier in the frame: %s" % {("R%d" % b, t): c for (b, t), c in pre.most_common(12)})

    # ---- (b) GS / HS / DS / SO ------------------------------------------------------------------------------
    P("\n== (b) G-buffer draws with a GS %d, HS %d, DS %d, stream-out targets %d"
      % (sum(1 for e in gb if e["gs"]), sum(1 for e in gb if e["hs"]), sum(1 for e in gb if e["ds"]), sum(1 for e in gb if e["so"])))
    P("   topologies %s" % dict(collections.Counter(e["topo"] for e in gb)))
    P("   input layouts %d distinct, VSs %d, PSs %d" % (len(set(e["layout"] for e in gb)), len(set(e["vs"] for e in gb)), len(set(e["ps"] for e in gb))))

    # ---- (c) RS states ----------------------------------------------------------------------------------------
    P("\n== (c) rasterizer states of G-buffer draws")
    for s, c in collections.Counter(e["rs"] for e in gb).most_common():
        d = rss.get(s)
        P("   RS R%d x%d %s" % (s, c, {k: (short(str(v)) if isinstance(v, str) else v) for k, v in (d or {}).items()} if d else "(null = default: cull BACK, depth clip on, no scissor)"))

    # ---- (d) DSS ----------------------------------------------------------------------------------------------
    P("\n== (d) depth-stencil states of G-buffer draws")
    for (s, ref), c in collections.Counter((e["dss"], e["ref"]) for e in gb).most_common():
        d = dss.get(s) or {}
        P("   DSS R%d ref %d x%d depth=%s write=%s func=%s stencil=%s wmask=0x%02x" % (s, ref, c, d.get("DepthEnable"),
          short(str(d.get("DepthWriteMask"))), short(str(d.get("DepthFunc"))), d.get("StencilEnable"), d.get("StencilWriteMask") or 0))

    # ---- (e) viewports / scissor -------------------------------------------------------------------------------
    P("\n== (e) viewport sets of G-buffer draws (count, vp0)")
    vpc = collections.Counter((len(e["vps"]), tuple(round(e["vps"][0][k], 4) for k in ("TopLeftX", "TopLeftY", "Width", "Height", "MinDepth", "MaxDepth")) if e["vps"] else None) for e in gb)
    for k, c in vpc.most_common(): P("   %s x%d" % (k, c))
    scs = collections.Counter(tuple(tuple(x.values()) if isinstance(x, dict) else x for x in e["sc"]) for e in gb)
    P("   scissor rect sets %s" % dict(scs.most_common(5)))

    # ---- (f) instanced draws ------------------------------------------------------------------------------------
    dii = [e for e in gb if e["kind"] == "DII"]
    P("\n== (f) DrawIndexedInstanced: %d (layers %s)" % (len(dii), dict(collections.Counter(layer(e) for e in dii))))
    P("   instance counts %s" % dict(collections.Counter(e["inst"] for e in dii).most_common(12)))
    P("   index counts %s" % dict(collections.Counter(e["ic"] for e in dii).most_common(12)))
    P("   VB slots bound %s" % dict(collections.Counter(tuple(sorted(s for s, v in e["vbs"].items() if v[0])) for e in dii)))
    ilc = collections.Counter(e["layout"] for e in dii)
    for lid, c in ilc.most_common(6):
        els = il.get(lid, [])
        P("   layout R%d x%d: %s" % (lid, c, [(x.get("SemanticName"), x.get("SemanticIndex"), short(str(x.get("Format"))), x.get("InputSlot"),
                                              short(str(x.get("InputSlotClass"))), x.get("InstanceDataStepRate")) for x in els]))
    P("   VS of instanced draws %d distinct; PS %d distinct; cb0 window sizes %s" % (len(set(e["vs"] for e in dii)), len(set(e["ps"] for e in dii)),
      dict(collections.Counter(e["vscb"].get(0, (0, 0, 0, 0))[2] for e in dii))))
    di = [e for e in gb if e["kind"] == "DI"]
    P("   (non-instanced DrawIndexed: %d; layouts with per-instance elements among them: %d)"
      % (len(di), sum(1 for e in di if any("INSTANCE" in str(x.get("InputSlotClass", "")) for x in il.get(e["layout"], [])))))

    # ---- other scene-depth users after the G-buffer, discards -----------------------------------------------------
    disc = [e for e in events if e["kind"] == "DISCARD" and tex_of_dsv(e["view"]) == scene]
    P("\n== scene depth DiscardView: %d at chunks %s (G-buffer ends at chunk %s)" % (len(disc), [e["ci"] for e in disc], last_g))

    # ---- replay: shader reflection per distinct VS / PS -----------------------------------------------------------
    if REPLAY and gb:
        acts = {}
        def walk(al):
            for c in al:
                acts[c.eventId] = c
                if c.children: walk(c.children)
        walk(ctl.GetRootActions())
        by_chunk = {}
        for eid, ac in acts.items():
            for ev in ac.events:
                by_chunk[ev.chunkIndex] = eid
        seen_vs = {}; seen_ps = {}
        for e in gb:
            if e["vs"] not in seen_vs: seen_vs[e["vs"]] = e
            if e["ps"] not in seen_ps: seen_ps[e["ps"]] = e
        P("\n== reflection: %d distinct VS, %d distinct PS in the G-buffer" % (len(seen_vs), len(seen_ps)))
        vs_cb_use = collections.Counter(); vs_srv_use = collections.Counter(); ps_depth = 0; ps_cov = 0; ps_discard = 0
        vs_out_pos_only = 0
        for sid, e in list(seen_vs.items()) + [(None, None)] + list(seen_ps.items()):
            if e is None: continue
            eid = by_chunk.get(e["ci"])
            if eid is None: continue
            try:
                ctl.SetFrameEvent(eid, True)
                d3 = ctl.GetD3D11PipelineState()
                is_vs = (e is seen_vs.get(sid)) and sid == e["vs"]
                if sid == e["vs"] and seen_vs.get(sid) is e:
                    refl = d3.vertexShader.reflection
                    if refl:
                        cbs = [cb.bindPoint if hasattr(cb, "bindPoint") else -1 for cb in refl.constantBlocks]
                        nsrv = len(refl.readOnlyResources)
                        outs = [(o.semanticName, o.semanticIndex) for o in refl.outputSignature]
                        ins = [(o.semanticName, o.semanticIndex) for o in refl.inputSignature]
                        vs_cb_use[len(refl.constantBlocks)] += 1
                        vs_srv_use[nsrv] += 1
                        P("   VS R%d: cbuffers %d %s, SRVs %d, samplers %d, inputs %s, outputs %s"
                          % (sid, len(refl.constantBlocks), [cb.name for cb in refl.constantBlocks], nsrv, len(refl.samplers), ins, outs))
                if sid == e["ps"] and seen_ps.get(sid) is e:
                    refl = d3.pixelShader.reflection
                    if refl:
                        outs = [(o.semanticName, o.semanticIndex, str(o.systemValue)) for o in refl.outputSignature]
                        dep = any("Depth" in str(o.systemValue) for o in refl.outputSignature)
                        cov = any("Coverage" in str(o.systemValue) for o in refl.outputSignature)
                        ps_depth += 1 if dep else 0; ps_cov += 1 if cov else 0
                        P("   PS R%d: outputs %s%s%s" % (sid, outs, "  <-- WRITES DEPTH" if dep else "", "  <-- COVERAGE" if cov else ""))
            except Exception:
                P("   EID %s: %s" % (eid, traceback.format_exc().splitlines()[-1]))
        P("   VS cbuffer-count histogram %s, VS SRV-count histogram %s; PS writing SV_Depth* %d, SV_Coverage %d"
          % (dict(vs_cb_use), dict(vs_srv_use), ps_depth, ps_cov))

    ctl.Shutdown(); cap.Shutdown()
    P("\ndone")
except Exception:
    P(traceback.format_exc())
f.close()
os._exit(0)
